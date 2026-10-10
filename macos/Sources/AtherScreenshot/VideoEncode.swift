import AVFoundation
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

// Saving: the timeline is cut into a few back-to-back parts on frame boundaries, each part is read back through
// the frame renderer (AVAssetReaderVideoCompositionOutput) and written on its own at the same time, and the parts
// are joined. An MP4's parts are HEVC files whose samples are copied into one without encoding again; a GIF's are
// ImageIO GIFs whose frames are copied into one.
extension VideoExport {
    // Developer hook for the bench (VideoBench.swift): every frame the compositor renders for an export (it gets
    // it from the renderer box), and notes on how the export ran.
    final class Probe {
        private let lock = NSLock()
        private(set) var frames = 0
        private(set) var notes: [String] = []
        var tap: ((Int, CVPixelBuffer) -> Void)?   // output frame index, the rendered frame; from any thread

        func frame(_ index: Int, _ buffer: CVPixelBuffer) {
            lock.lock(); frames += 1; lock.unlock()
            tap?(index, buffer)
        }
        func note(_ s: String) { lock.lock(); notes.append(s); lock.unlock() }
    }

    // `count` back-to-back runs of `frames` frames.
    static func split(_ frames: Int, into count: Int) -> [Range<Int>] {
        let count = max(1, min(count, frames))
        return (0..<count).map { k in k * frames / count ..< (k + 1) * frames / count }
    }

    // `body` for parts 0..<count at once; the results in order. The first error cancels the others.
    private static func inParallel<T: Sendable>(_ count: Int, _ body: @escaping @Sendable (Int) async throws -> T) async throws -> [T] {
        try await withThrowingTaskGroup(of: (Int, T).self) { g in
            for k in 0..<count { g.addTask { (k, try await body(k)) } }
            var out = [T?](repeating: nil, count: count)
            for try await (k, v) in g { out[k] = v }
            return out.map { $0! }
        }
    }

    // The composition's frames from `start` to `end` (nil: to the very end, which can include a frame right at it).
    private static func frameReader(_ comp: AVComposition, _ vc: AVVideoComposition, from start: CMTime, to end: CMTime?) throws
        -> (AVAssetReader, AVAssetReaderVideoCompositionOutput) {
        let reader = try AVAssetReader(asset: comp)
        if start != .zero || end != nil { reader.timeRange = CMTimeRange(start: start, end: end ?? .positiveInfinity) }
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: comp.tracks(withMediaType: .video), videoSettings: nil)
        vo.videoComposition = vc
        vo.alwaysCopiesSampleData = false
        reader.add(vo)
        return (reader, vo)
    }
}

// MARK: - MP4

extension VideoExport {
    static func mp4(_ e: VideoEdit, to url: URL, encoders: Int? = nil, probe: Probe? = nil) async throws {
        let p = try await prepare(e, probe: probe)
        p.video.frameDuration = mp4FrameDuration(p.video.frameDuration, size: p.size)
        let frames = Int((p.composition.duration.seconds / p.video.frameDuration.seconds).rounded(.up))
        let parts = split(frames, into: encoders ?? self.encoders(for: p.composition.duration.seconds))
        let sound = await audio(p, speed: e.speed)
        probe?.note(parts.count == 1 ? "writer 1 piece" : "writer \(parts.count) pieces")
        if parts.count == 1 { return try await encode(p, parts[0], of: frames, to: url, final: true, sound: sound) }

        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-pieces-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let files = parts.indices.map { dir.appendingPathComponent("\($0).mp4") }
        _ = try await inParallel(parts.count) { k in try await encode(p, parts[k], of: frames, to: files[k], final: false, sound: nil) }
        try await join(files, at: parts.map { p.time(ofFrame: $0.lowerBound) }, sound: sound, from: p.composition, end: p.composition.duration, to: url)
    }

    // The frame rates the HighestQuality preset saved at: at most H.264 level 5.1's macroblock rate (983,040 a
    // second), so 3K at 60 fps saves at 30 while 1440p60 stays 60. Smoother would mean twice the frames to encode.
    static func mp4FrameDuration(_ fd: CMTime, size: CGSize) -> CMTime {
        let blocks = ceil(size.width / 16) * ceil(size.height / 16)
        let k = Int32(max(1, ceil(blocks / fd.seconds / 983_040 - 1e-9)))
        return k > 1 ? CMTimeMultiply(fd, multiplier: k) : fd
    }

    // How many encoders work on one MP4 at once: two HEVC sessions beat one on Apple silicon, a third doesn't.
    // Under ~5 s, splitting and joining cost what they save.
    static func encoders(for seconds: Double) -> Int { seconds >= 5 ? 2 : 1 }

