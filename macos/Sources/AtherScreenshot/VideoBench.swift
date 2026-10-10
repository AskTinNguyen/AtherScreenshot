import AVFoundation
import CoreImage
import Foundation

// Developer tool: times the video export on real recordings, and checks that a faster export still makes the
// same pictures (the Mac side of src/videobench.cpp).
//
//   AtherScreenshot --bench-export <outDir> [tap] <clip>...
//     Exports each clip in a few typical edits and prints how long each took, how many frames it rendered and how
//     it ran. With `tap`, also hashes every frame the export renders and keeps every 30th one (raw BGRA) in
//     <outDir>, for --bench-compare.
//   AtherScreenshot --bench-compare <dirA> <dirB>
//     Compares two tapped runs: identical frames, and the PSNR of the kept frames that differ.
// Reads the clips only; everything it writes goes to <outDir>. ATHER_BENCH_ONLY=<scenario> runs one scenario;
// ATHER_ENCODERS=n and ATHER_GIF_PARTS=n try another number of MP4 encoders or GIF parts.
enum VideoBench {
    struct Scenario {
        let name: String
        let gif: Bool
        var edit: VideoEdit
    }

    static let lines = ["Open the project settings", "Pick the build target", "Then click deploy",
                        "Wait for the green check", "That's it, it's live", "Thanks for watching"]

    static func mark(_ k: MarkKind, _ x0: Double, _ y0: Double, _ x1: Double, _ y1: Double, _ start: Double, _ end: Double,
                     _ style: AnimStyle = .auto, _ text: String = "") -> Mark {
        Mark(kind: k, start: start, end: end, a: CGPoint(x: x0, y: y0), b: CGPoint(x: x1, y: y1), text: text, color: 0, style: style)
    }

    // Marks and captions like a typical tutorial edit, placed in a W × H frame from `t0` on (AddMarkup).
    static func addMarkup(_ e: inout VideoEdit, _ W: Double, _ H: Double, _ t0: Double, _ len: Double) {
        func at(_ f: Double) -> Double { t0 + len * f }
        var title = mark(.title, 0, 0, W, H, at(0), at(0.12), .auto, "Release 1.2")
        title.subtitle = "What's new"
        e.marks.append(title)
        e.marks.append(mark(.box, W * 0.1, H * 0.2, W * 0.4, H * 0.45, at(0.1), at(0.4), .drawOn))
        e.marks.append(mark(.arrow, W * 0.7, H * 0.8, W * 0.45, H * 0.5, at(0.15), at(0.45), .drawOn))
        e.marks.append(mark(.text, W * 0.55, H * 0.1, W * 0.9, H * 0.2, at(0.2), at(0.6), .typewriter, "Click Deploy to ship it"))
        e.marks.append(mark(.blur, W * 0.6, H * 0.6, W * 0.85, H * 0.75, at(0.05), at(0.95)))
        e.marks.append(mark(.pixelate, W * 0.05, H * 0.75, W * 0.3, H * 0.92, at(0.3), at(0.6)))
        e.marks.append(mark(.zoom, W * 0.3, H * 0.3, W * 0.5, H * 0.5, at(0.55), at(0.75)))
        var emoji = mark(.emoji, W * 0.8, H * 0.3, W * 0.8 + H * 0.1, H * 0.4, at(0.4), at(0.8), .pop, "✅")
        emoji.emphasis = .ping
        e.marks.append(emoji)
        var bubble = mark(.bubble, W * 0.2, H * 0.55, W * 0.45, H * 0.65, at(0.6), at(0.9), .pop, "Saved!")
        bubble.emphasis = .pulse
        e.marks.append(bubble)
        for i in 0..<6 { e.captions.append(Caption(start: at(Double(i) / 6), end: at((Double(i) + 0.9) / 6), text: lines[i])) }
    }

