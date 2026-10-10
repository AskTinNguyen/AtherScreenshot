import AVFoundation
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

// Saving an MP4: the composition is read back through the frame renderer (AVAssetReaderVideoCompositionOutput) and
// written with AVAssetWriter, so the encoder settings, the sound and the pieces are ours to choose.
extension VideoExport {
    // H.264 High at a constant quality: about the size and quality the HighestQuality preset gave, and faster.
    // No B-frames, so every frame decodes in order and pieces join without edits between them.
    static func videoSettings(_ size: CGSize, fps: Double) -> [String: Any] {
        [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: Int(size.width), AVVideoHeightKey: Int(size.height),
         AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2, AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                                     AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2],
         AVVideoCompressionPropertiesKey: [AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                                           kVTCompressionPropertyKey_Quality as String: 0.85,
                                           AVVideoAllowFrameReorderingKey: false,   // see writePieces
                                           AVVideoExpectedSourceFrameRateKey: fps] as [String: Any]]
    }

    // How many encoders work on one MP4 at once: the media engine runs two H.264 sessions faster than one
    // (M4 Max, 3K: +4%, 1080p: +17%); a third adds nothing. Under ~5 s, splitting and joining cost what it saves.
    static func encoders(for seconds: Double) -> Int {
        if let v = ProcessInfo.processInfo.environment["ATHER_ENCODERS"], let n = Int(v) { return max(1, n) }   // the bench
        return seconds >= 5 ? 2 : 1
    }

    // Copies samples from `next` to `input` until it runs out (true), or the input stops taking them or it's
    // stopped (false). A cancelled writer stops asking for data, so `stop` is what ends the wait then.
    private final class Pump {
        private let lock = NSLock()
        private var k: CheckedContinuation<Bool, Never>?
        private var result: Bool?

        func run(_ input: AVAssetWriterInput, on queue: DispatchQueue, _ next: @escaping () -> CMSampleBuffer?) async -> Bool {
            await withCheckedContinuation { (k: CheckedContinuation<Bool, Never>) in
                lock.lock()
                if let r = result { lock.unlock(); return k.resume(returning: r) }   // stopped before it started
                self.k = k
                lock.unlock()
                input.requestMediaDataWhenReady(on: queue) { [self] in
                    while input.isReadyForMoreMediaData, !done {
                        guard let s = next() else {
                            input.markAsFinished()
                            return finish(true)
                        }
                        if !input.append(s) { return finish(false) }
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

    // The same sample, `by` later (picture and decode times).
    static func shifted(_ s: CMSampleBuffer, by t: CMTime) -> CMSampleBuffer? {
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
        CMSampleBufferCreateCopyWithNewTiming(allocator: nil, sampleBuffer: s, sampleTimingEntryCount: n, sampleTimingArray: &timing, sampleBufferOut: &out)
        return out
    }

    // The composition's sound as written to the MP4: copied as it is when nothing changes it (one AAC source at
    // normal speed, like the export session did), otherwise mixed, sped up keeping its pitch, and encoded as AAC.
    private static func audioIO(_ p: Prepared, speed: Double) async -> (AVAssetReaderOutput, AVAssetWriterInput)? {
        let tracks = p.composition.tracks(withMediaType: .audio)
        guard !tracks.isEmpty else { return nil }
        if speed == 1, tracks.count == 1, let t = tracks.first {
            let descs = (try? await t.load(.formatDescriptions)) ?? []
            if descs.count == 1, CMFormatDescriptionGetMediaSubType(descs[0]) == kAudioFormatMPEG4AAC {
                let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
                o.alwaysCopiesSampleData = false
                let i = AVAssetWriterInput(mediaType: .audio, outputSettings: nil, sourceFormatHint: descs[0])
                i.expectsMediaDataInRealTime = false
                return (o, i)
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
        return (o, i)
    }

    // The composition's timescale: frame times are kept exactly, so pieces moved back to where they start land
    // where one pass puts them (at 600 they'd round differently).
    static let timeScale: CMTimeScale = 60000

    private static func videoOutput(_ p: Prepared) -> AVAssetReaderVideoCompositionOutput {
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: p.composition.tracks(withMediaType: .video), videoSettings: nil)
        vo.videoComposition = p.video
        vo.alwaysCopiesSampleData = false
        return vo
    }

    private static func videoInput(_ p: Prepared) -> AVAssetWriterInput {
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings(p.size, fps: 1 / p.video.frameDuration.seconds))
        vi.expectsMediaDataInRealTime = false
        vi.mediaTimeScale = timeScale
        return vi
    }

    // Runs a reader into a writer: each (input, next) pair is pumped on its own queue, then the file is closed at `end`.
    private static func run(_ reader: AVAssetReader?, _ writer: AVAssetWriter, end: CMTime, _ streams: [(AVAssetWriterInput, () -> CMSampleBuffer?)]) async throws {
        writer.movieTimeScale = timeScale   // the edit lists too, so a piece ends exactly where the next starts
        if let reader, !reader.startReading() { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        guard writer.startWriting() else { throw Failure.failed(writer.error?.localizedDescription ?? "Can't write the video.") }
        writer.startSession(atSourceTime: .zero)
        // Cancelling stops the reading and the writing; the pumps then run out and the file is thrown away below.
        let pumps = streams.map { _ in Pump() }
        let ok = await withTaskCancellationHandler {
            await withTaskGroup(of: Bool.self) { g in
                for (n, (input, next)) in streams.enumerated() { g.addTask { await pumps[n].run(input, on: DispatchQueue(label: "ather.export.\(n)"), next) } }
                return await g.reduce(true) { $0 && $1 }
            }
        } onCancel: {
            for p in pumps { p.finish(false) }
            reader?.cancelReading()
        }
        if !ok || reader?.status == .failed || writer.status == .failed || Task.isCancelled {
            reader?.cancelReading()
            writer.cancelWriting()
            try Task.checkCancellation()
            throw Failure.failed((writer.error ?? reader?.error)?.localizedDescription ?? "Export failed.")
        }
        writer.endSession(atSourceTime: end)
        await writer.finishWriting()
        if writer.status != .completed { throw Failure.failed(writer.error?.localizedDescription ?? "Export failed.") }
    }

    // `encoders`: how many at once (nil: by length).
    static func write(_ p: Prepared, speed: Double, encoders count: Int? = nil, to url: URL) async throws {
        let n = count ?? encoders(for: p.composition.duration.seconds)
        if n > 1 { return try await writePieces(p, speed: speed, count: n, to: url) }
        // One pass: the whole composition, picture and sound, into one MP4.
        let reader = try AVAssetReader(asset: p.composition)
        let vo = videoOutput(p)
        reader.add(vo)
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        writer.shouldOptimizeForNetworkUse = true
        let vi = videoInput(p)
        writer.add(vi)
        var streams: [(AVAssetWriterInput, () -> CMSampleBuffer?)] = [(vi, { vo.copyNextSampleBuffer() })]
        if let (ao, ai) = await audioIO(p, speed: speed) {
            reader.add(ao)
            writer.add(ai)
            streams.append((ai, { ao.copyNextSampleBuffer() }))
        }
        probe?.note("writer 1 piece")
        try await run(reader, writer, end: p.composition.duration, streams)
    }

    // Pieces: the picture is cut into `count` back-to-back parts on frame boundaries, each read through the
    // compositor and encoded by its own writer at the same time, the sound is written on its own alongside, and
    // then everything is copied into one MP4 without encoding again (each piece starts on a key frame). With B-frames
    // each piece would start with its own decode delay, and the joined file gets an empty edit at every cut.
    static func writePieces(_ p: Prepared, speed: Double, count: Int, to url: URL) async throws {
        let fd = p.video.frameDuration, total = p.composition.duration
        let frames = Int((total.seconds / fd.seconds).rounded(.up))
        let cuts: [CMTime] = (0...count).map { k in k == count ? total : CMTimeMultiply(fd, multiplier: Int32(frames * k / count)) }
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-pieces-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: dir) }
        let pieces = (0..<count).map { dir.appendingPathComponent("\($0).mp4") }
        probe?.note("writer \(count) pieces")
        try await withThrowingTaskGroup(of: Void.self) { g in
            for k in 0..<count {
                g.addTask {
                    let reader = try AVAssetReader(asset: p.composition)
                    // The last piece reads to the end, like one pass (which can include a frame right at the end).
                    reader.timeRange = CMTimeRange(start: cuts[k], end: k == count - 1 ? .positiveInfinity : cuts[k + 1])
                    let vo = videoOutput(p)
                    reader.add(vo)
                    let writer = try AVAssetWriter(outputURL: pieces[k], fileType: .mp4)
                    let vi = videoInput(p)
                    writer.add(vi)
                    let back = CMTimeSubtract(.zero, cuts[k])   // each piece starts at 0
                    try await run(reader, writer, end: CMTimeSubtract(cuts[k + 1], cuts[k]), [(vi, { vo.copyNextSampleBuffer().flatMap { shifted($0, by: back) } })])
                }
            }
            try await g.waitForAll()
        }
        try await join(pieces, at: cuts, sound: await audioIO(p, speed: speed), from: p.composition, end: total, to: url)
    }

    // The pieces' samples one after another (each moved to where it starts) copied into one MP4, with the sound.
    // The sound comes straight from the composition: written to a file of its own and copied, AAC loses its
    // priming trim and plays ~44 ms late.
    static func join(_ pieces: [URL], at cuts: [CMTime], sound: (AVAssetReaderOutput, AVAssetWriterInput)?, from comp: AVComposition,
                     end: CMTime, to url: URL) async throws {
        func track(_ u: URL, _ type: AVMediaType) async throws -> (AVAssetReader, AVAssetReaderTrackOutput, CMFormatDescription?) {
            let asset = AVURLAsset(url: u)
            guard let t = try await asset.loadTracks(withMediaType: type).first else { throw Failure.failed("A piece of the export is missing.") }
            let r = try AVAssetReader(asset: asset)
            let o = AVAssetReaderTrackOutput(track: t, outputSettings: nil)
            o.alwaysCopiesSampleData = false
            r.add(o)
            return (r, o, try await t.load(.formatDescriptions).first)
        }
        var video: [(AVAssetReader, AVAssetReaderTrackOutput, CMFormatDescription?)] = []
        for u in pieces { video.append(try await track(u, .video)) }
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        writer.shouldOptimizeForNetworkUse = true
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: video[0].2)
        vi.expectsMediaDataInRealTime = false
        vi.mediaTimeScale = timeScale
        writer.add(vi)
        for v in video where !v.0.startReading() { throw Failure.failed(v.0.error?.localizedDescription ?? "Can't read a piece of the export.") }
        var k = 0
        var streams: [(AVAssetWriterInput, () -> CMSampleBuffer?)] = [(vi, {
            while k < video.count {
                if let s = video[k].1.copyNextSampleBuffer() { return shifted(s, by: cuts[k]) }
                k += 1
            }
            return nil
        })]
        var reader: AVAssetReader?
        if let (ao, ai) = sound {
            let r = try AVAssetReader(asset: comp)
            r.add(ao)
            writer.add(ai)
            streams.append((ai, { ao.copyNextSampleBuffer() }))
            reader = r
        }
        try await run(reader, writer, end: end, streams)
    }
}

// Saving a GIF: the composition renders straight at the GIF's rate and size (≤ 960 px), one pass, no MP4 in between.
extension VideoExport {
    static func gifSize(_ s: CGSize, max m: CGFloat = 960) -> CGSize {
        let k = min(1, m / s.width, m / s.height)
        return CGSize(width: max(1, (s.width * k).rounded(.down)), height: max(1, (s.height * k).rounded(.down)))
    }

    // `chunks`: how many parts ImageIO encodes at once (nil: by length and cores).
    static func gif(_ e: VideoEdit, to url: URL, fps: Double = 12, chunks: Int? = nil) async throws {
        let p = try await prepare(e)
        let probe = VideoExport.probe
        VideoExport.probe = nil   // the bench taps the GIF's frames, not the compositor's
        defer { VideoExport.probe = probe }
        let vc = p.video.mutableCopy() as! AVMutableVideoComposition
        vc.frameDuration = CMTime(seconds: 1 / fps, preferredTimescale: 600)
        vc.renderSize = gifSize(p.size)
        // As many frames as the saved MP4 is long (whole frames of the sequence), like before.
        let fd = p.video.frameDuration.seconds
        let n = max(1, Int((p.composition.duration.seconds / fd).rounded(.up) * fd * fps + 1e-6))
        // ImageIO picks the palette and compresses at finalize, on one thread. So the frames go out in a few chunks,
        // each finalized on its own as soon as its frames are in, and the chunks are joined (joinGIFs). And they're
        // read in two halves at once, which mostly means decoding the source twice as fast.
        let k = max(1, min(chunks ?? gifChunks(n), n))
        let halves = n >= 24 && k >= 2 ? 2 : 1
        probe?.note(k == 1 ? "gif 1 pass" : "gif 1 pass, \(k) chunks, \(halves) readers")
        let edges = (0...halves).map { $0 * n / halves }
        let reads = (0..<halves).map { h in
            Task { () throws -> [Task<Data?, Never>] in
                let reader = try AVAssetReader(asset: p.composition)
                reader.timeRange = CMTimeRange(start: CMTimeMultiply(vc.frameDuration, multiplier: Int32(edges[h])),
                                               end: h == halves - 1 ? .positiveInfinity : CMTimeMultiply(vc.frameDuration, multiplier: Int32(edges[h + 1])))
                let vo = AVAssetReaderVideoCompositionOutput(videoTracks: p.composition.tracks(withMediaType: .video), videoSettings: nil)
                vo.videoComposition = vc
                vo.alwaysCopiesSampleData = false
                reader.add(vo)
                defer { reader.cancelReading() }
                guard reader.startReading() else { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
                var parts: [Task<Data?, Never>] = []
                var dest: CGImageDestination?, data: CFMutableData?, chunk = -1
                func close() {
                    guard let d = dest, let m = data else { return }
                    parts.append(Task.detached { CGImageDestinationFinalize(d) ? m as Data : nil })
                    dest = nil
                }
                func add(_ j: Int, _ img: CGImage, frames: Int) throws {
                    let c = j * k / n
                    if c != chunk || dest == nil {
                        close()
                        chunk = c
                        let m = CFDataCreateMutable(nil, 0)!
                        // At most the frames j with j * k / n == c, in this half.
                        let count = min((c + 1) * n + k - 1, edges[h + 1] * k + k - 1) / k - max(c * n + k - 1, edges[h] * k + k - 1) / k
                        guard let d = CGImageDestinationCreateWithData(m, UTType.gif.identifier as CFString, max(1, count), nil) else { throw Failure.failed("Can't write the GIF.") }
                        CGImageDestinationSetProperties(d, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
                        (dest, data) = (d, m)
                    }
                    CGImageDestinationAddImage(dest!, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: Double(frames) / fps]] as CFDictionary)
                }
                // Each frame shows until the next one; a frame the compositor skipped (nothing new) lengthens the one
                // before, and the first one of a half covers its start.
                var pending: (Int, CGImage)?
                while let s = vo.copyNextSampleBuffer() {
                    try Task.checkCancellation()
                    guard let pb = CMSampleBufferGetImageBuffer(s), let img = cgImage(pb) else { continue }
                    let i = max(edges[h], Int((CMSampleBufferGetPresentationTimeStamp(s).seconds * fps).rounded()))
                    guard i < edges[h + 1] else { break }
                    if let (j, prev) = pending {
                        guard i > j else { pending = (j, img); continue }
                        try add(j, prev, frames: i - j)
                    }
                    probe?.frame(i, img)
                    pending = (pending == nil ? edges[h] : i, img)
                }
                if reader.status == .failed { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
                if let (j, prev) = pending { try add(j, prev, frames: max(1, edges[h + 1] - j)) }
                close()
                return parts
            }
        }
        var parts: [Task<Data?, Never>] = []
        do {
            for r in reads { parts += try await r.value }
        } catch {
            for r in reads { r.cancel() }
            throw error
        }
        let t0 = Date()
        var done: [Data] = []
        for t in parts {
            guard let d = await t.value else { throw Failure.failed("Can't write the GIF.") }
            done.append(d)
        }
        probe?.note(String(format: "waited %.2f s for the last chunks", Date().timeIntervalSince(t0)))
        if k == 1 { return try done[0].write(to: url) }
        guard let gif = joinGIFs(done) else {   // not what ImageIO usually writes: one part after all
            probe?.note("join failed, 1 chunk")
            return try await self.gif(e, to: url, fps: fps, chunks: 1)
        }
        try gif.write(to: url)
    }

    static func gifChunks(_ frames: Int) -> Int {
        if let v = ProcessInfo.processInfo.environment["ATHER_GIF_CHUNKS"], let k = Int(v) { return max(1, k) }   // the bench
        return max(1, min(ProcessInfo.processInfo.activeProcessorCount / 2, frames / 24))
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
