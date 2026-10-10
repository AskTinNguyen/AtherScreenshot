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

// MARK: - Retiming

extension VideoEdit {
    // Replaces the clips and moves everything on the timeline with the footage it sits on (ApplyClips in
    // videoedit.cpp, same rules): an item stays at the same moment of the same file, looked up in any clip with
    // the same `source` that still shows it (the same clip first, which keeps things in place across a split).
    // Otherwise it falls back to its clip, clamped, and goes when that clip is gone or both its ends were cut off
    // on the same side. Items keep their length; the trim follows its footage.
    mutating func applyClips(_ clips: [Clip]) {
        let before = self.clips
        let oldTotal = VideoSequence.total(before), newTotal = VideoSequence.total(clips)
        let starts = VideoSequence.starts(clips)
        struct Landing { var t: Double?; var side = 0; var clip: UUID? }
        func land(_ t: Double) -> Landing {
            var l = Landing()
            guard let spot = VideoSequence.locate(before, t) else { return l }
            let oc = before[spot.index], src = spot.fileTime
            var found: Int?
            for (i, nc) in clips.enumerated() {
                guard nc.source == oc.source, nc.path.lowercased() == oc.path.lowercased(), src >= nc.inPoint - 1e-6, src <= nc.outPoint + 1e-6 else { continue }
                if found == nil || nc.id == oc.id { found = i }
            }
            if found == nil { found = clips.firstIndex { $0.id == oc.id } }
            guard let k = found else { return l }
            let nc = clips[k]
            l.clip = nc.id
            l.side = src < nc.inPoint - 1e-6 ? -1 : src > nc.outPoint + 1e-6 ? 1 : 0
            l.t = starts[k] + min(max(0, src - nc.inPoint), nc.duration)
            return l
        }
        func move(_ start: inout Double, _ end: inout Double) -> Bool {
            let a = land(start), b = land(max(start, end - 1e-6))
            guard let at = a.t else { return false }
            if a.side != 0 && b.clip == a.clip && b.side == a.side { return false }
            let len = end - start
            start = min(at, max(0, newTotal - 0.1))
            end = min(newTotal, start + len)
            return end > start
        }
        marks = marks.compactMap { m in
            var m = m
            return move(&m.start, &m.end) ? m : nil
        }
        captions = captions.compactMap { c in
            var c = c
            let s0 = c.start
            guard move(&c.start, &c.end) else { return nil }
            for i in c.words.indices { c.words[i].start += c.start - s0; c.words[i].end += c.start - s0 }
            return c
        }
        captions = captions.enumerated().sorted { ($0.element.start, $0.offset) < ($1.element.start, $1.offset) }.map(\.element)
        // An untouched end stays at the end; otherwise both ends follow their footage.
        let wholeStart = trimStart <= 1e-6, wholeEnd = trimEnd >= oldTotal - 1e-6
        let ts = land(trimStart), te = land(max(0, trimEnd - 1e-6))
        trimStart = wholeStart || ts.t == nil ? 0 : ts.t!
        trimEnd = wholeEnd || te.t == nil ? newTotal : min(newTotal, te.t! + (te.side != 0 ? 0 : 1e-6))  // looked up just before itself
        if trimEnd - trimStart < 0.1 { trimStart = 0; trimEnd = newTotal }
        self.clips = clips
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
        let unplayable: Set<UUID>  // clips whose file couldn't be read: black in the composition, skipped while playing
    }

    private static let scale: CMTimeScale = 60000

