import AVFoundation
import AppKit
import ImageIO
import UniformTypeIdentifiers
import VideoToolbox

// "Compress to under N MB": a smaller copy of a video or picture next to the original (<name>_small.mp4 / .jpg), sized
// to fit a chat app's or email's limit. The original is never changed. Port of src/compress.h/.cpp; the math is the same.
//
// Videos: H.264 + AAC MP4 (plays everywhere, unlike HEVC) at an average bit rate worked out from the cap and the length:
// 90% of the cap (room for the container and the encoder's drift) over the duration, less 96 kbps of sound. Longer than
// 1920 on the long side is scaled down (never up); when the video gets under 500 kbps, 1280; under 250 kbps, 854. When
// the result still comes out over the cap, the bit rate drops by cap / actual × 0.95 and it's encoded again, at most twice.
// Pictures: JPEG, quality 85% and then lower, then smaller, until it fits.
enum Compress {
    static let defaultVideoMB = 30.0
    static let defaultPictureMB = 2.0
    static let videoPresetsMB: [Double] = [8, 25, 50]   // Discord (free), email, Discord Nitro Basic and the like
    static let picturePresetsMB: [Double] = [1, 2, 5]
    static let margin = 0.9          // of the cap, for the container and rate-control drift
    static let audioBps = 96_000.0   // AAC
    static let floorBps = 100_000.0  // less video than this looks too poor: trim the clip instead
    static let retries = 2

    static func bytes(mb: Double) -> Double { mb * 1024 * 1024 }

    struct VideoPlan: Equatable {
        var fits = false   // false: the cap is too small for this length, even at the lowest size
        var w = 0, h = 0   // the output frame: even, the source's shape, never larger than the source
        var videoBps = 0.0, audioBps = 0.0
    }

    // The largest side the video keeps at `videoBps` bits a second.
    static func longSide(_ videoBps: Double) -> Int { videoBps >= 500_000 ? 1920 : videoBps >= 250_000 ? 1280 : 854 }

    // `w` × `h`: the source frame as shown (upright). `videoScale`: a retry's factor on the video rate (retryScale).
    static func planVideo(cap: Double, seconds: Double, w: Int, h: Int, audio: Bool, videoScale: Double = 1) -> VideoPlan {
        var p = VideoPlan()
        guard cap > 0, seconds > 0, w > 0, h > 0 else { return p }
        let total = cap * margin * 8 / seconds
        p.audioBps = audio ? audioBps : 0
        p.videoBps = (total - p.audioBps) * videoScale
        p.fits = p.videoBps >= floorBps
        let scale = min(1, Double(longSide(p.videoBps)) / Double(max(w, h)))
        p.w = max(2, Int((Double(w) * scale / 2).rounded()) * 2)   // H.264 needs even sides
        p.h = max(2, Int((Double(h) * scale / 2).rounded()) * 2)
        return p
    }

    // After an attempt that came out at `actual` bytes over `cap`: the factor for the next attempt's video rate.
    static func retryScale(cap: Double, actual: Double) -> Double { actual > 0 ? cap / actual * 0.95 : 0.5 }

    // JPEG attempts for a picture, in order, until one fits: each quality at full size, then each again at ¾, ½ and ⅜
    // of the size, never below 320 px on the long side.
    struct PictureStep: Equatable {
        var quality = 0.85, scale = 1.0
    }
    static func pictureSteps(w: Int, h: Int) -> [PictureStep] {
        var steps: [PictureStep] = []
        for scale in [1.0, 0.75, 0.5, 0.375] {
            if scale < 1 && Double(max(w, h)) * scale < 320 { break }
            for q in [0.85, 0.75, 0.6, 0.45] { steps.append(PictureStep(quality: q, scale: scale)) }
        }
        return steps
    }

    // "12", "12 MB", "12mb", "1.5", "0,5" → megabytes; 0 when it isn't a size from 0.1 MB to 4 GB.
    static func parseMegabytes(_ s: String) -> Double {
        var t = s.replacingOccurrences(of: ",", with: ".").lowercased().replacingOccurrences(of: " ", with: "")
        for unit in ["mb", "m"] where t.hasSuffix(unit) { t.removeLast(unit.count); break }
        guard let mb = Double(t), mb.isFinite else { return 0 }
        return mb >= 0.1 && mb <= 4096 ? mb : 0
    }

