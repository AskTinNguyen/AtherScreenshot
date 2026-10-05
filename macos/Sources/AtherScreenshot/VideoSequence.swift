import AVFoundation
import CoreImage
import UniformTypeIdentifiers

// MARK: - One list of picture and video types

// The open panels, drop targets, the gallery's import and CFBundleDocumentTypes (public.image, public.movie) all
// agree with this list. Videos are whatever AVFoundation says it can open (Windows: MediaExtensions in library.cpp).
enum MediaFiles {
    static let pictureExtensions: [String] = ["png", "jpg", "jpeg", "gif", "bmp", "webp", "tif", "tiff", "heic", "heif"]
    static let videoExtensions: [String] = {
        var out = ["mp4", "mov", "m4v"]
        for t in AVURLAsset.audiovisualTypes() {
            guard let u = UTType(t.rawValue), u.conforms(to: .movie) else { continue }
            for e in u.tags[.filenameExtension] ?? [] where !out.contains(e.lowercased()) { out.append(e.lowercased()) }
        }
        return out
    }()
    static let all: Set<String> = Set(pictureExtensions + videoExtensions)

    static func isVideo(_ u: URL) -> Bool { videoExtensions.contains(u.pathExtension.lowercased()) }
    static func isMedia(_ u: URL) -> Bool { all.contains(u.pathExtension.lowercased()) }

    static func contentTypes(pictures: Bool, videos: Bool) -> [UTType] {
        ((pictures ? pictureExtensions : []) + (videos ? videoExtensions : [])).compactMap { UTType(filenameExtension: $0) }
    }
}

// MARK: - Clips

// One piece of video on the timeline. Pieces split from one added video share `source`; a second copy of the same
// file added separately gets its own. Times are seconds in the file. Same fields as Clip in videoedit.h, plus the
// rotation the Mac reads from the track.
struct Clip: Equatable {
    var id = UUID()
    var source = UUID()
    var path: String
    var inPoint: Double
    var outPoint: Double
    var length: Double         // the whole file
    var w: Int, h: Int         // upright size
    var fps: Double
    var hasAudio: Bool
    var rotation = 0           // clockwise degrees that show it upright (phone videos)

    var duration: Double { max(0, outPoint - inPoint) }
    var url: URL { URL(fileURLWithPath: path) }
    var name: String { url.lastPathComponent }
}

enum VideoSource {
    enum Failure: LocalizedError {
        case noVideo, undecodable(String)
        var errorDescription: String? {
            switch self {
            case .noVideo: return "This file has no video track."
            case .undecodable(let name): return "This Mac can't decode the video in \(name)."
            }
        }
    }

    // A video only counts if its frames really decode (an HEVC file without the codec opens fine but can't be
    // shown), and its sound only if the audio decodes too.
    static func probe(_ url: URL) async throws -> Clip {
        let asset = AVURLAsset(url: url, options: [AVURLAssetPreferPreciseDurationAndTimingKey: true])
        guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw Failure.noVideo }
        let (natural, tf, fps) = try await vt.load(.naturalSize, .preferredTransform, .nominalFrameRate)
        let length = try await asset.load(.duration).seconds
        let shown = CGRect(origin: .zero, size: natural).applying(tf)
        let gen = AVAssetImageGenerator(asset: asset)
        gen.maximumSize = CGSize(width: 64, height: 64)
        gen.requestedTimeToleranceAfter = CMTime(seconds: 1, preferredTimescale: 600)
        do { _ = try await gen.image(at: .zero) } catch { throw Failure.undecodable(url.lastPathComponent) }
        var audio = false
        for at in try await asset.loadTracks(withMediaType: .audio) where decodes(asset, at) { audio = true; break }
        guard length.isFinite, length > 0 else { throw Failure.undecodable(url.lastPathComponent) }
        return Clip(path: url.path, inPoint: 0, outPoint: length, length: length, w: Int(abs(shown.width).rounded()), h: Int(abs(shown.height).rounded()),
                    fps: Double(fps), hasAudio: audio, rotation: rotation(tf))
    }

    static func decodes(_ asset: AVAsset, _ track: AVAssetTrack) -> Bool {
        guard let r = try? AVAssetReader(asset: asset) else { return false }
        let o = AVAssetReaderTrackOutput(track: track, outputSettings: [AVFormatIDKey: kAudioFormatLinearPCM])
        guard r.canAdd(o) else { return false }
        r.add(o)
        guard r.startReading() else { return false }
        defer { r.cancelReading() }
        return o.copyNextSampleBuffer() != nil
    }

    // The track's transform as quarter turns clockwise (0, 90, 180, 270).
    static func rotation(_ t: CGAffineTransform) -> Int {
        let deg = Int((atan2(t.b, t.a) * 180 / .pi).rounded())
        return ((deg % 360 + 360) % 360 + 45) / 90 % 4 * 90
    }
}