    // HEVC at a constant quality with the encoder told to favour speed (without that, HEVC is no faster than H.264,
    // which the media engine can't run much faster than the old export did). Quality 0.85 keeps the frames at least
    // as close to what was rendered as the old HighestQuality H.264 files; the size follows the content.
    // No B-frames: with them each part starts with its own decode delay, and the joined file gets an empty edit at
    // every cut.
    static func videoSettings(_ size: CGSize, fps: Double) -> [String: Any] {
        [AVVideoCodecKey: AVVideoCodecType.hevc, AVVideoWidthKey: Int(size.width), AVVideoHeightKey: Int(size.height),
         AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2, AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                                     AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2],
         AVVideoCompressionPropertiesKey: [AVVideoProfileLevelKey: kVTProfileLevel_HEVC_Main_AutoLevel as String,
                                           kVTCompressionPropertyKey_Quality as String: 0.85,
                                           kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality as String: true,
                                           AVVideoAllowFrameReorderingKey: false,
                                           AVVideoExpectedSourceFrameRateKey: fps] as [String: Any]]
    }

    // One part of the picture rendered and encoded into `url`, starting at 0; with `sound`, the sound too (then the
    // part is the whole video: one pass).
    private static func encode(_ p: Prepared, _ part: Range<Int>, of frames: Int, to url: URL, final: Bool, sound: Sound?) async throws {
        let last = part.upperBound == frames
        let start = p.time(ofFrame: part.lowerBound), end = last ? p.composition.duration : p.time(ofFrame: part.upperBound)
        precondition(sound == nil || (part.lowerBound == 0 && last), "sound goes with the whole video only")
        let (reader, vo) = try frameReader(p.composition, p.video, from: start, to: last ? nil : end)
        let writer = try makeWriter(url, final: final)
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings(p.size, fps: 1 / p.video.frameDuration.seconds))
        vi.expectsMediaDataInRealTime = false
        vi.mediaTimeScale = VideoSequence.timeScale
        writer.add(vi)
        let back = CMTimeSubtract(.zero, start)
        var streams = [Stream(vi) { try vo.copyNextSampleBuffer().map { try shifted($0, by: back) } }]
        if let sound {
            reader.add(sound.output)
            writer.add(sound.input)
            streams.append(Stream(sound.input) { sound.output.copyNextSampleBuffer() })
        }
        try await run([reader], writer, end: CMTimeSubtract(end, start), streams)
    }

    // The parts' samples one after another, each moved to where its part starts, copied into one MP4 with the
    // sound. The sound is read from the composition here, not written as a part: copied from a file of its own,
    // AAC loses its priming trim and plays ~44 ms late.
    private static func join(_ files: [URL], at starts: [CMTime], sound: Sound?, from comp: AVComposition, end: CMTime, to url: URL) async throws {
        var readers: [AVAssetReader] = [], outputs: [AVAssetReaderTrackOutput] = []
        var format: CMFormatDescription?
        for f in files {
            let asset = AVURLAsset(url: f)
            guard let t = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.failed("A piece of the export is missing.") }
            if format == nil { format = try await t.load(.formatDescriptions).first }
            let r = try AVAssetReader(asset: asset)
            let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
            o.alwaysCopiesSampleData = false
            r.add(o)
            readers.append(r)
            outputs.append(o)
        }
        let writer = try makeWriter(url, final: true)
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: format)
        vi.expectsMediaDataInRealTime = false
        vi.mediaTimeScale = VideoSequence.timeScale
        writer.add(vi)
        var k = 0
        var streams = [Stream(vi) {
            while k < outputs.count {
                if let s = outputs[k].copyNextSampleBuffer() { return try shifted(s, by: starts[k]) }
                k += 1
            }
            return nil
        }]
        if let sound {
            let r = try AVAssetReader(asset: comp)
            r.add(sound.output)
            readers.append(r)
            writer.add(sound.input)
            streams.append(Stream(sound.input) { sound.output.copyNextSampleBuffer() })
        }
        try await run(readers, writer, end: end, streams)
    }

    // `final`: the file people get, with its index at the front; parts are copied again anyway. Times are kept at
    // the composition's timescale (edit lists too), so a part ends exactly where the next starts and frames land
    // where one pass puts them.
    private static func makeWriter(_ url: URL, final: Bool) throws -> AVAssetWriter {
        let w = try AVAssetWriter(outputURL: url, fileType: .mp4)
        w.movieTimeScale = VideoSequence.timeScale
        w.shouldOptimizeForNetworkUse = final
        return w
    }

    private struct Sound {
        let output: AVAssetReaderOutput
        let input: AVAssetWriterInput
    }

    // The composition's sound as written to the MP4: copied as it is when nothing changes it (one AAC source at
    // normal speed, like the export session did), otherwise mixed, sped up keeping its pitch, and encoded as AAC.
    private static func audio(_ p: Prepared, speed: Double) async -> Sound? {
        let tracks = p.composition.tracks(withMediaType: .audio)
        guard !tracks.isEmpty else { return nil }
        if speed == 1, tracks.count == 1, let t = tracks.first {
            let descs = (try? await t.load(.formatDescriptions)) ?? []
            if descs.count == 1, CMFormatDescriptionGetMediaSubType(descs[0]) == kAudioFormatMPEG4AAC {
                let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
                o.alwaysCopiesSampleData = false
                let i = AVAssetWriterInput(mediaType: .audio, outputSettings: nil, sourceFormatHint: descs[0])
                i.expectsMediaDataInRealTime = false
                return Sound(output: o, input: i)
            }
        }
        let o = AVAssetReaderAudioMixOutput(audioTracks: tracks, audioSettings: [AVFormatIDKey: kAudioFormatLinearPCM])
        o.audioTimePitchAlgorithm = .spectral   // sped-up audio keeps its pitch
        o.alwaysCopiesSampleData = false
        var rate = 48000.0, channels = 2
        if let d = ((try? await tracks[0].load(.formatDescriptions)) ?? []).first, let asbd = CMAudioFormatDescriptionGetStreamBasicDescription(d)?.pointee {
            rate = asbd.mSampleRate > 0 ? asbd.mSampleRate : rate
            channels = Int(min(2, max(1, asbd.mChannelsPerFrame)))
        }
        let i = AVAssetWriterInput(mediaType: .audio, outputSettings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: rate, AVNumberOfChannelsKey: channels,
                                                                       AVEncoderBitRateKey: channels * 96000])
        i.expectsMediaDataInRealTime = false
        return Sound(output: o, input: i)
    }

    // The same sample, `by` later (picture and decode times).
    static func shifted(_ s: CMSampleBuffer, by t: CMTime) throws -> CMSampleBuffer {
        guard t != .zero else { return s }
        var n: CMItemCount = 0
        CMSampleBufferGetSampleTimingInfoArray(s, entryCount: 0, arrayToFill: nil, entriesNeededOut: &n)
        var timing = [CMSampleTimingInfo](repeating: CMSampleTimingInfo(), count: n)
        CMSampleBufferGetSampleTimingInfoArray(s, entryCount: n, arrayToFill: &timing, entriesNeededOut: &n)
        for i in timing.indices {
            if timing[i].presentationTimeStamp.isValid { timing[i].presentationTimeStamp = CMTimeAdd(timing[i].presentationTimeStamp, t) }
            if timing[i].decodeTimeStamp.isValid { timing[i].decodeTimeStamp = CMTimeAdd(timing[i].decodeTimeStamp, t) }
        }
        var out: CMSampleBuffer?
        guard CMSampleBufferCreateCopyWithNewTiming(allocator: nil, sampleBuffer: s, sampleTimingEntryCount: n, sampleTimingArray: &timing,
                                                    sampleBufferOut: &out) == noErr, let out else { throw Failure.failed("Can't move a frame of the export.") }
        return out
    }

    // What goes into one writer input: the next sample, or nil when there are no more.
    private struct Stream {
        let input: AVAssetWriterInput
        let next: () throws -> CMSampleBuffer?
        init(_ input: AVAssetWriterInput, _ next: @escaping () throws -> CMSampleBuffer?) { self.input = input; self.next = next }
    }

    // Runs readers into a writer: every stream is pumped on its own queue, then the file is closed at `end`.
    // Cancelling stops the reading and the pumps; the file is thrown away.
    private static func run(_ readers: [AVAssetReader], _ writer: AVAssetWriter, end: CMTime, _ streams: [Stream]) async throws {
        for r in readers {
            guard r.startReading() else { throw Failure.failed(r.error?.localizedDescription ?? "Can't read this video.") }
        }
        guard writer.startWriting() else { throw Failure.failed(writer.error?.localizedDescription ?? "Can't write the video.") }
        writer.startSession(atSourceTime: .zero)
        let pumps = streams.map { _ in Pump() }
        let ok = await withTaskCancellationHandler {
            await withTaskGroup(of: Bool.self) { g in
                for (n, s) in streams.enumerated() { g.addTask { await pumps[n].run(s, on: DispatchQueue(label: "ather.export.\(n)")) } }
                return await g.reduce(true) { $0 && $1 }
            }
        } onCancel: {
            for p in pumps { p.finish(false) }
            for r in readers { r.cancelReading() }
        }
        let failed = readers.first { $0.status == .failed }
        if !ok || failed != nil || writer.status == .failed || Task.isCancelled {
            for r in readers { r.cancelReading() }
            writer.cancelWriting()
            try Task.checkCancellation()
            if let e = pumps.lazy.compactMap(\.error).first { throw e }
            throw Failure.failed((writer.error ?? failed?.error)?.localizedDescription ?? "Export failed.")
        }
        writer.endSession(atSourceTime: end)
        await writer.finishWriting()
        if writer.status != .completed { throw Failure.failed(writer.error?.localizedDescription ?? "Export failed.") }
    }

    // Copies a stream's samples into its input until they run out (true), or the input stops taking them, the
    // stream fails or it's stopped (false). A cancelled writer stops asking for data, so `finish` is what ends the
    // wait then.
    private final class Pump {
        private let lock = NSLock()
        private var k: CheckedContinuation<Bool, Never>?
        private var result: Bool?
        private(set) var error: Error?

        func run(_ s: Stream, on queue: DispatchQueue) async -> Bool {
            await withCheckedContinuation { (k: CheckedContinuation<Bool, Never>) in
                lock.lock()
                if let r = result { lock.unlock(); return k.resume(returning: r) }   // stopped before it started
                self.k = k
                lock.unlock()
                s.input.requestMediaDataWhenReady(on: queue) { [self] in
                    while s.input.isReadyForMoreMediaData, !done {
                        do {
                            guard let b = try s.next() else {
                                s.input.markAsFinished()
                                return finish(true)
                            }
                            if !s.input.append(b) { return finish(false) }
                        } catch {
                            self.error = error
                            return finish(false)
                        }
                    }
                }
            }
        }

        private var done: Bool { lock.lock(); defer { lock.unlock() }; return result != nil }

        func finish(_ ok: Bool) {
            lock.lock()
            guard result == nil else { return lock.unlock() }
            result = ok
            let k = self.k
            self.k = nil
            lock.unlock()
            k?.resume(returning: ok)
        }
    }
}