    // `range`: the part of the timeline to include (nil: all). `speed` scales it. `box` draws the markup.
    static func build(_ clips: [Clip], frame: CGSize, range: ClosedRange<Double>? = nil, speed: Double = 1, muted: Bool = false,
                      box: RendererBox) async throws -> Built {
        let comp = AVMutableComposition()
        guard let vtrack = comp.addMutableTrack(withMediaType: .video, preferredTrackID: kCMPersistentTrackID_Invalid) else { throw VideoSource.Failure.noVideo }
        var audio: [AVMutableCompositionTrack] = []
        struct Piece { let index: Int; let timelineStart: Double; let at: CMTime; let duration: CMTime }
        var pieces: [Piece] = []
        var unplayable: Set<UUID> = []
        var cursor = CMTime.zero
        let a = range?.lowerBound ?? 0, b = range?.upperBound ?? .infinity
        var timeline = 0.0
        for (i, c) in clips.enumerated() {
            defer { timeline += c.duration }
            let from = max(a, timeline), to = min(b, timeline + c.duration)
            guard to - from > 0.0005 else { continue }
            let src = CMTimeRange(start: CMTime(seconds: c.inPoint + from - timeline, preferredTimescale: scale),
                                  duration: CMTime(seconds: to - from, preferredTimescale: scale))
            let asset = AVURLAsset(url: c.url, options: [AVURLAssetPreferPreciseDurationAndTimingKey: true])
            do {
                guard let vt = try await asset.loadTracks(withMediaType: .video).first else { throw VideoSource.Failure.noVideo }
                try vtrack.insertTimeRange(src, of: vt, at: cursor)
            } catch {
                // Moved, deleted or unreadable since it was added: keep its time (so everything after stays in
                // sync) and let playback skip it.
                vtrack.insertEmptyTimeRange(CMTimeRange(start: cursor, duration: src.duration))
                unplayable.insert(c.id)
            }
            if !muted && c.hasAudio && !unplayable.contains(c.id) {
                let tracks = (try? await asset.loadTracks(withMediaType: .audio))?.filter { VideoSource.decodes(asset, $0) } ?? []
                for (k, at) in tracks.enumerated() {
                    if k >= audio.count, let t = comp.addMutableTrack(withMediaType: .audio, preferredTrackID: kCMPersistentTrackID_Invalid) { audio.append(t) }
                    guard k < audio.count else { break }
                    // Clips without sound leave a silent gap, so the sound stays in sync after them.
                    let end = audio[k].timeRange.end
                    if end < cursor { audio[k].insertEmptyTimeRange(CMTimeRange(start: end, end: cursor)) }
                    let len = min(src.duration, CMTimeSubtract((try? await at.load(.timeRange).end) ?? .zero, src.start))
                    if len > .zero { try? audio[k].insertTimeRange(CMTimeRange(start: src.start, duration: len), of: at, at: cursor) }
                }
            }
            pieces.append(Piece(index: i, timelineStart: from, at: cursor, duration: src.duration))
            cursor = CMTimeAdd(cursor, src.duration)
        }
        guard !pieces.isEmpty, unplayable.count < pieces.count else { throw VideoSource.Failure.noVideo }
        if speed != 1 {
            comp.scaleTimeRange(CMTimeRange(start: .zero, duration: cursor), toDuration: CMTimeMultiplyByFloat64(cursor, multiplier: 1 / speed))
        }
        // One instruction per piece, back to back (consecutive pieces of one file may share a track segment, so
        // these come from the pieces, not the segments). Each knows its neighbours, in case a frame right at a cut
        // comes from the other side.
        var instructions: [SequenceInstruction] = []
        for (n, p) in pieces.enumerated() {
            let start = instructions.last?.timeRange.end ?? .zero
            let end = n == pieces.count - 1 ? comp.duration : CMTimeMultiplyByFloat64(CMTimeAdd(p.at, p.duration), multiplier: 1 / speed)
            let near = [n > 0 ? pieces[n - 1].index : nil, n + 1 < pieces.count ? pieces[n + 1].index : nil].compactMap { $0 }.map { clips[$0] }
            instructions.append(SequenceInstruction(range: CMTimeRange(start: start, end: max(start, end)), track: vtrack.trackID, clip: clips[p.index],
                                                    neighbours: near, frame: frame, timelineStart: p.timelineStart, speed: speed, box: box))
        }
        let vc = AVMutableVideoComposition()
        vc.customVideoCompositorClass = SequenceCompositor.self
        vc.instructions = instructions
        vc.renderSize = box.renderer.out
        let fps = clips.first.map { $0.fps > 1 ? min(60, $0.fps.rounded()) : 30 } ?? 30
        vc.frameDuration = CMTime(value: 1, timescale: CMTimeScale(fps))
        return Built(composition: comp, video: vc, unplayable: unplayable)
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
    let neighbours: [Clip]
    let frame: CGSize
    let timelineStart: Double  // timeline time at the start of this instruction
    let speed: Double
    let box: RendererBox

    init(range: CMTimeRange, track: CMPersistentTrackID, clip: Clip, neighbours: [Clip], frame: CGSize, timelineStart: Double, speed: Double, box: RendererBox) {
        timeRange = range
        self.track = track
        requiredSourceTrackIDs = [NSNumber(value: track)]
        self.clip = clip
        self.neighbours = neighbours
        self.frame = frame
        self.timelineStart = timelineStart
        self.speed = speed
        self.box = box
    }

    func timelineTime(_ t: CMTime) -> Double { timelineStart + CMTimeSubtract(t, timeRange.start).seconds * speed }

    // The clip a decoded frame belongs to: this one, unless its stored size says it's from the clip next door.
    func clip(for frame: CVPixelBuffer) -> Clip {
        let w = CVPixelBufferGetWidth(frame), h = CVPixelBufferGetHeight(frame)
        func fits(_ c: Clip) -> Bool { c.rotation % 180 == 0 ? (c.w == w && c.h == h) : (c.w == h && c.h == w) }
        if fits(clip) { return clip }
        return neighbours.first(where: fits) ?? clip
    }
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
        let src = req.sourceFrame(byTrackID: ins.track).map { VideoSequence.place(CIImage(cvPixelBuffer: $0), clip: ins.clip(for: $0), frame: ins.frame) }
            ?? CIImage(color: .black).cropped(to: CGRect(origin: .zero, size: ins.frame))
        var img = ins.box.renderer.render(src, at: ins.timelineTime(req.compositionTime))
        let full = ins.box.renderer.out
        if abs(size.width - full.width) > 0.5 || abs(size.height - full.height) > 0.5 {   // a smaller render (the GIF)
            img = img.transformed(by: CGAffineTransform(scaleX: size.width / full.width, y: size.height / full.height))
        }
        SequenceCompositor.context.render(img, to: out, bounds: CGRect(origin: .zero, size: size), colorSpace: space)
        if let p = VideoExport.probe { p.frame(Int((req.compositionTime.seconds / req.renderContext.videoComposition.frameDuration.seconds).rounded()), out) }
        req.finish(withComposedVideoFrame: out)
    }
}