// MARK: - The sequence: clips end to end, as one composition

// Timeline time is the clips laid end to end. The preview player, export, GIF, thumbnails, frame grabs and auto
// captions all read the composition this builds; each clip is turned upright and fitted into the frame with black
// bars by the compositor below, then the frame renderer draws the markup.
enum VideoSequence {
    // Where each clip starts on the timeline, and the total.
    static func starts(_ clips: [Clip]) -> [Double] {
        var t = 0.0
        return clips.map { c in defer { t += c.duration }; return t }
    }
    static func total(_ clips: [Clip]) -> Double { clips.reduce(0) { $0 + $1.duration } }

    // The clip showing timeline time `t`, and the time in its file.
    static func locate(_ clips: [Clip], _ t: Double) -> (index: Int, fileTime: Double)? {
        guard !clips.isEmpty else { return nil }
        var start = 0.0
        for (i, c) in clips.enumerated() {
            if t < start + c.duration || i == clips.count - 1 { return (i, c.inPoint + min(max(0, t - start), c.duration)) }
            start += c.duration
        }
        return nil
    }

    struct Built {
        let composition: AVMutableComposition
        let video: AVMutableVideoComposition
    }

    private static let scale: CMTimeScale = 60000

    // `range`: the part of the timeline to include (nil: all). `speed` scales it. `box` draws the markup.
    static func build(_ clips: [Clip], frame: CGSize, range: ClosedRange<Double>? = nil, speed: Double = 1, muted: Bool = false,
                      box: RendererBox) async throws -> Built {
        let comp = AVMutableComposition()
        guard let vtrack = comp.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid) else { throw VideoSource.Failure.noVideo }
        var audio: [AVMutableCompositionTrack] = []
        struct Piece { let clip: Clip; let timelineStart: Double }
        var pieces: [Piece] = []
        var cursor = CMTime.zero
        let a = range?.lowerBound ?? 0, b = range?.upperBound ?? .infinity
        var timeline = 0.0
        for c in clips {
            defer { timeline += c.duration }
            let from = max(a, timeline), to = min(b, timeline + c.duration)
            guard to - from > 0.0005 else { continue }
            let asset = AVURLAsset(url: c.url, options: [AVURLAssetPreferPreciseDurationAndTimingKey: true])
            guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw VideoSource.Failure.noVideo }
            let src = CMTimeRange(start: CMTime(seconds: c.inPoint + from - timeline, preferredTimescale: scale),
                                  duration: CMTime(seconds: to - from, preferredTimescale: scale))
            try vtrack.insertTimeRange(src, of: vt, at: cursor)
            if !muted && c.hasAudio {
                let tracks = try await asset.loadTracks(withMediaType: .audio).filter { VideoSource.decodes(asset, $0) }
                for (k, at) in tracks.enumerated() {
                    if k >= audio.count, let t = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) { audio.append(t) }
                    guard k < audio.count else { break }
                    // Clips without sound leave a silent gap, so the sound stays in sync after them.
                    let end = audio[k].timeRange.end
                    if end < cursor { audio[k].insertEmptyTimeRange(CMTimeRange(start: end, end: cursor)) }
                    let len = min(src.duration, CMTimeSubtract(try await at.load(.timeRange).end, src.start))
                    if len > .zero { try audio[k].insertTimeRange(CMTimeRange(start: src.start, duration: len), of: at, at: cursor) }
                }
            }
            pieces.append(Piece(clip: c, timelineStart: from))
            cursor = CMTimeAdd(cursor, src.duration)
        }
        guard !pieces.isEmpty else { throw VideoSource.Failure.noVideo }
        if speed != 1 {
            comp.scaleTimeRange(CMTimeRange(start: .zero, duration: cursor), toDuration: CMTimeMultiplyByFloat64(cursor, multiplier: 1 / speed))
        }
        // One instruction per piece, from the track's own segments, so a clip boundary lands exactly where the
        // composition switches files (rounding of the speed change included).
        let segments = vtrack.segments.filter { !$0.isEmpty }
        var instructions: [SequenceInstruction] = []
        for (i, seg) in segments.enumerated() where i < pieces.count {
            var r = seg.timeMapping.target
            if let last = instructions.last { r = CMTimeRange(start: last.timeRange.end, end: r.end) }  // no gaps
            if i == segments.count - 1 || i == pieces.count - 1 { r = CMTimeRange(start: r.start, end: max(r.end, comp.duration)) }
            instructions.append(SequenceInstruction(range: r, track: vtrack.trackID, clip: pieces[i].clip, frame: frame,
                                                    timelineStart: pieces[i].timelineStart, speed: speed, box: box))
        }
        let vc = AVMutableVideoComposition()
        vc.customVideoCompositorClass = SequenceCompositor.self
        vc.instructions = instructions
        vc.renderSize = box.renderer.out
        let fps = clips.first.map { $0.fps > 1 ? min(60, $0.fps.rounded()) : 30 } ?? 30
        vc.frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps))
        return Built(composition: comp, video: vc)
    }

    // A clip's frame turned upright and fitted into the sequence frame with black bars (CI coordinates, y up).
    static func place(_ src: CIImage, clip: Clip, frame: CGSize) -> CIImage {
        var img = src
        switch clip.rotation {
        case 90: img = img.oriented(.right)
        case 180: img = img.oriented(.down)
        case 270: img = img.oriented(.left)
        default: break
        }
        img = img.transformed(by: CGAffineTransform(translationX: -img.extent.minX, y: -img.extent.minY))
        let w = img.extent.width, h = img.extent.height
        guard w > 0, h > 0 else { return CIImage(color: .black).cropped(to: CGRect(origin: .zero, size: frame)) }
        if abs(w - frame.width) < 0.5 && abs(h - frame.height) < 0.5 { return img }
        let k = min(frame.width / w, frame.height / h)
        let fitted = img.transformed(by: CGAffineTransform(scaleX: k, y: k)
            .concatenating(CGAffineTransform(translationX: (frame.width - w * k) / 2, y: (frame.height - h * k) / 2)))
        return fitted.composited(over: CIImage(color: .black)).cropped(to: CGRect(origin: .zero, size: frame))
    }
}