    static func scenarios(_ c: Clip) -> [Scenario] {
        let D = c.length, W = Double(c.w), H = Double(c.h)
        func base(_ end: Double) -> VideoEdit {
            var e = VideoEdit(trimEnd: min(D, end))
            e.clips = [c]
            e.frame = CGSize(width: c.w, height: c.h)
            return e
        }
        var out: [Scenario] = []
        out.append(Scenario(name: "plain", gif: false, edit: base(20)))
        do {
            var e = base(0)
            e.trimStart = min(1, D / 10)
            e.trimEnd = min(D, e.trimStart + 20)
            e.crop = CGRect(x: W * 0.05, y: H * 0.05, width: W * 0.9, height: H * 0.9)
            addMarkup(&e, W, H, e.trimStart, e.trimEnd - e.trimStart)
            out.append(Scenario(name: "edits", gif: false, edit: e))
        }
        do {
            var e = base(30)
            e.speed = 2
            out.append(Scenario(name: "speed2", gif: false, edit: e))
        }
        do {   // captions all along, nothing else
            var e = base(20)
            for i in 0..<6 { e.captions.append(Caption(start: e.trimEnd * Double(i) / 6, end: e.trimEnd * (Double(i) + 0.95) / 6, text: lines[i])) }
            out.append(Scenario(name: "captions", gif: false, edit: e))
        }
        do {   // a typical tutorial: captions all along, a box and an arrow now and then, a small blur hiding an address
            var e = base(20)
            let T = e.trimEnd
            for i in 0..<6 { e.captions.append(Caption(start: T * Double(i) / 6, end: T * (Double(i) + 0.95) / 6, text: lines[i])) }
            e.marks.append(mark(.box, W * 0.1, H * 0.2, W * 0.3, H * 0.3, T * 0.1, T * 0.3, .drawOn))
            e.marks.append(mark(.arrow, W * 0.6, H * 0.6, W * 0.45, H * 0.45, T * 0.5, T * 0.7, .drawOn))
            e.marks.append(mark(.blur, W * 0.7, H * 0.06, W * 0.85, H * 0.1, 0, T))
            out.append(Scenario(name: "light", gif: false, edit: e))
        }
        if D >= 40 { out.append(Scenario(name: "long", gif: false, edit: base(D))) }   // a whole long recording, as it is
        do {
            var e = base(8)
            addMarkup(&e, W, H, 0, e.trimEnd)
            out.append(Scenario(name: "gif", gif: true, edit: e))
        }
        return out
    }

    static func cpuSeconds() -> Double {
        var u = rusage()
        getrusage(RUSAGE_SELF, &u)
        func s(_ t: timeval) -> Double { Double(t.tv_sec) + Double(t.tv_usec) / 1e6 }
        return s(u.ru_utime) + s(u.ru_stime)
    }

    // A rendered frame as tightly packed BGRA (premultiplied, sRGB), whatever it came as.
    private static let tapContext = CIContext(options: [.workingColorSpace: CGColorSpace(name: CGColorSpace.sRGB)!, .cacheIntermediates: false])
    static func bgra(_ pb: CVPixelBuffer) -> (w: Int, h: Int, bytes: [UInt8])? {
        let w = CVPixelBufferGetWidth(pb), h = CVPixelBufferGetHeight(pb)
        var bytes = [UInt8](repeating: 0, count: w * h * 4)
        if CVPixelBufferGetPixelFormatType(pb) == kCVPixelFormatType_32BGRA {
            CVPixelBufferLockBaseAddress(pb, .readOnly)
            defer { CVPixelBufferUnlockBaseAddress(pb, .readOnly) }
            guard let base = CVPixelBufferGetBaseAddress(pb) else { return nil }
            let row = CVPixelBufferGetBytesPerRow(pb)
            bytes.withUnsafeMutableBytes { d in
                for y in 0..<h { memcpy(d.baseAddress! + y * w * 4, base + y * row, w * 4) }
            }
        } else {
            tapContext.render(CIImage(cvPixelBuffer: pb), toBitmap: &bytes, rowBytes: w * 4, bounds: CGRect(x: 0, y: 0, width: w, height: h),
                              format: .BGRA8, colorSpace: CGColorSpace(name: CGColorSpace.sRGB)!)
        }
        return (w, h, bytes)
    }

    static func hash(_ b: [UInt8]) -> UInt64 {
        var h: UInt64 = 1469598103934665603
        b.withUnsafeBytes { p in
            let words = p.bindMemory(to: UInt64.self)
            for v in words { h = (h ^ v) &* 1099511628211 }
        }
        return h
    }

    // A number from the environment, to try another setting.
    static func knob(_ name: String) -> Int? { ProcessInfo.processInfo.environment[name].flatMap { Int($0) }.map { max(1, $0) } }

    static func say(_ s: String) {
        FileHandle.standardOutput.write(s.data(using: .utf8)!)
    }

