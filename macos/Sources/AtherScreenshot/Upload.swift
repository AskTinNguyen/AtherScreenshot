import CryptoKit
import Foundation
import UniformTypeIdentifiers

struct UploadConfig {
    var uploader: String   // none | imgur | custom | s3
    var imgurClientId: String
    var customUrl: String, customFileField: String, customHeaders: String, customResponseUrl: String
    var s3Endpoint: String, s3Bucket: String, s3Region: String, s3AccessKey: String, s3SecretKey: String, s3PublicUrl: String
}

enum Upload {
    static func problem(_ c: UploadConfig) -> String? {
        switch c.uploader.lowercased() {
        case "imgur": return c.imgurClientId.isEmpty ? "Set an Imgur client ID in Settings › Upload." : nil
        case "custom": return URL(string: c.customUrl)?.scheme == nil ? "Set a custom upload URL in Settings › Upload." : nil
        case "s3":
            return c.s3Bucket.isEmpty || c.s3AccessKey.isEmpty || c.s3SecretKey.isEmpty
                ? "Set the S3 endpoint, bucket, access key and secret key in Settings › Upload." : nil
        default: return "Choose an uploader (Imgur, custom or S3) in Settings › Upload."
        }
    }

    static func upload(_ file: URL, _ c: UploadConfig, done: @escaping (Result<String, Error>) -> Void) {
        Task {
            let r: Result<String, Error>
            do {
                switch c.uploader.lowercased() {
                case "imgur": r = .success(try await imgur(file, c))
                case "custom": r = .success(try await custom(file, c))
                case "s3": r = .success(try await s3(file, c))
                default: throw err(problem(c) ?? "No uploader")
                }
            } catch { r = .failure(error) }
            await MainActor.run { done(r) }
        }
    }

    static func err(_ s: String) -> NSError { NSError(domain: kAppName, code: 1, userInfo: [NSLocalizedDescriptionKey: s]) }

    static func mime(_ url: URL) -> String { UTType(filenameExtension: url.pathExtension)?.preferredMIMEType ?? "application/octet-stream" }

    private static func multipart(field: String, file: URL, data: Data) -> (Data, String) {
        let boundary = "AtherBoundary\(UUID().uuidString)"
        var body = Data()
        body.append("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(field)\"; filename=\"\(file.lastPathComponent)\"\r\nContent-Type: \(mime(file))\r\n\r\n".data(using: .utf8)!)
        body.append(data)
        body.append("\r\n--\(boundary)--\r\n".data(using: .utf8)!)
        return (body, "multipart/form-data; boundary=\(boundary)")
    }

    private static func send(_ req: URLRequest, body: Data) async throws -> (Data, HTTPURLResponse) {
        let (data, resp) = try await URLSession.shared.upload(for: req, from: body)
        guard let http = resp as? HTTPURLResponse else { throw err("No response") }
        guard (200..<300).contains(http.statusCode) else {
            let msg = String(data: data.prefix(300), encoding: .utf8) ?? ""
            throw err("HTTP \(http.statusCode) \(msg)")
        }
        return (data, http)
    }

    static func imgur(_ file: URL, _ c: UploadConfig) async throws -> String {
        let (body, type) = multipart(field: "image", file: file, data: try Data(contentsOf: file))
        var req = URLRequest(url: URL(string: "https://api.imgur.com/3/image")!)
        req.httpMethod = "POST"
        req.setValue("Client-ID \(c.imgurClientId)", forHTTPHeaderField: "Authorization")
        req.setValue(type, forHTTPHeaderField: "Content-Type")
        let (data, _) = try await send(req, body: body)
        guard let link = jsonPath(data, "data.link"), !link.isEmpty else { throw err("Imgur returned no link") }
        return link
    }