    // "8 MB", "2.5 MB".
    static func label(_ mb: Double) -> String {
        mb == mb.rounded() ? "\(Int(mb)) MB" : "\(String(format: "%.3g", mb)) MB"
    }

    static func sizeText(_ bytes: Double) -> String {
        bytes < 1024 * 1024 ? "\(Int((bytes / 1024).rounded())) KB" : String(format: "%.1f MB", bytes / (1024 * 1024))
    }

    // Names handed out for results not moved in yet, so two at once never share one.
    private static let lock = NSLock()
    private static var reserved = Set<String>()

    // Where the smaller copy goes: <folder>/<name>_small.<ext>, or _small-2, _small-3… when that's taken.
    static func outputURL(for source: URL, ext: String) -> URL {
        let dir = source.deletingLastPathComponent(), stem = source.deletingPathExtension().lastPathComponent
        lock.lock(); defer { lock.unlock() }
        var i = 1
        while true {
            let u = dir.appendingPathComponent("\(stem)_small\(i > 1 ? "-\(i)" : "").\(ext)")
            if !FileManager.default.fileExists(atPath: u.path) && !reserved.contains(u.path) {
                reserved.insert(u.path)
                return u
            }
            i += 1
        }
    }
    private static func unreserve(_ u: URL) { lock.lock(); reserved.remove(u.path); lock.unlock() }

    enum Failure: LocalizedError {
        case failed(String)
        var errorDescription: String? { if case .failed(let s) = self { return s }; return nil }
    }

    struct Result {
        var skipped = false    // already under the cap: nothing made
        var tmp: URL?          // the new file, in the temporary folder until finish moves it to `out`
        var out: URL?          // where it goes (outputURL, reserved until finish)
        var bytesBefore = 0.0, bytesAfter = 0.0
        var w = 0, h = 0       // the new file's frame
        var attempts = 0       // encodes (videos) or JPEG tries (pictures)
    }

