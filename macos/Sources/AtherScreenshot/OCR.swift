import AppKit
import Vision

struct OcrWord {
    var text: String
    var rect: CGRect   // image pixels, top-left origin
    var line: Int
}

enum OCR {
    private static func lines(_ img: CGImage, fast: Bool = false) throws -> [VNRecognizedTextObservation] {
        let req = VNRecognizeTextRequest()
        req.recognitionLevel = fast ? .fast : .accurate
        req.usesLanguageCorrection = true
        req.automaticallyDetectsLanguage = true
        try VNImageRequestHandler(cgImage: img, options: [:]).perform([req])
        // Reading order: top to bottom, then left to right.
        return (req.results ?? []).sorted {
            abs($0.boundingBox.midY - $1.boundingBox.midY) > 0.01 ? $0.boundingBox.midY > $1.boundingBox.midY : $0.boundingBox.minX < $1.boundingBox.minX
        }
    }

    static func text(_ img: CGImage) throws -> String {
        try lines(img).compactMap { $0.topCandidates(1).first?.string }.joined(separator: "\n")
    }

    static func words(_ img: CGImage) throws -> [OcrWord] {
        let W = CGFloat(img.width), H = CGFloat(img.height)
        var out: [OcrWord] = []
        for (i, obs) in try lines(img).enumerated() {
            guard let cand = obs.topCandidates(1).first else { continue }
            let s = cand.string
            var idx = s.startIndex
            while idx < s.endIndex {
                guard let start = s[idx...].firstIndex(where: { !$0.isWhitespace }) else { break }
                let end = s[start...].firstIndex(where: { $0.isWhitespace }) ?? s.endIndex
                if let box = try? cand.boundingBox(for: start..<end)?.boundingBox {
                    let r = CGRect(x: box.minX * W, y: (1 - box.maxY) * H, width: box.width * W, height: box.height * H)
                    out.append(OcrWord(text: String(s[start..<end]), rect: r, line: i))
                }
                idx = end
            }
        }
        return out
    }

    static func async<T>(_ work: @escaping () throws -> T, done: @escaping (Result<T, Error>) -> Void) {
        DispatchQueue.global(qos: .userInitiated).async {
            let r = Result { try work() }
            DispatchQueue.main.async { done(r) }
        }
    }

    // MARK: sensitive-data detection (emails, IPs, keys/tokens/JWTs, Luhn-valid cards, phone numbers)

    private static func luhn(_ digits: [Int]) -> Bool {
        var sum = 0
        for (i, d0) in digits.reversed().enumerated() {
            var d = d0
            if i % 2 == 1 { d *= 2; if d > 9 { d -= 9 } }
            sum += d
        }
        return sum % 10 == 0
    }

    private static let secretPrefixes = ["sk-", "sk_", "pk_", "rk_", "ghp_", "gho_", "ghs_", "github_pat_", "xox", "AKIA", "ASIA",
                                         "AIza", "eyJ", "glpat-", "npm_", "hf_"]

    static func looksLikeSecret(_ w: String) -> Bool {
        if w.count >= 12, secretPrefixes.contains(where: { w.hasPrefix($0) }) { return true }
        if w.count < 20 { return false }
        var letters = 0, digits = 0, upper = 0, lower = 0
        for c in w {
            if c.isNumber { digits += 1 }
            else if c.isLetter {
                letters += 1
                if c.isUppercase { upper += 1 } else { lower += 1 }
            } else if !"_-./+=".contains(c) { return false }
        }
        return digits >= 3 && letters >= 6 && upper > 0 && lower > 0  // random-looking mixed token, not a long word
    }

    private static let patterns: [NSRegularExpression] = [
        #"[A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(\.[A-Za-z0-9\-]+)+"#,                       // email
        #"\b(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}\b"#,            // IPv4
        #"\b([0-9a-fA-F]{1,4}:){3,7}[0-9a-fA-F]{1,4}\b"#,                               // IPv6
        #"(\+?\d{1,3}[\s.\-]?)?\(?\d{2,4}\)?[\s.\-]?\d{3,4}[\s.\-]?\d{3,4}"#,           // phone
    ].map { try! NSRegularExpression(pattern: $0) }
    private static let card = try! NSRegularExpression(pattern: #"\b(\d[ \-]?){13,19}\b"#)

    static func findSensitive(_ words: [OcrWord]) -> [CGRect] {
        var hit = words.map { looksLikeSecret($0.text) }
        // Line-level patterns can span several OCR words ("4111 1111 1111 1111", "+1 555 123 4567").
        var start = 0
        while start < words.count {
            var end = start
            var line = ""
            var spans: [Range<Int>] = []
            while end < words.count, words[end].line == words[start].line {
                if end > start { line += " " }
                let a = (line as NSString).length
                line += words[end].text
                spans.append(a..<(line as NSString).length)
                end += 1
            }
            func mark(_ r: NSRange) {
                for (k, s) in spans.enumerated() where s.lowerBound < NSMaxRange(r) && s.upperBound > r.location { hit[start + k] = true }
            }
            let ns = line as NSString
            let all = NSRange(location: 0, length: ns.length)
            for (pi, re) in patterns.enumerated() {
                for m in re.matches(in: line, range: all) {
                    let digits = ns.substring(with: m.range).filter(\.isNumber).count
                    if pi == 3 && digits < 9 { continue }  // short numbers aren't phone numbers
                    mark(m.range)
                }
            }
            for m in card.matches(in: line, range: all) {
                let digits = ns.substring(with: m.range).compactMap { $0.wholeNumberValue }
                if digits.count >= 13 && luhn(digits) { mark(m.range) }
            }
            start = end
        }
        // Merge neighbouring hits on the same line into one box.
        var out: [CGRect] = []
        var i = 0
        while i < words.count {
            if !hit[i] { i += 1; continue }
            var r = words[i].rect
            while i + 1 < words.count, hit[i + 1], words[i + 1].line == words[i].line {
                i += 1
                r = r.union(words[i].rect)
            }
            out.append(r.insetBy(dx: -3, dy: -3))
            i += 1
        }
        return out
    }

    static func pixelate(_ img: CGImage, rects: [CGRect]) -> CGImage {
        guard !rects.isEmpty else { return img }
        var ci = CIImage(cgImage: img)
        let H = CGFloat(img.height)
        for r in rects {
            let block = max(6, r.height / 3)
            let cr = CGRect(x: r.minX, y: H - r.maxY, width: r.width, height: r.height)  // CI is bottom-up
            let px = CIImage(cgImage: img).clampedToExtent()
                .applyingFilter("CIPixellate", parameters: [kCIInputScaleKey: block, kCIInputCenterKey: CIVector(x: cr.minX, y: cr.minY)])
                .cropped(to: cr)
            ci = px.composited(over: ci)
        }
        return sharedCIContext.createCGImage(ci, from: CGRect(x: 0, y: 0, width: img.width, height: img.height)) ?? img
    }
}