// MARK: - GIF

extension VideoExport {
    // `parts`: how many ImageIO encodes at once (nil: by length and cores).
    static func gif(_ e: VideoEdit, to url: URL, fps: Double = 12, parts count: Int? = nil, probe: Probe? = nil) async throws {
        let p = try await prepare(e, probe: probe)
        let vc = p.video.mutableCopy() as! AVMutableVideoComposition
        vc.frameDuration = CMTime(seconds: 1 / fps, preferredTimescale: 600)
        vc.renderSize = gifSize(p.size)
        // As many frames as the saved MP4 is long (whole frames of it), like before.
        let fd = mp4FrameDuration(p.video.frameDuration, size: p.size).seconds
        let n = max(1, Int((p.composition.duration.seconds / fd).rounded(.up) * fd * fps + 1e-6))
        let parts = split(n, into: count ?? gifParts(n))
        probe?.note("gif \(parts.count) parts")
        let gifs = try await inParallel(parts.count) { k in try await gifPart(p.composition, vc, parts[k], last: k == parts.count - 1, fps: fps) }
        if gifs.count == 1 { return try gifs[0].write(to: url) }
        guard let gif = joinGIFs(gifs) else {   // not what ImageIO usually writes: one part after all
            probe?.note("join failed, 1 part")
            return try await self.gif(e, to: url, fps: fps, parts: 1, probe: probe)
        }
        try gif.write(to: url)
    }