    static func custom(_ file: URL, _ c: UploadConfig) async throws -> String {
        let (body, type) = multipart(field: c.customFileField.isEmpty ? "file" : c.customFileField, file: file, data: try Data(contentsOf: file))
        var req = URLRequest(url: URL(string: c.customUrl)!)
        req.httpMethod = "POST"
        req.setValue(type, forHTTPHeaderField: "Content-Type")
        for h in c.customHeaders.split(separator: ";") {
            let parts = h.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
            if parts.count == 2 { req.setValue(parts[1], forHTTPHeaderField: parts[0]) }
        }
        let (data, _) = try await send(req, body: body)
        let link = c.customResponseUrl.isEmpty
            ? (String(data: data, encoding: .utf8) ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
            : (jsonPath(data, c.customResponseUrl) ?? "")
        guard !link.isEmpty else { throw err("The response had no link at “\(c.customResponseUrl)”") }
        return link
    }

    // MARK: S3-compatible PUT, AWS Signature V4

    static func uriEncode(_ s: String, keepSlash: Bool) -> String {
        var allowed = CharacterSet(charactersIn: "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_.~")
        if keepSlash { allowed.insert("/") }
        return s.addingPercentEncoding(withAllowedCharacters: allowed) ?? s
    }

    private static func hex(_ d: some Sequence<UInt8>) -> String { d.map { String(format: "%02x", $0) }.joined() }
    private static func hmac(_ key: Data, _ s: String) -> Data {
        Data(HMAC<SHA256>.authenticationCode(for: Data(s.utf8), using: SymmetricKey(data: key)))
    }

    static func s3(_ file: URL, _ c: UploadConfig) async throws -> String {
        let data = try Data(contentsOf: file)
        let region = c.s3Region.isEmpty ? "auto" : c.s3Region
        let endpoint = c.s3Endpoint.isEmpty ? "https://s3.\(region == "auto" ? "us-east-1" : region).amazonaws.com" : c.s3Endpoint
        guard let base = URL(string: endpoint.hasPrefix("http") ? endpoint : "https://" + endpoint), let host = base.host else {
            throw err("Invalid S3 endpoint")
        }
        let month: String = {
            let f = DateFormatter()
            f.dateFormat = "yyyy-MM"
            return f.string(from: Date())
        }()
        let key = month + "/" + file.lastPathComponent
        let uri = "/" + uriEncode(c.s3Bucket, keepSlash: false) + "/" + uriEncode(key, keepSlash: true)
        let hostHeader = base.port.map { "\(host):\($0)" } ?? host

        let df = DateFormatter()
        df.locale = Locale(identifier: "en_US_POSIX")
        df.timeZone = TimeZone(identifier: "UTC")
        df.dateFormat = "yyyyMMdd'T'HHmmss'Z'"
        let amzDate = df.string(from: Date())
        let day = String(amzDate.prefix(8))
        let payloadHash = hex(SHA256.hash(data: data))
        let type = mime(file)

        let signed = "content-type;host;x-amz-content-sha256;x-amz-date"
        let canonical = ["PUT", uri, "", "content-type:\(type)", "host:\(hostHeader)", "x-amz-content-sha256:\(payloadHash)",
                         "x-amz-date:\(amzDate)", "", signed, payloadHash].joined(separator: "\n")
        let scope = "\(day)/\(region)/s3/aws4_request"
        let toSign = ["AWS4-HMAC-SHA256", amzDate, scope, hex(SHA256.hash(data: Data(canonical.utf8)))].joined(separator: "\n")
        var k = hmac(Data(("AWS4" + c.s3SecretKey).utf8), day)
        k = hmac(k, region)
        k = hmac(k, "s3")
        k = hmac(k, "aws4_request")
        let sig = hex(hmac(k, toSign))

        var comps = URLComponents(url: base, resolvingAgainstBaseURL: false)!
        comps.percentEncodedPath = uri
        var req = URLRequest(url: comps.url!)
        req.httpMethod = "PUT"
        req.setValue(type, forHTTPHeaderField: "Content-Type")
        req.setValue(payloadHash, forHTTPHeaderField: "x-amz-content-sha256")
        req.setValue(amzDate, forHTTPHeaderField: "x-amz-date")
        req.setValue("AWS4-HMAC-SHA256 Credential=\(c.s3AccessKey)/\(scope), SignedHeaders=\(signed), Signature=\(sig)",
                     forHTTPHeaderField: "Authorization")
        _ = try await send(req, body: data)
        let pub = c.s3PublicUrl.trimmingCharacters(in: CharacterSet(charactersIn: "/ "))
        let root = pub.isEmpty ? base.absoluteString.trimmingCharacters(in: CharacterSet(charactersIn: "/")) + "/" + uriEncode(c.s3Bucket, keepSlash: false) : pub
        return root + "/" + uriEncode(key, keepSlash: true)
    }

    // Dotted JSON path lookup: "data.link", "files.0.url".
    static func jsonPath(_ data: Data, _ path: String) -> String? {
        guard var cur = try? JSONSerialization.jsonObject(with: data, options: [.fragmentsAllowed]) else { return nil }
        for part in path.split(separator: ".").map(String.init) {
            if let d = cur as? [String: Any], let v = d[part] { cur = v }
            else if let a = cur as? [Any], let i = Int(part), a.indices.contains(i) { cur = a[i] }
            else { return nil }
        }
        if let s = cur as? String { return s }
        if let n = cur as? NSNumber { return n.stringValue }
        return nil
    }
}