    static func fileBytes(_ u: URL) -> Double { Double((try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
    private static func tempURL(_ ext: String) -> URL {
        FileManager.default.temporaryDirectory.appendingPathComponent("ather-compress-\(UUID().uuidString).\(ext)")
    }

    // Moves the temporary file to `out`, never over an existing file.
    static func finish(_ r: inout Result) throws {
        guard let tmp = r.tmp, let out = r.out else { return }
        defer { unreserve(out); r.tmp = nil }
        do { try FileManager.default.moveItem(at: tmp, to: out) }   // fails rather than replace
        catch {
            try? FileManager.default.removeItem(at: tmp)
            throw Failure.failed("Can't save next to the original (\(out.path)).")
        }
    }

    // Either kind, by the file's type. Writes only temporary files; finish puts the result in place.
    static func file(_ url: URL, cap: Double, progress: ((Double, Int) -> Void)? = nil) async throws -> Result {
        switch MediaType.of(url) {
        case .video: return try await video(url, cap: cap, progress: progress)
        case .image: return try await Task.detached(priority: .userInitiated) { try Compress.picture(url, cap: cap) }.value
        case .gif: throw Failure.failed("GIFs can't be compressed yet.")
        }
    }

    // MARK: video

    // `progress(fraction, attempt)` from any thread, every 5% or so of an encode; `attempt` 0, then 1 and 2 for retries.
    static func video(_ url: URL, cap: Double, progress: ((Double, Int) -> Void)? = nil) async throws -> Result {
        let before = fileBytes(url)
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.failed("Can't read this video.") }
        let audio = try await asset.loadTracks(withMediaType: .audio).first
        let duration = try await asset.load(.duration)
        let (natural, transform, nominal) = try await track.load(.naturalSize, .preferredTransform, .nominalFrameRate)
        let upright = CGRect(origin: .zero, size: natural).applying(transform).size
        let seconds = duration.seconds
        guard seconds > 0, upright.width >= 1, upright.height >= 1 else { throw Failure.failed("Can't read this video.") }
        if before > 0 && before <= cap {
            var r = Result()
            r.bytesBefore = before
            r.skipped = true
            return r
        }
        let fps = nominal > 0 ? min(60, max(1, Double(nominal).rounded())) : 30
        var r = try await underCap(seconds: seconds, w: Int(upright.width.rounded()), h: Int(upright.height.rounded()), audio: audio != nil, cap: cap,
                                   progress: progress) { p, tmp, report in
            try await encode(asset, video: track, audio: audio, transform: transform, upright: upright, duration: duration, fps: fps, plan: p, to: tmp,
                             progress: report)
        }
        r.bytesBefore = before
        r.out = outputURL(for: url, ext: "mp4")
        return r
    }

    // The video editor's "Save smaller": the edit (trim, crop, speed, captions, markup…) rendered the way Save renders it
    // and encoded under the cap the same way. `out` is left for the caller (the editor saves into the captures folder).
    static func edit(_ e: VideoEdit, cap: Double, progress: ((Double, Int) -> Void)? = nil) async throws -> Result {
        let prepared = try await VideoExport.prepare(e)
        let seconds = prepared.composition.duration.seconds
        guard seconds > 0, prepared.size.width >= 1, prepared.size.height >= 1 else { throw Failure.failed("Can't read this video.") }
        let audio = prepared.composition.tracks(withMediaType: .audio)
        return try await underCap(seconds: seconds, w: Int(prepared.size.width.rounded()), h: Int(prepared.size.height.rounded()), audio: !audio.isEmpty,
                                  cap: cap, progress: progress) { p, tmp, report in
            try await encodeEdit(prepared, audio: audio, plan: p, to: tmp, progress: report)
        }
    }

    // The plan for the length, frame and sound, encoded by `encode` into a temporary file, then up to two smaller tries
    // when it comes out over the cap. The result's `tmp` is the file; `out` isn't set.
    private static func underCap(seconds: Double, w: Int, h: Int, audio: Bool, cap: Double, progress: ((Double, Int) -> Void)?,
                                 encode: (VideoPlan, URL, @escaping (Double) -> Void) async throws -> Void) async throws -> Result {
        var r = Result()
        let tmp = tempURL("mp4")
        var scale = 1.0
        for attempt in 0...retries {
            let p = planVideo(cap: cap, seconds: seconds, w: w, h: h, audio: audio, videoScale: scale)
            guard p.fits else {
                try? FileManager.default.removeItem(at: tmp)
                throw Failure.failed(attempt == 0 ? "\(clockText(seconds)) of video doesn't fit in \(label(cap / (1024 * 1024))). Trim it first, or pick a larger size."
                                                  : "It still came out at \(sizeText(r.bytesAfter)). Trim it first, or pick a larger size.")
            }
            try? FileManager.default.removeItem(at: tmp)
            do {
                try await encode(p, tmp) { progress?($0, attempt) }
            } catch {
                try? FileManager.default.removeItem(at: tmp)
                throw error
            }
            r.attempts += 1
            r.bytesAfter = fileBytes(tmp)
            r.w = p.w; r.h = p.h
            if r.bytesAfter > 0 && r.bytesAfter <= cap {
                r.tmp = tmp
                return r
            }
            scale *= retryScale(cap: cap, actual: r.bytesAfter)
        }
        try? FileManager.default.removeItem(at: tmp)
        throw Failure.failed("It came out at \(sizeText(r.bytesAfter)), over \(label(cap / (1024 * 1024))). Trim it first, or pick a larger size.")
    }

    private static func clockText(_ s: Double) -> String { s < 90 ? "\(Int(s.rounded())) s" : String(format: "%d:%02d", Int(s) / 60, Int(s) % 60) }

    private static func videoInput(_ p: VideoPlan, fps: Double) -> AVAssetWriterInput {
        let vi = AVAssetWriterInput(mediaType: .video, outputSettings: [
            AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: p.w, AVVideoHeightKey: p.h,
            AVVideoCompressionPropertiesKey: [AVVideoAverageBitRateKey: Int(p.videoBps),
                                              AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
                                              AVVideoExpectedSourceFrameRateKey: fps,
                                              AVVideoMaxKeyFrameIntervalDurationKey: 2] as [String: Any]])
        vi.expectsMediaDataInRealTime = false
        return vi
    }

    // 48 kHz stereo 16-bit PCM from the reader, AAC at the plan's rate into the file.
    private static let pcmSettings: [String: Any] = [AVFormatIDKey: kAudioFormatLinearPCM, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2,
                                                     AVLinearPCMBitDepthKey: 16, AVLinearPCMIsFloatKey: false, AVLinearPCMIsBigEndianKey: false,
                                                     AVLinearPCMIsNonInterleaved: false]
    private static func audioInput(_ p: VideoPlan) -> AVAssetWriterInput {
        let ai = AVAssetWriterInput(mediaType: .audio, outputSettings: [
            AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 2, AVEncoderBitRateKey: Int(p.audioBps)])
        ai.expectsMediaDataInRealTime = false
        return ai
    }

    private static func writer(_ url: URL) throws -> AVAssetWriter {
        let w = try AVAssetWriter(outputURL: url, fileType: .mp4)
        w.shouldOptimizeForNetworkUse = true   // the index at the front: previews and streams at once
        return w
    }

    // One encode of a file: the video turned upright and scaled to the plan's frame by a video composition, H.264 at the
    // plan's average rate; the sound as 48 kHz stereo AAC at 96 kbps.
    private static func encode(_ asset: AVAsset, video track: AVAssetTrack, audio: AVAssetTrack?, transform: CGAffineTransform, upright: CGSize,
                               duration: CMTime, fps: Double, plan p: VideoPlan, to url: URL, progress: @escaping (Double) -> Void) async throws {
        let reader = try AVAssetReader(asset: asset)
        let comp = AVMutableVideoComposition()
        comp.renderSize = CGSize(width: p.w, height: p.h)
        comp.frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps))
        let ins = AVMutableVideoCompositionInstruction()
        ins.timeRange = CMTimeRange(start: .zero, duration: duration)
        let layer = AVMutableVideoCompositionLayerInstruction(assetTrack: track)
        layer.setTransform(transform.concatenating(CGAffineTransform(scaleX: CGFloat(p.w) / upright.width, y: CGFloat(p.h) / upright.height)), at: .zero)
        ins.layerInstructions = [layer]
        comp.instructions = [ins]
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: [track], videoSettings: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange])
        vo.videoComposition = comp
        vo.alwaysCopiesSampleData = false
        guard reader.canAdd(vo) else { throw Failure.failed("Can't read this video.") }
        reader.add(vo)
        let w = try writer(url)
        let vi = videoInput(p, fps: fps)
        w.add(vi)
        var streams = [Stream(input: vi, reports: true, next: { vo.copyNextSampleBuffer() }, append: { vi.append($0) })]
        if let audio, p.audioBps > 0 {
            let ao = AVAssetReaderTrackOutput(track: audio, outputSettings: pcmSettings)
            ao.alwaysCopiesSampleData = false
            let ai = audioInput(p)
            if reader.canAdd(ao) && w.canAdd(ai) {
                reader.add(ao)
                w.add(ai)
                streams.append(Stream(input: ai, reports: false, next: { ao.copyNextSampleBuffer() }, append: { ai.append($0) }))
            }
        }
        try await run(reader, w, streams, duration: duration, progress: progress)
    }

    // One encode of an edit: its frames as the editor's renderer draws them (the same composition Save uses), scaled to
    // the plan's frame when it's smaller; its sound mixed and, for a speed change, kept at its pitch, as Save does.
    private static func encodeEdit(_ prepared: VideoExport.Prepared, audio: [AVAssetTrack], plan p: VideoPlan, to url: URL,
                                   progress: @escaping (Double) -> Void) async throws {
        let comp = prepared.composition
        let reader = try AVAssetReader(asset: comp)
        let vo = AVAssetReaderVideoCompositionOutput(videoTracks: comp.tracks(withMediaType: .video), videoSettings: nil)
        vo.videoComposition = prepared.video
        vo.alwaysCopiesSampleData = false
        guard reader.canAdd(vo) else { throw Failure.failed("Can't read this video.") }
        reader.add(vo)
        let w = try writer(url)
        let fps = min(60, max(1, 1 / prepared.video.frameDuration.seconds))
        let vi = videoInput(p, fps: fps)
        let adaptor = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: vi, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: p.w,
            kCVPixelBufferHeightKey as String: p.h])
        w.add(vi)
        let scaler = Scaler()
        var streams = [Stream(input: vi, reports: true, next: { vo.copyNextSampleBuffer() }, append: { b in
            guard let src = CMSampleBufferGetImageBuffer(b) else { return true }   // no picture: nothing to write
            let t = CMSampleBufferGetPresentationTimeStamp(b)
            if CVPixelBufferGetWidth(src) == p.w && CVPixelBufferGetHeight(src) == p.h { return adaptor.append(src, withPresentationTime: t) }
            guard let pool = adaptor.pixelBufferPool, let dst = scaler.scaled(src, pool: pool) else { return false }
            return adaptor.append(dst, withPresentationTime: t)
        })]
        if !audio.isEmpty, p.audioBps > 0 {
            let ao = AVAssetReaderAudioMixOutput(audioTracks: audio, audioSettings: pcmSettings)
            ao.audioTimePitchAlgorithm = .spectral   // sped-up sound keeps its pitch
            ao.alwaysCopiesSampleData = false
            let ai = audioInput(p)
            if reader.canAdd(ao) && w.canAdd(ai) {
                reader.add(ao)
                w.add(ai)
                streams.append(Stream(input: ai, reports: false, next: { ao.copyNextSampleBuffer() }, append: { ai.append($0) }))
            }
        }
        try await run(reader, w, streams, duration: comp.duration, progress: progress)
    }

    // Scales frames down with VideoToolbox into buffers from the writer's pool.
    private final class Scaler {
        private var session: VTPixelTransferSession?
        func scaled(_ src: CVPixelBuffer, pool: CVPixelBufferPool) -> CVPixelBuffer? {
            if session == nil {
                VTPixelTransferSessionCreate(allocator: nil, pixelTransferSessionOut: &session)
                if let session { VTSessionSetProperty(session, key: kVTPixelTransferPropertyKey_ScalingMode, value: kVTScalingMode_Normal) }
            }
            var dst: CVPixelBuffer?
            guard let session, CVPixelBufferPoolCreatePixelBuffer(nil, pool, &dst) == kCVReturnSuccess, let dst,
                  VTPixelTransferSessionTransferImage(session, from: src, to: dst) == noErr else { return nil }
            return dst
        }
    }

    private struct Stream {
        let input: AVAssetWriterInput
        let reports: Bool                              // its timestamps drive the progress
        let next: () -> CMSampleBuffer?
        let append: (CMSampleBuffer) -> Bool
    }

    // Runs the reader into the writer, every stream pumped on its own queue, then closes the file.
    private static func run(_ reader: AVAssetReader, _ writer: AVAssetWriter, _ streams: [Stream], duration: CMTime,
                            progress: @escaping (Double) -> Void) async throws {
        guard reader.startReading() else { throw Failure.failed(reader.error?.localizedDescription ?? "Can't read this video.") }
        guard writer.startWriting() else { throw Failure.failed(writer.error?.localizedDescription ?? "Can't write the video.") }
        writer.startSession(atSourceTime: .zero)
        let total = max(duration.seconds, 0.001)
        let ok = await withTaskGroup(of: Bool.self) { g in
            for (n, s) in streams.enumerated() {
                g.addTask {
                    await withCheckedContinuation { (k: CheckedContinuation<Bool, Never>) in
                        let state = PumpState()   // touched only on the pump's queue
                        s.input.requestMediaDataWhenReady(on: DispatchQueue(label: "ather.compress.\(n)")) {
                            while !state.done && s.input.isReadyForMoreMediaData {
                                guard let b = s.next() else {
                                    s.input.markAsFinished()
                                    state.done = true
                                    return k.resume(returning: reader.status != .failed)
                                }
                                if s.reports {
                                    let f = min(1, max(0, CMSampleBufferGetPresentationTimeStamp(b).seconds / total))
                                    if Int(f * 20) != state.shown { state.shown = Int(f * 20); progress(f) }
                                }
                                if !s.append(b) {
                                    state.done = true
                                    return k.resume(returning: false)
                                }
                            }
                        }
                    }
                }
            }
            return await g.reduce(true) { $0 && $1 }
        }
        guard ok, reader.status != .failed, writer.status != .failed else {
            reader.cancelReading()
            writer.cancelWriting()
            throw Failure.failed((writer.error ?? reader.error)?.localizedDescription ?? "Encoding failed.")
        }
        writer.endSession(atSourceTime: duration)
        await writer.finishWriting()
        guard writer.status == .completed else { throw Failure.failed(writer.error?.localizedDescription ?? "Couldn't finish the MP4.") }
        progress(1)
    }

    private final class PumpState {
        var done = false
        var shown = -1   // the last 5% step reported
    }

    // MARK: picture

    static func picture(_ url: URL, cap: Double) throws -> Result {
        var r = Result()
        r.bytesBefore = fileBytes(url)
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil),
              let props = CGImageSourceCopyPropertiesAtIndex(src, 0, nil) as? [CFString: Any],
              var w = props[kCGImagePropertyPixelWidth] as? Int, var h = props[kCGImagePropertyPixelHeight] as? Int, w > 0, h > 0
        else { throw Failure.failed("Can't read this picture.") }
        if r.bytesBefore > 0 && r.bytesBefore <= cap { r.skipped = true; return r }
        if let o = props[kCGImagePropertyOrientation] as? Int, o >= 5 { swap(&w, &h) }   // stored sideways: shown the other way round
        var image: CGImage?, scaledBy = 0.0
        for s in pictureSteps(w: w, h: h) {
            if s.scale != scaledBy {
                scaledBy = s.scale
                image = flattened(src, maxSide: max(1, Int((Double(max(w, h)) * s.scale).rounded())))
            }
            guard let image, let jpeg = jpegData(image, quality: s.quality) else { break }
            r.attempts += 1
            r.bytesAfter = Double(jpeg.count)
            r.w = image.width; r.h = image.height
            if r.bytesAfter <= cap {
                let tmp = tempURL("jpg")
                do { try jpeg.write(to: tmp) } catch { throw Failure.failed("Can't write the picture.") }
                r.tmp = tmp
                r.out = outputURL(for: url, ext: "jpg")
                return r
            }
        }
        throw Failure.failed(r.attempts > 0 ? "The smallest it got was \(sizeText(r.bytesAfter)). Pick a larger size." : "Can't encode the picture.")
    }

    // The picture upright, at most `maxSide` on its long side, with any transparency on white (JPEG has none).
    private static func flattened(_ src: CGImageSource, maxSide: Int) -> CGImage? {
        guard let img = CGImageSourceCreateThumbnailAtIndex(src, 0, [kCGImageSourceCreateThumbnailFromImageAlways: true,
                                                                     kCGImageSourceCreateThumbnailWithTransform: true,
                                                                     kCGImageSourceThumbnailMaxPixelSize: maxSide] as CFDictionary),
              let ctx = CGContext(data: nil, width: img.width, height: img.height, bitsPerComponent: 8, bytesPerRow: 0,
                                  space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
        else { return nil }
        let rect = CGRect(x: 0, y: 0, width: img.width, height: img.height)
        ctx.setFillColor(CGColor(gray: 1, alpha: 1))
        ctx.fill(rect)
        ctx.draw(img, in: rect)
        return ctx.makeImage()
    }

    private static func jpegData(_ img: CGImage, quality: Double) -> Data? {
        let data = NSMutableData()
        guard let dest = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(dest, img, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        return CGImageDestinationFinalize(dest) ? data as Data : nil
    }

    // MARK: UI

    // The sizes offered for these files: the last one used for their kind first, then the presets (videos' when any is
    // a video, else pictures').
    static func sizes(for urls: [URL]) -> [Double] {
        let video = urls.contains { MediaType.of($0) == .video }
        let last = Settings.shared.double(video ? "CompressVideoMB" : "CompressPictureMB")
        let first = last > 0 ? last : video ? defaultVideoMB : defaultPictureMB
        return [first] + (video ? videoPresetsMB : picturePresetsMB).filter { $0 != first }
    }

    // A palette of sizes for these files; typing a number offers that size too.
    static func showPicker(_ urls: [URL]) {
        guard !urls.isEmpty else { return }
        if let g = urls.first(where: { MediaType.of($0) == .gif }) { return Toast.shared.show("GIFs can't be compressed yet", g.lastPathComponent) }
        let video = urls.contains { MediaType.of($0) == .video }
        let items = sizes(for: urls).enumerated().map { i, mb in
            var hint = video && mb == 8 ? "Discord" : video && mb == 25 ? "Email" : ""
            if i == 0 { hint = mb == (video ? Compress.defaultVideoMB : Compress.defaultPictureMB) ? "Default" : "Last used" }
            return PaletteItem(id: "compress\(i)", title: "Under \(Compress.label(mb))",
                               keywords: video ? "smaller shrink mp4 h264 discord email size" : "smaller shrink jpeg jpg size",
                               icon: "arrow.down.right.and.arrow.up.left", hint: hint) { Compress.run(urls, mb: mb) }
        }
        let what = urls.count == 1 ? urls[0].lastPathComponent : "\(urls.count) files"
        Palette.shared.show(items, placeholder: "Compress \(what) to under…  (or type a size in MB)", mruKey: "CompressPaletteMRU",
                            create: ("Compress to under", { text in
                                let mb = Compress.parseMegabytes(text)
                                guard mb > 0 else { return Toast.shared.show("Type a size in MB", "For example 10 or 2.5") }
                                Compress.run(urls, mb: mb)
                            }))
    }

    // Each file compressed in turn, with a progress toast, the result in a toast (click: show it in Finder), and the new
    // files stacked with their originals in the gallery.
    static func run(_ urls: [URL], mb: Double) {
        guard !urls.isEmpty, mb > 0 else { return }
        let pictures = !urls.contains { MediaType.of($0) == .video }
        Settings.shared.set(pictures ? "CompressPictureMB" : "CompressVideoMB", mb)
        let toast = Toast.shared.post("Compressing to under \(label(mb))…", urls.count == 1 ? urls[0].lastPathComponent : "\(urls.count) files", ms: 3_600_000)
        Task { @MainActor in
            var made: [URL] = [], before = 0.0, after = 0.0, skipped = 0
            var firstError: (file: String, text: String)?
            for (i, u) in urls.enumerated() {
                let name = u.lastPathComponent, of = urls.count > 1 ? " (\(i + 1) of \(urls.count))" : ""
                do {
                    var r = try await Compress.file(u, cap: Compress.bytes(mb: mb)) { f, attempt in
                        let body = "\(name)\(of)  ·  \(Int(f * 100))%\(attempt > 0 ? ", smaller try \(attempt + 1)" : "")"
                        DispatchQueue.main.async { Toast.shared.update(toast, body: body) }
                    }
                    if r.skipped { skipped += 1; continue }
                    guard let out = r.out else { continue }
                    // The gallery learns it's a version of the original before it sees the file.
                    let src = Library.shared.meta(u)
                    Library.shared.noteEdit(out, from: u, info: NameInfo(app: src.app, window: src.window), edited: false)
                    try Compress.finish(&r)
                    made.append(out)
                    before += r.bytesBefore
                    after += Compress.fileBytes(out)
                } catch {
                    if firstError == nil { firstError = (name, error.localizedDescription) }
                }
            }
            Toast.shared.hide()
            let cap = Compress.label(mb)
            guard !made.isEmpty else {
                if let e = firstError { return Toast.shared.show("Couldn't compress \(e.file)", e.text, ms: 8000) }
                return Toast.shared.show(urls.count == 1 ? "Already under \(cap)" : "All already under \(cap)", "Nothing to make smaller.", ms: 4000)
            }
            var body = (made.count == 1 ? "\(made[0].lastPathComponent), was " : "Was ") + Compress.sizeText(before) + "  ·  click to show in Finder"
            if skipped > 0 { body += "\n\(skipped) already under \(cap)" }
            if let e = firstError { body += "\n\(e.file): \(e.text)" }
            Toast.shared.show((made.count == 1 ? "Compressed to " : "Compressed \(made.count) files to ") + Compress.sizeText(after), body, ms: 8000) {
                NSWorkspace.shared.activateFileViewerSelecting(made)
            }
        }
    }
}