    // ImageIO picks a GIF's palette and compresses it at finalize, on one thread (~5.6 ms a frame at 960 px), and
    // the source decodes faster split up too: one part per 24 frames, at most half the cores.
    static func gifParts(_ frames: Int) -> Int { max(1, min(ProcessInfo.processInfo.activeProcessorCount / 2, frames / 24)) }

    static func gifSize(_ s: CGSize, max m: CGFloat = 960) -> CGSize {
        let k = min(1, m / s.width, m / s.height)
        return CGSize(width: max(1, (s.width * k).rounded(.down)), height: max(1, (s.height * k).rounded(.down)))
    }

    // One part of a GIF: its frames rendered and handed to ImageIO in order. Each frame shows until the next one; a
    // frame the compositor skipped (nothing new) lengthens the one before, and the first covers the part's start.
    private static func gifPart(_ comp: AVComposition, _ vc: AVVideoComposition, _ part: Range<Int>, last: Bool, fps: Double) async throws -> Data {
        let at = { (i: Int) in CMTimeMultiply(vc.frameDuration, multiplier: Int32(i)) }
        let (reader, vo) = try frameReader(comp, vc, from: at(part.lowerBound), to: last ? nil : at(part.upperBound))
        let data = CFDataCreateMutable(nil, 0)!
        guard let dest = CGImageDestinationCreateWithData(data, UTType.gif.identifier as CFString, part.count, nil) else { throw Failure.failed("Can't write the GIF.") }
        CGImageDestinationSetProperties(dest, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        func add(_ img: CGImage, frames: Int) {
            CGImageDestinationAddImage(dest, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: Double(frames) / fps]] as CFDictionary)
        }
        guard reader.startReading() else { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        defer { reader.cancelReading() }
        var pending: (Int, CGImage)?
        while let s = vo.copyNextSampleBuffer() {
            try Task.checkCancellation()
            guard let pb = CMSampleBufferGetImageBuffer(s), let img = cgImage(pb) else { continue }
            let i = Int((CMSampleBufferGetPresentationTimeStamp(s).seconds * fps).rounded())
            guard i < part.upperBound else { break }
            if let (j, prev) = pending {
                guard i > j else { pending = (j, img); continue }
                add(prev, frames: i - j)
            }
            pending = (pending == nil ? part.lowerBound : i, img)
        }
        if reader.status == .failed { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        if let (j, prev) = pending { add(prev, frames: max(1, part.upperBound - j)) }
        guard CGImageDestinationFinalize(dest) else { throw Failure.failed("Can't write the GIF.") }
        return data as Data
    }

    // GIFs from ImageIO joined into one: the first as it is, the frames of the others after it, each with its GIF's
    // palette as a local color table. ImageIO writes a global palette and no local ones; nil if a part isn't like that.
    static func joinGIFs(_ parts: [Data]) -> Data? {
        guard var out = parts.first.map({ [UInt8]($0) }), out.last == 0x3B else { return nil }
        out.removeLast()
        for part in parts.dropFirst() {
            let d = [UInt8](part)
            guard d.count > 13, d.starts(with: Array("GIF".utf8)), d[10] & 0x80 != 0 else { return nil }
            let bits = d[10] & 7
            let table = d[13..<(13 + 3 * (2 << Int(bits)))]
            var p = table.endIndex
            func subBlocks() -> Bool {   // copies data sub-blocks up to and including the terminator
                while p < d.count {
                    let n = Int(d[p])
                    guard p + 1 + n <= d.count else { return false }
                    out += d[p..<(p + 1 + n)]
                    p += 1 + n
                    if n == 0 { return true }
                }
                return false
            }
            loop: while p < d.count {
                switch d[p] {
                case 0x21:   // extension: keep the frame's control block, drop the rest (the loop count is in the first)
                    guard p + 1 < d.count else { return nil }
                    if d[p + 1] == 0xF9 {
                        out += d[p...(p + 1)]
                        p += 2
                        guard subBlocks() else { return nil }
                    } else {
                        let mark = out.count
                        p += 2
                        guard subBlocks() else { return nil }
                        out.removeSubrange(mark...)
                    }
                case 0x2C:   // image: its own table, or this part's palette as its local one
                    guard p + 11 <= d.count else { return nil }
                    let flags = d[p + 9]
                    out += d[p..<(p + 9)]
                    if flags & 0x80 != 0 {
                        out.append(flags)
                        p += 10
                        let n = 3 * (2 << Int(flags & 7))
                        guard p + n <= d.count else { return nil }
                        out += d[p..<(p + n)]
                        p += n
                    } else {
                        out.append(0x80 | (flags & 0x40) | bits)
                        out += table
                        p += 10
                    }
                    out.append(d[p])   // LZW minimum code size
                    p += 1
                    guard subBlocks() else { return nil }
                case 0x3B: break loop
                default: return nil
                }
            }
        }
        out.append(0x3B)
        return Data(out)
    }

    // A copy of a BGRA frame as a CGImage (the buffer goes back to the pool).
    static func cgImage(_ pb: CVPixelBuffer) -> CGImage? {
        CVPixelBufferLockBaseAddress(pb, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
        guard let base = CVPixelBufferGetBaseAddress(pb),
              let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                                  bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue),
              let dst = ctx.data else { return nil }
        let src = CVPixelBufferGetBytesPerRow(pb), dr = ctx.bytesPerRow
        for y in 0..<h { memcpy(dst + y * dr, base + y * src, w * 4) }
        return ctx.makeImage()
    }
}
