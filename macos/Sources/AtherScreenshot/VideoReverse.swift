import AVFoundation
import VideoToolbox

// Reverse: a clip's footage written end to start (picture and sound) as a file in the temp folder, which then plays,
// trims and saves like any other clip (WriteReversed in videoio.cpp). Reversing it again goes back to the original.
enum VideoReverse {
    enum Failure: LocalizedError {
        case failed(String)
        var errorDescription: String? { if case .failed(let s) = self { return s }; return nil }
    }

    // Where `c` reversed is kept: the same footage reuses the file.
    static func path(for c: Clip) -> URL {
        let dir = FileManager.default.temporaryDirectory.appendingPathComponent("ather-reversed", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        var h: UInt64 = 1469598103934665603   // FNV-1a: the same in every run, unlike hashValue
        for b in "\(c.path.lowercased())|\(c.inPoint)|\(c.outPoint)".utf8 { h = (h ^ UInt64(b)) &* 1099511628211 }
        let name = String(c.url.deletingPathExtension().lastPathComponent.prefix(40))
        return dir.appendingPathComponent("\(name) reversed \(String(format: "%016llx", h)).mp4")
    }

    // `c` played the other way: a reversed file of its footage, or for a reversed clip the original file again.
    // Keeps its id, source, flip and speed.
    static func toggled(_ c: Clip, progress: ((Double) -> Void)? = nil) async throws -> Clip {
        var r = c
        if let orig = c.reverseOf {
            guard let o = try? await VideoSource.probe(URL(fileURLWithPath: orig)) else {
                throw Failure.failed("The original video can't be read any more: \(URL(fileURLWithPath: orig).lastPathComponent)")
            }
            r.path = orig
            r.inPoint = min(max(0, c.reverseEnd - c.outPoint), o.length)
            r.outPoint = min(max(r.inPoint, c.reverseEnd - c.inPoint), o.length)
            r.reverseOf = nil
            r.reverseEnd = 0
            (r.length, r.w, r.h, r.fps, r.hasAudio, r.rotation) = (o.length, o.w, o.h, o.fps, o.hasAudio, o.rotation)
            return r
        }
        let file = path(for: c)
        var made = try? await VideoSource.probe(file)
        if made == nil {
            let part = file.deletingPathExtension().appendingPathExtension("part.mp4")
            try? FileManager.default.removeItem(at: part)
            do {
                try await write(c, to: part, progress: progress)
                try? FileManager.default.removeItem(at: file)
                try FileManager.default.moveItem(at: part, to: file)
            } catch {
                try? FileManager.default.removeItem(at: part)
                throw error
            }
            guard let m = try? await VideoSource.probe(file) else { throw Failure.failed("Can't read the reversed video.") }
            made = m
        }
        let m = made!
        r.path = file.path
        r.inPoint = 0
        r.outPoint = min(c.duration, m.length)
        (r.length, r.w, r.h, r.fps, r.rotation) = (m.length, m.w, m.h, m.fps, m.rotation)
        r.hasAudio = c.hasAudio && m.hasAudio
        r.reverseOf = c.path
        r.reverseEnd = c.outPoint
        return r
    }

    static let rate = 48000.0

    // Writes `c`'s footage [in, out) end to start into `url`: each frame lasts as long as it did, so time r of the new
    // file shows the source's time out − r; the sound is the same stretch played backwards. Frames are decoded a block
    // at a time from the end (as many as fit in ~384 MB), each block read forwards and written backwards.
    static func write(_ c: Clip, to url: URL, progress: ((Double) -> Void)? = nil) async throws {
        let asset = AVURLAsset(url: c.url, options: [AVURLAssetPreferPreciseDurationAndTimingKey: true])
        guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.failed("Can't read this video.") }
        let (natural, transform, nominal) = try await vt.load(.naturalSize, .preferredTransform, .nominalFrameRate)
        let size = CGSize(width: max(2, natural.width.rounded()), height: max(2, natural.height.rounded()))
        let fps = c.fps > 0 ? c.fps : nominal > 0 ? Double(nominal) : 30

        // Every frame's time in the file, and the ones the clip shows: the one on screen at its in point, then the ones
        // before its out point.
        var T: [Double] = []
        do {
            let r = try AVAssetReader(asset: asset)
            let o = AVAssetReaderTrackOutput(track: vt, outputSettings: nil)
            o.alwaysCopiesSampleData = false
            r.add(o)
            guard r.startReading() else { throw Failure.failed("Can't read this video.") }
            while let s = o.copyNextSampleBuffer() {
                let t = CMSampleBufferGetOutputPresentationTimeStamp(s)
                if t.isValid, CMSampleBufferGetNumSamples(s) > 0 { T.append(t.seconds) }
            }
            T.sort()
        }
        guard !T.isEmpty else { throw Failure.failed("Can't read this video.") }
        var first = max(0, (T.firstIndex { $0 > c.inPoint + 1e-3 } ?? T.count) - 1)
        first = min(first, T.count - 1)
        var last = first
        while last + 1 < T.count && T[last + 1] < c.outPoint - 1e-4 { last += 1 }
        let n = last - first + 1
        func from(_ j: Int) -> Double { min(max(T[j], c.inPoint), c.outPoint) }
        func to(_ j: Int) -> Double { j < last ? min(T[j + 1], c.outPoint) : c.outPoint }

        // The sound, end to start (stereo float at 48 kHz).
        var pcm: [Float] = []
        if c.hasAudio, let at = try await asset.loadTracks(withMediaType: .audio).first(where: { VideoSource.decodes(asset, $0) }) {
            let r = try AVAssetReader(asset: asset)
            r.timeRange = CMTimeRange(start: CMTime(seconds: c.inPoint, preferredTimescale: 600000), end: CMTime(seconds: c.outPoint, preferredTimescale: 600000))
            let o = AVAssetReaderTrackOutput(track: at, outputSettings: [AVFormatIDKey: kAudioFormatLinearPCM, AVLinearPCMBitDepthKey: 32, AVLinearPCMIsFloatKey: true,
                                                                       AVLinearPCMIsNonInterleaved: false, AVLinearPCMIsBigEndianKey: false,
                                                                       AVSampleRateKey: rate, AVNumberOfChannelsKey: 2])
            r.add(o)
            if r.startReading() {
                while let s = o.copyNextSampleBuffer() {
                    guard let bb = CMSampleBufferGetDataBuffer(s) else { continue }
                    let bytes = CMBlockBufferGetDataLength(bb)
                    var chunk = [Float](repeating: 0, count: bytes / 4)
                    chunk.withUnsafeMutableBytes { _ = CMBlockBufferCopyDataBytes(bb, atOffset: 0, dataLength: bytes, destination: $0.baseAddress!) }
                    pcm += chunk
                }
            }
            let frames = min(pcm.count / 2, Int((c.duration * rate).rounded()))
            pcm.removeSubrange((frames * 2)...)
            var rev = [Float](repeating: 0, count: pcm.count)
            for i in 0..<frames { rev[i * 2] = pcm[(frames - 1 - i) * 2]; rev[i * 2 + 1] = pcm[(frames - 1 - i) * 2 + 1] }
            pcm = rev
        }

        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        writer.movieTimeScale = VideoSequence.timeScale
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: VideoExport.videoSettings(size, fps: fps))
        vi.expectsMediaDataInRealTime = false
        vi.mediaTimeScale = VideoSequence.timeScale
        vi.transform = transform   // stays upright the way the source is
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vi, sourcePixelBufferAttributes: nil)
        writer.add(vi)
        var ai: AVAssetWriterInput?
        if !pcm.isEmpty {
            let a = AVAssetWriterInput(mediaType: .audio, outputSettings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: rate, AVNumberOfChannelsKey: 2,
                                                                           AVEncoderBitRateKey: 192000])
            a.expectsMediaDataInRealTime = false
            writer.add(a)
            ai = a
        }
        guard writer.startWriting() else { throw Failure.failed(writer.error?.localizedDescription ?? "Can't write the reversed video.") }
        writer.startSession(atSourceTime: .zero)
        func fail(_ e: Error) -> Error { writer.cancelWriting(); return e }

        // Waits until `input` takes more; a writer that stops taking anything for 20 s has stalled.
        func ready(_ input: AVAssetWriterInput) async throws {
            var waited = 0
            while !input.isReadyForMoreMediaData {
                if writer.status == .failed { throw Failure.failed(writer.error?.localizedDescription ?? "Encoding failed.") }
                waited += 1
                if waited > 20_000 { throw Failure.failed("The encoder stopped taking frames.") }
                try await Task.sleep(nanoseconds: 1_000_000)
            }
        }

        let audioTotal = pcm.count / 2
        var audioAt = 0, audioDone = false
        // The sound up to `t`, as far as the writer takes it now (`wait`: all of it). The writer keeps its inputs within
        // about half a second of each other, so the sound goes in alongside the picture, never blocking it.
        func writeAudio(until t: Double, wait: Bool = false) async throws {
            guard let ai else { return }
            let end = t.isFinite ? min(audioTotal, Int((t * rate).rounded())) : audioTotal
            while audioAt < end {
                if wait { try await ready(ai) } else if !ai.isReadyForMoreMediaData { return }
                let k = min(4800, end - audioAt)
                guard let s = audioBuffer(pcm, from: audioAt, count: k) else { throw Failure.failed("Can't write the sound.") }
                if !ai.append(s) { throw Failure.failed(writer.error?.localizedDescription ?? "Encoding failed.") }
                audioAt += k
            }
            if audioAt >= audioTotal, !audioDone { audioDone = true; ai.markAsFinished() }   // all in: the writer stops waiting for more
        }

        let block = min(max(Int(384 << 20) / max(1, Int(size.width * size.height * 1.5)), 8), 120)
        var shown: CVPixelBuffer?   // the frame written last: stands in for one that didn't decode
        var done = 0
        var hi = last + 1
        do {
            while hi > first {
                try Task.checkCancellation()
                let lo = hi - first > block ? hi - block : first
                var pics = [CVPixelBuffer?](repeating: nil, count: hi - lo)
                let r = try AVAssetReader(asset: asset)
                r.timeRange = CMTimeRange(start: CMTime(seconds: T[lo], preferredTimescale: 600000) - CMTime(value: 1, timescale: 600000),
                                          end: CMTime(seconds: T[hi - 1], preferredTimescale: 600000) + CMTime(value: 1, timescale: 1000))
                let o = AVAssetReaderTrackOutput(track: vt, outputSettings: [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                                                                             kCVPixelBufferIOSurfacePropertiesKey as String: [String: Int]()])
                r.add(o)
                if r.startReading() {
                    while let s = o.copyNextSampleBuffer() {
                        guard let pb = CMSampleBufferGetImageBuffer(s) else { continue }
                        let t = CMSampleBufferGetOutputPresentationTimeStamp(s).seconds
                        // The frame it is: the last whose time is at or before it.
                        guard let j = (lo..<hi).last(where: { T[$0] <= t + 1e-3 }), abs(T[j] - t) < 1e-3 else { continue }
                        pics[j - lo] = copy(pb)   // the decoder's own buffers go back to it, or it stalls holding a block
                    }
                }
                for j in (lo..<hi).reversed() {
                    try Task.checkCancellation()
                    if let p = pics[j - lo] { shown = p }
                    guard let frame = shown ?? pics.compactMap({ $0 }).last else { continue }
                    let t0 = c.outPoint - to(j)
                    var waited = 0
                    while !vi.isReadyForMoreMediaData {   // the sound may be what it waits for
                        if writer.status == .failed { throw Failure.failed(writer.error?.localizedDescription ?? "Encoding failed.") }
                        try await writeAudio(until: .infinity)   // as far as the writer lets it lead (the encoder holds frames)
                        waited += 1
                        if waited > 20_000 { throw Failure.failed("The encoder stopped taking frames.") }
                        try await Task.sleep(nanoseconds: 1_000_000)
                    }
                    if !adaptor.append(frame, withPresentationTime: CMTime(seconds: t0, preferredTimescale: VideoSequence.timeScale)) {
                        throw Failure.failed(writer.error?.localizedDescription ?? "Encoding failed.")
                    }
                    try await writeAudio(until: t0 + max(1e-3, to(j) - from(j)) + 0.2)
                    done += 1
                    progress?(Double(done) / Double(n))
                }
                hi = lo
            }
            vi.markAsFinished()
            try await writeAudio(until: Double(audioTotal) / rate, wait: true)
        } catch {
            throw fail(error)
        }
        if !audioDone { ai?.markAsFinished() }
        writer.endSession(atSourceTime: CMTime(seconds: c.outPoint - from(first), preferredTimescale: VideoSequence.timeScale))
        await writer.finishWriting()
        if writer.status != .completed { throw Failure.failed(writer.error?.localizedDescription ?? "Couldn't finish the reversed video.") }
    }

    // A copy of a decoded frame (planes and all) in a buffer of its own.
    private static func copy(_ src: CVPixelBuffer) -> CVPixelBuffer? {
        var out: CVPixelBuffer?
        let attrs = [kCVPixelBufferIOSurfacePropertiesKey as String: [String: Int]()] as CFDictionary
        guard CVPixelBufferCreate(nil, CVPixelBufferGetWidth(src), CVPixelBufferGetHeight(src), CVPixelBufferGetPixelFormatType(src), attrs, &out) == kCVReturnSuccess,
              let dst = out else { return nil }
        CVBufferPropagateAttachments(src, dst)
        CVPixelBufferLockBaseAddress(src, .readOnly)
        CVPixelBufferLockBaseAddress(dst, [])
        defer { CVPixelBufferUnlockBaseAddress(dst, []); CVPixelBufferUnlockBaseAddress(src, .readOnly) }
        let planes = max(1, CVPixelBufferGetPlaneCount(src))
        for p in 0..<planes {
            let planar = CVPixelBufferIsPlanar(src)
            guard let s = planar ? CVPixelBufferGetBaseAddressOfPlane(src, p) : CVPixelBufferGetBaseAddress(src),
                  let d = planar ? CVPixelBufferGetBaseAddressOfPlane(dst, p) : CVPixelBufferGetBaseAddress(dst) else { return nil }
            let rows = planar ? CVPixelBufferGetHeightOfPlane(src, p) : CVPixelBufferGetHeight(src)
            let sr = planar ? CVPixelBufferGetBytesPerRowOfPlane(src, p) : CVPixelBufferGetBytesPerRow(src)
            let dr = planar ? CVPixelBufferGetBytesPerRowOfPlane(dst, p) : CVPixelBufferGetBytesPerRow(dst)
            for y in 0..<rows { memcpy(d + y * dr, s + y * sr, min(sr, dr)) }
        }
        return dst
    }

    // `count` stereo frames of `pcm` from frame `from`, as a sample buffer at that time.
    private static func audioBuffer(_ pcm: [Float], from: Int, count: Int) -> CMSampleBuffer? {
        var asbd = AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatLinearPCM, mFormatFlags: kLinearPCMFormatFlagIsFloat | kLinearPCMFormatFlagIsPacked,
                                               mBytesPerPacket: 8, mFramesPerPacket: 1, mBytesPerFrame: 8, mChannelsPerFrame: 2, mBitsPerChannel: 32, mReserved: 0)
        var fmt: CMAudioFormatDescription?
        guard CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil, extensions: nil,
                                             formatDescriptionOut: &fmt) == noErr, let fmt else { return nil }
        let bytes = count * 8
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: bytes, blockAllocator: nil, customBlockSource: nil, offsetToData: 0,
                                                 dataLength: bytes, flags: 0, blockBufferOut: &block) == noErr, let block else { return nil }
        let ok = pcm.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress! + from * 8, blockBuffer: block, offsetIntoDestination: 0, dataLength: bytes) == noErr }
        guard ok else { return nil }
        var s: CMSampleBuffer?
        CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: nil, dataBuffer: block, formatDescription: fmt, sampleCount: count,
                                                             presentationTimeStamp: CMTime(value: CMTimeValue(from), timescale: CMTimeScale(rate)),
                                                             packetDescriptions: nil, sampleBufferOut: &s)
        return s
    }
}
