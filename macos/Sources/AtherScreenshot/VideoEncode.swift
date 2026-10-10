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

    static func gif(_ e: VideoEdit, to url: URL, fps: Double = 12) async throws {
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
        guard let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.gif.identifier as CFString, n, nil) else { throw Failure.failed("Can't write the GIF.") }
        CGImageDestinationSetProperties(dest, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        probe?.note("gif 1 pass")
        guard reader.startReading() else { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        // Each frame shows until the next one; a frame the compositor skipped (nothing new) lengthens the one before.
        var pending: (Int, CGImage)?
        func add(_ img: CGImage, frames: Int) {
            CGImageDestinationAddImage(dest, img, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: Double(frames) / fps]] as CFDictionary)
        }
        while let s = vo.copyNextSampleBuffer() {
            try Task.checkCancellation()
            guard let pb = CMSampleBufferGetImageBuffer(s), let img = cgImage(pb) else { continue }
            let i = Int((CMSampleBufferGetPresentationTimeStamp(s).seconds * fps).rounded())
            guard i < n else { break }
            if let (j, prev) = pending {
                guard i > j else { pending = (j, img); continue }
                add(prev, frames: i - j)
            }
            probe?.frame(i, img)
            pending = (i, img)
        }
        if reader.status == .failed { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        if let (j, prev) = pending { add(prev, frames: max(1, n - j)) }
        let t0 = Date()
        defer { probe?.note(String(format: "finalize %.2f s", Date().timeIntervalSince(t0))) }
        guard CGImageDestinationFinalize(dest) else { throw Failure.failed("Can't write the GIF.") }
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
