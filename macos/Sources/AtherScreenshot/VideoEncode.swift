import AVFoundation
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
