import AVFoundation
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

// Saving an MP4: the composition is read back through the frame renderer (AVAssetReaderVideoCompositionOutput) and
// written with AVAssetWriter, so the encoder settings, the sound and the pieces are ours to choose.
extension VideoExport {
    // H.264 High at a constant quality: about the size and quality the HighestQuality preset gave, and faster.
    static func videoSettings(_ size: CGSize, fps: Double) -> [String: Any] {
        [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: Int(size.width), AVVideoHeightKey: Int(size.height),
         AVVideoColorPropertiesKey: [AVVideoColorPrimariesKey: AVVideoColorPrimaries_ITU_R_709_2, AVVideoTransferFunctionKey: AVVideoTransferFunction_ITU_R_709_2,
                                     AVVideoYCbCrMatrixKey: AVVideoYCbCrMatrix_ITU_R_709_2],
         AVVideoCompressionPropertiesKey: [AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                                           kVTCompressionPropertyKey_Quality as String: 0.85,
                                           AVVideoExpectedSourceFrameRateKey: fps] as [String: Any]]
    }

    // Copies samples from `output` to `input` until either runs out (or the task is cancelled).
    private static func pump(_ output: AVAssetReaderOutput, into input: AVAssetWriterInput, on queue: DispatchQueue,
                             each: ((CMSampleBuffer) -> Void)? = nil) async {
        await withCheckedContinuation { (k: CheckedContinuation<Void, Never>) in
            var done = false
            input.requestMediaDataWhenReady(on: queue) {
                guard !done else { return }
                while input.isReadyForMoreMediaData {
                    guard let s = output.copyNextSampleBuffer() else {
                        done = true
                        input.markAsFinished()
                        k.resume()
                        return
                    }
                    each?(s)
                    if !input.append(s) {
                        done = true
                        k.resume()
                        return
                    }
                }
            }
        }
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
        return (o, i)
    }

    // One pass: the whole composition, picture and sound, into one MP4.
    static func write(_ p: Prepared, speed: Double, to url: URL) async throws {
        let reader = try AVAssetReader(asset: p.composition)
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: p.composition.tracks(withMediaType: .video), videoSettings: nil)
        vo.videoComposition = p.video
        vo.alwaysCopiesSampleData = false
        reader.add(vo)
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        writer.shouldOptimizeForNetworkUse = true
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings(p.size, fps: 1 / p.video.frameDuration.seconds))
        vi.expectsMediaDataInRealTime = false
        writer.add(vi)
        let audio = await audioIO(p, speed: speed)
        if let (ao, ai) = audio {
            reader.add(ao)
            ai.expectsMediaDataInRealTime = false
            writer.add(ai)
        }
        probe?.note("writer 1 piece")
        guard reader.startReading() else { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        guard writer.startWriting() else { throw Failure.failed(writer.error?.localizedDescription ?? "Can't write the video.") }
        writer.startSession(atSourceTime: .zero)
        await withTaskGroup(of: Void.self) { g in
            g.addTask { await pump(vo, into: vi, on: DispatchQueue(label: "ather.export.video")) }
            if let (ao, ai) = audio { g.addTask { await pump(ao, into: ai, on: DispatchQueue(label: "ather.export.audio")) } }
        }
        if reader.status == .failed || writer.status == .failed {
            reader.cancelReading()
            writer.cancelWriting()
            throw Failure.failed((writer.error ?? reader.error)?.localizedDescription ?? "Export failed.")
        }
        writer.endSession(atSourceTime: p.composition.duration)
        await writer.finishWriting()
        if writer.status != .completed { throw Failure.failed(writer.error?.localizedDescription ?? "Export failed.") }
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
        let reader = try AVAssetReader(asset: p.composition)
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: p.composition.tracks(withMediaType: .video), videoSettings: nil)
        vo.videoComposition = vc
        vo.alwaysCopiesSampleData = false
        reader.add(vo)
        // As many frames as the saved MP4 is long (whole frames of the sequence), like before.
        let fd = p.video.frameDuration.seconds
        let n = max(1, Int((p.composition.duration.seconds / fd).rounded(.up) * fd * fps + 1e-6))
        // ImageIO picks the palette and compresses at finalize, on one thread. So the frames go out in a few chunks,
        // each finalized on its own as soon as its frames are in, and the chunks are joined (joinGIFs).
        let k = max(1, min(chunks ?? gifChunks(n), n))
        probe?.note(k == 1 ? "gif 1 pass" : "gif 1 pass, \(k) chunks")
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
                guard let d = CGImageDestinationCreateWithData(m, UTType.gif.identifier as CFString, ((c + 1) * n + k - 1) / k - (c * n + k - 1) / k, nil) else { throw Failure.failed("Can't write the GIF.") }
                CGImageDestinationSetProperties(d, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
                (dest, data) = (d, m)
            }
            CGImageDestinationAddImage(dest!, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: Double(frames) / fps]] as CFDictionary)
        }
        // Each frame shows until the next one; a frame the compositor skipped (nothing new) lengthens the one before.
        var pending: (Int, CGImage)?
        while let s = vo.copyNextSampleBuffer() {
            try Task.checkCancellation()
            guard let pb = CMSampleBufferGetImageBuffer(s), let img = cgImage(pb) else { continue }
            let i = Int((CMSampleBufferGetPresentationTimeStamp(s).seconds * fps).rounded())
            guard i < n else { break }
            if let (j, prev) = pending {
                guard i > j else { pending = (j, img); continue }
                try add(j, prev, frames: i - j)
            }
            probe?.frame(i, img)
            pending = (i, img)
        }
        if reader.status == .failed { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        if let (j, prev) = pending { try add(j, prev, frames: max(1, n - j)) }
        close()
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