final class SequenceInstruction: NSObject, AVVideoCompositionInstructionProtocol {
    let timeRange: CMTimeRange
    let enablePostProcessing = false
    let containsTweening = true
    let requiredSourceTrackIDs: [NSValue]?
    let passthroughTrackID = kCMPersistentTrackID_Invalid
    let track: CMPersistentTrackID
    let clip: Clip
    let frame: CGSize
    let timelineStart: Double  // timeline time at the start of this instruction
    let speed: Double
    let box: RendererBox

    init(range: CMTimeRange, track: CMPersistentTrackID, clip: Clip, frame: CGSize, timelineStart: Double, speed: Double, box: RendererBox) {
        timeRange = range
        self.track = track
        requiredSourceTrackIDs = [NSNumber(value: track)]
        self.clip = clip
        self.frame = frame
        self.timelineStart = timelineStart
        self.speed = speed
        self.box = box
    }

    func timelineTime(_ t: CMTime) -> Double { timelineStart + CMTimeSubtract(t, timeRange.start).seconds * speed }
}

final class SequenceCompositor: NSObject, AVVideoCompositing {
    private static let bgra: [String: any Sendable] = [kCVPixelBufferPixelFormatTypeKey as String: [kCVPixelFormatType_32BGRA],
                                                        kCVPixelBufferIOSurfacePropertiesKey as String: [String: Int]()]
    let sourcePixelBufferAttributes: [String: any Sendable]? = SequenceCompositor.bgra
    let requiredPixelBufferAttributesForRenderContext: [String: any Sendable] = SequenceCompositor.bgra
    private let space = CGColorSpace(name: CGColorSpace.sRGB)!
    // Blends in sRGB like the earlier filter pipeline (and GDI+ on Windows), so captions keep their weight.
    private static let context = CIContext(options: [.workingColorSpace: CGColorSpace(name: CGColorSpace.sRGB)!, .cacheIntermediates: false])

    func renderContextChanged(_ newRenderContext: AVVideoCompositionRenderContext) {}

    func startRequest(_ req: AVAsynchronousVideoCompositionRequest) {
        guard let ins = req.videoCompositionInstruction as? SequenceInstruction, let out = req.renderContext.newPixelBuffer() else {
            return req.finish(with: NSError(domain: "Ather", code: 1, userInfo: [NSLocalizedDescriptionKey: "No frame to render."]))
        }
        let size = req.renderContext.size
        let src = req.sourceFrame(byTrackID: ins.track).map { VideoSequence.place(CIImage(cvPixelBuffer: $0), clip: ins.clip, frame: ins.frame) }
            ?? CIImage(color: .black).cropped(to: CGRect(origin: .zero, size: ins.frame))
        let img = ins.box.renderer.render(src, at: ins.timelineTime(req.compositionTime))
        SequenceCompositor.context.render(img, to: out, bounds: CGRect(origin: .zero, size: size), colorSpace: space)
        req.finish(withComposedVideoFrame: out)
    }
}
