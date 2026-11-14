import AVFoundation
import Combine
import CoreGraphics
import Foundation

// Scrubbing a video tile in the gallery: the mouse along the thin timeline at the bottom of the tile shows the frame
// there, and the duration badge reads `0:12 / 0:34`. As on Windows (src/gallery.cpp): 48 frames spread over the
// video, the one under the mouse decoded first and the rest coarse to fine, each at the tile's thumbnail size; one
// video at a time (asking for another drops the last one's), and until a frame arrives the nearest decoded one shows.

enum Scrub {
    static let frames = 48             // frame k shows time (k + 0.5) / frames of the video
    static let zone: CGFloat = 18      // the bottom of a tile that scrubs, in points
    static let minWidth: CGFloat = 60  // narrower tiles get no timeline

    // The frame at `at` (0…1) along the timeline.
    static func index(_ at: Double) -> Int { min(frames - 1, max(0, Int(at * Double(frames)))) }
    // The time frame `k` shows, in a video of `duration` seconds.
    static func time(_ k: Int, duration: Double) -> Double { (Double(k) + 0.5) / Double(frames) * duration }

    // The order frames are decoded in: coarse to fine (32, 16, 8, 24, 40…), so a scrub has something near every spot early on.
    static let order: [Int] = {
        var order: [Int] = [], seen = [Bool](repeating: false, count: frames)
        var step = 64
        while step >= 1 {
            var k = step / 2
            while k < frames {
                if !seen[k] { seen[k] = true; order.append(k) }
                k += step
            }
            step /= 2
        }
        return order + (0..<frames).filter { !seen[$0] }
    }()

    // The decoded frame nearest `k`, while the rest are still coming.
    static func nearest<T>(_ pics: [T?], _ k: Int) -> T? {
        for d in 0..<pics.count {
            for j in [k - d, k + d] where j >= 0 && j < pics.count { if let p = pics[j] { return p } }
        }
        return nil
    }

    // The badge while scrubbing: the second the mouse is in, over the length (`0:12 / 0:34`).
    static func badge(_ at: Double, length: Double) -> String {
        formatTime(floor(min(1, max(0, at)) * length)) + " / " + formatTime(length)
    }

    // Whether a point (tile coordinates, top-left origin) is on a video tile's timeline.
    static func inZone(_ p: CGPoint, width: CGFloat, height: CGFloat) -> Bool {
        width > minWidth && p.x >= 0 && p.x <= width && p.y >= height - zone && p.y <= height
    }
}

// The video being scrubbed and its frames, as they come. One per gallery; only the scrubbed tile's layers read it.
// Its state is only touched on the main thread; decodes run off it and hand back there.
final class Scrubber: ObservableObject, @unchecked Sendable {
    @Published private(set) var url: URL?            // the tile being scrubbed, or nil
    @Published private(set) var at: Double = 0       // 0…1 along its timeline
    @Published private(set) var pics: [CGImage?] = []
    @Published private(set) var duration: Double = 0  // from the file, for videos the index hasn't measured yet

    private var source: URL?    // the video `pics` are of
    private var side = 0
    private var gen = 0
    private var want = -1
    private var done: [Bool] = []
    private var busy = false
    private var asset: Source?

    // A video's decoder. Used by one decode at a time.
    private final class Source: @unchecked Sendable {
        let gen: AVAssetImageGenerator
        let asset: AVURLAsset
        var duration: Double?
        init(_ u: URL, side: Int) {
            asset = AVURLAsset(url: u)
            gen = AVAssetImageGenerator(asset: asset)
            gen.appliesPreferredTrackTransform = true
            gen.maximumSize = CGSize(width: side, height: side)
        }
        func frame(_ k: Int) async -> (CGImage?, Double) {
            if duration == nil { duration = (try? await asset.load(.duration)).map(CMTimeGetSeconds) ?? 0 }
            let d = duration ?? 0
            guard d > 0 else { return (nil, 0) }
            // Any frame within half a slot of the spot will do: much cheaper than an exact one, and it looks the same.
            let tol = CMTime(seconds: d / Double(Scrub.frames) / 2, preferredTimescale: 600)
            gen.requestedTimeToleranceBefore = tol
            gen.requestedTimeToleranceAfter = tol
            return ((try? await gen.image(at: CMTime(seconds: Scrub.time(k, duration: d), preferredTimescale: 600)).image), d)
        }
    }

    // The mouse at `at` (0…1) along `u`'s timeline, on a tile `side` points across. Main thread.
    func scrub(_ u: URL, at: Double, side: CGFloat) {
        let a = min(1, max(0, at))
        if url != u { url = u }
        if self.at != a { self.at = a }
        let px = ThumbCache.bucket(side)
        if u != source || px > self.side {
            source = u
            self.side = px
            gen += 1
            pics = [CGImage?](repeating: nil, count: Scrub.frames)
            done = [Bool](repeating: false, count: Scrub.frames)
            duration = 0
            asset = Source(u, side: px)
        }
        want = Scrub.index(a)
        pump()
    }

    // The mouse left `u`'s timeline: its poster comes back. Its frames stay, for coming back to it.
    func end(_ u: URL? = nil) {
        guard url != nil, u == nil || u == url else { return }
        url = nil
    }

    var picture: CGImage? { url != nil && url == source ? Scrub.nearest(pics, Scrub.index(at)) : nil }

    // Decodes the next frame: the one under the mouse, else the next coarse-to-fine one. One at a time, so a fast
    // mouse never queues up work: by the time a frame is done, `want` is wherever the mouse is now.
    private func pump() {
        guard !busy, let asset, !done.isEmpty else { return }
        let k: Int
        if want >= 0 && want < done.count && !done[want] { k = want }
        else if let n = Scrub.order.first(where: { !done[$0] }) { k = n }
        else { return }
        done[k] = true
        busy = true
        let g = gen
        Task.detached(priority: .utility) { [weak self] in
            let (img, d) = await asset.frame(k)
            DispatchQueue.main.async { [weak self] in
                guard let self else { return }
                self.busy = false
                if g == self.gen {
                    if d <= 0 { self.done = [Bool](repeating: true, count: Scrub.frames) }  // not a video it can read
                    if self.duration != d { self.duration = d }
                    if let img { self.pics[k] = img }
                }
                self.pump()
            }
        }
    }

    // Frames decoded so far, for tests.
    var decoded: Int { pics.lazy.filter { $0 != nil }.count }
}