    static func bench(_ dir: URL, tap: Bool, clips: [String]) async -> Int32 {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let only = ProcessInfo.processInfo.environment["ATHER_BENCH_ONLY"] ?? ""
        var total = 0.0
        for path in clips {
            let url = URL(fileURLWithPath: path)
            guard let clip = try? await VideoSource.probe(url) else { say("can't open \(path)\n"); return 1 }
            let base = url.deletingPathExtension().lastPathComponent
            say(String(format: "%@  %dx%d  %.0f fps  %.1f s%@\n", base, clip.w, clip.h, clip.fps, clip.length, clip.hasAudio ? "  audio" : ""))
            for s in scenarios(clip) where only.isEmpty || only == s.name {
                let tag = "\(base)_\(s.name)"
                let out = dir.appendingPathComponent(tag + (s.gif ? ".gif" : ".mp4"))
                try? FileManager.default.removeItem(at: out)
                let probe = VideoExport.Probe()
                let lock = NSLock()
                var hashes: [UInt64] = []
                let every = s.name == "long" ? 300 : 30   // keeps the long run's kept frames to a few GB
                if tap {
                    probe.tap = { i, img in
                        guard let (w, h, bytes) = bgra(img) else { return }
                        let v = hash(bytes)
                        lock.lock()
                        if hashes.count <= i { hashes += Array(repeating: 0, count: i + 1 - hashes.count) }
                        hashes[i] = v
                        lock.unlock()
                        if i % every == 0 {
                            var raw = Data()
                            withUnsafeBytes(of: Int32(w)) { raw.append(contentsOf: $0) }
                            withUnsafeBytes(of: Int32(h)) { raw.append(contentsOf: $0) }
                            raw.append(contentsOf: bytes)
                            try? raw.write(to: dir.appendingPathComponent("\(tag)_f\(i).raw"))
                        }
                    }
                }
                let cpu0 = cpuSeconds()
                let t0 = Date()
                var err: String?
                do {
                    if s.gif { try await VideoExport.gif(s.edit, to: out, parts: knob("ATHER_GIF_PARTS"), probe: probe) }
                    else { try await VideoExport.mp4(s.edit, to: out, encoders: knob("ATHER_ENCODERS"), probe: probe) }
                } catch { err = error.localizedDescription }
                let secs = Date().timeIntervalSince(t0), cpu = cpuSeconds() - cpu0
                total += secs
                say(String(format: "  %@ %7.2f s  %5d frames  %7.1f fps  cpu %6.2f s  [%@]%@\n", s.name.padding(toLength: 8, withPad: " ", startingAt: 0) as NSString, secs, probe.frames,
                           Double(probe.frames) / max(1e-9, secs), cpu, probe.notes.joined(separator: "; ") as NSString, err.map { "  FAILED: \($0)" } ?? ""))
                if tap { try? hashes.map(String.init).joined(separator: "\n").appending("\n").write(to: dir.appendingPathComponent(tag + ".hashes"), atomically: true, encoding: .utf8) }
            }
        }
        var u = rusage()
        getrusage(RUSAGE_SELF, &u)
        say(String(format: "total %.2f s, peak memory %.0f MB\n", total, Double(u.ru_maxrss) / 1048576))
        return 0
    }

    static func compare(_ a: URL, _ b: URL) -> Int32 {
        let fm = FileManager.default
        guard let names = try? fm.contentsOfDirectory(atPath: a.path).filter({ $0.hasSuffix(".hashes") }).sorted() else { return 2 }
        var bad: Int32 = 0
        for name in names {
            let tag = String(name.dropLast(7))
            let ha = ((try? String(contentsOf: a.appendingPathComponent(name), encoding: .utf8)) ?? "").split(separator: "\n")
            let hb = ((try? String(contentsOf: b.appendingPathComponent(name), encoding: .utf8)) ?? "").split(separator: "\n")
            let same = zip(ha, hb).filter { $0 == $1 }.count
            var worst = 1e9, maxDiff = 0, off = 0.0, all = 0.0
            let kept = Set(((try? fm.contentsOfDirectory(atPath: a.path)) ?? []) + ((try? fm.contentsOfDirectory(atPath: b.path)) ?? []))
                .filter { $0.hasPrefix(tag + "_f") && $0.hasSuffix(".raw") }
            for f in kept {
                let ra = try? Data(contentsOf: a.appendingPathComponent(f)), rb = try? Data(contentsOf: b.appendingPathComponent(f))
                guard let ra, let rb, ra.count == rb.count else { worst = 0; continue }
                var se = 0.0, n = 0
                ra.withUnsafeBytes { pa in
                    rb.withUnsafeBytes { pb in
                        var k = 8
                        while k < ra.count {
                            for c in 0..<3 {
                                let d = Int(pa[k + c]) - Int(pb[k + c])
                                se += Double(d * d)
                                maxDiff = max(maxDiff, abs(d))
                                if abs(d) > 1 { off += 1 }
                                all += 1
                                n += 1
                            }
                            k += 4
                        }
                    }
                }
                worst = min(worst, se == 0 ? 99 : 10 * log10(255 * 255 / (se / Double(n))))
            }
            say(String(format: "%@ frames %d/%d  identical %d  worst kept-frame PSNR %.1f dB, max diff %d, off by >1: %.4f%%\n", tag.padding(toLength: 28, withPad: " ", startingAt: 0) as NSString, ha.count, hb.count, same,
                       worst == 1e9 ? 99 : worst, maxDiff, all > 0 ? off * 100 / all : 0))
            if ha.count != hb.count || worst < 40 { bad += 1 }
        }
        return bad
    }

    static func main(_ args: [String]) -> Int32 {
        var code: Int32 = 2
        let done = DispatchSemaphore(value: 0)
        Task.detached {
            if args.first == "--bench-compare" {
                if args.count == 3 { code = compare(URL(fileURLWithPath: args[1]), URL(fileURLWithPath: args[2])) }
            } else if args.count >= 3 {
                let tap = args[2] == "tap"
                code = await bench(URL(fileURLWithPath: args[1]), tap: tap, clips: Array(args.dropFirst(tap ? 3 : 2)))
            }
            done.signal()
        }
        done.wait()
        return code
    }
}
