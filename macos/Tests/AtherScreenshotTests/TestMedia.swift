import AVFoundation
import AppKit
@testable import AtherScreenshot

// Test videos, written to the temporary folder (never the captures or support folder).
enum TestMedia {
    // `paint(frame, ctx)` draws each frame (top-left origin, stored size). `rotation`: the track's clockwise turn
    // (a phone video stored sideways). `tone`: a 440 Hz sine on an audio track.
    static func clip(w: Int = 640, h: Int = 360, fps: Int = 30, seconds: Double = 3, rotation: Int = 0, tone: Bool = false,
                     paint: (Int, CGContext) -> Void) async throws -> URL {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("ather-clip-\(UUID().uuidString).mp4")
        let writer = try AVAssetWriter(outputURL: url, fileType: .mp4)
        let input = AVAssetWriterInput(mediaType: .video, outputSettings: [AVVideoCodecKey: AVVideoCodecType.h264, AVVideoWidthKey: w, AVVideoHeightKey: h])
        input.transform = CGAffineTransform(rotationAngle: CGFloat(rotation) * .pi / 180)
        let ad = AVAssetWriterInputPixelBufferAdaptor(assetWriterInput: input, sourcePixelBufferAttributes: [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA, kCVPixelBufferWidthKey as String: w, kCVPixelBufferHeightKey as String: h])
        writer.add(input)
        var audio: AVAssetWriterInput?
        if tone {
            let a = AVAssetWriterInput(mediaType: .audio, outputSettings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48000, AVNumberOfChannelsKey: 1, AVEncoderBitRateKey: 64000])
            writer.add(a)
            audio = a
        }
        writer.startWriting()
        writer.startSession(atSourceTime: .zero)
        // Video and sound go in interleaved: the writer stops taking one while the other lags too far behind.
        let n = Int(seconds * Double(fps))
        let rate = 48000.0, total = Int(seconds * rate)
        var at = 0
        for i in 0..<n {
            while !input.isReadyForMoreMediaData { try await Task.sleep(nanoseconds: 2_000_000) }
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, ad.pixelBufferPool!, &pb)
            CVPixelBufferLockBaseAddress(pb!, [])
            let ctx = CGContext(data: CVPixelBufferGetBaseAddress(pb!), width: w, height: h, bitsPerComponent: 8, bytesPerRow: CVPixelBufferGetBytesPerRow(pb!),
                                space: CGColorSpace(name: CGColorSpace.sRGB)!, bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
            ctx.translateBy(x: 0, y: CGFloat(h))
            ctx.scaleBy(x: 1, y: -1)
            paint(i, ctx)
            CVPixelBufferUnlockBaseAddress(pb!, [])
            ad.append(pb!, withPresentationTime: CMTime(value: CMTimeValue(i), timescale: CMTimeScale(fps)))
            if let a = audio {
                let upTo = i == n - 1 ? total : min(total, Int((Double(i + 1) / Double(fps) + 1) * rate))  // sound a second ahead
                while at < upTo {
                    while !a.isReadyForMoreMediaData { try await Task.sleep(nanoseconds: 2_000_000) }
                    let k = min(4800, upTo - at)
                    var samples = [Int16](repeating: 0, count: k)
                    for j in 0..<k { samples[j] = Int16(sin(2 * .pi * 440 * Double(at + j) / rate) * 12000) }
                    a.append(pcm(samples, at: at, rate: rate))
                    at += k
                }
                if at >= total { a.markAsFinished(); audio = nil }  // the writer holds video back while sound may still come
            }
        }
        input.markAsFinished()
        await writer.finishWriting()
        if writer.status != .completed { throw writer.error ?? VideoSource.Failure.noVideo }
        return url
    }

    private static func pcm(_ s: [Int16], at: Int, rate: Double) -> CMSampleBuffer {
        var asbd = AudioStreamBasicDescription(mSampleRate: rate, mFormatID: kAudioFormatLinearPCM, mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                               mBytesPerPacket: 2, mFramesPerPacket: 1, mBytesPerFrame: 2, mChannelsPerFrame: 1, mBitsPerChannel: 16, mReserved: 0)
        var fmt: CMAudioFormatDescription?
        CMAudioFormatDescriptionCreate(allocator: nil, asbd: &asbd, layoutSize: 0, layout: nil, magicCookieSize: 0, magicCookie: nil, extensions: nil, formatDescriptionOut: &fmt)
        var block: CMBlockBuffer?
        let bytes = s.count * 2
        CMBlockBufferCreateWithMemoryBlock(allocator: nil, memoryBlock: nil, blockLength: bytes, blockAllocator: nil, customBlockSource: nil, offsetToData: 0, dataLength: bytes, flags: 0, blockBufferOut: &block)
        s.withUnsafeBytes { _ = CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block!, offsetIntoDestination: 0, dataLength: bytes) }
        var sb: CMSampleBuffer?
        CMAudioSampleBufferCreateReadyWithPacketDescriptions(allocator: nil, dataBuffer: block!, formatDescription: fmt!, sampleCount: s.count,
                                                             presentationTimeStamp: CMTime(value: CMTimeValue(at), timescale: CMTimeScale(rate)), packetDescriptions: nil, sampleBufferOut: &sb)
        return sb!
    }

    // A clip whose color changes every second (red, green, blue, …).
    static func colors(w: Int = 640, h: Int = 360, seconds: Double = 3, colors: [NSColor] = [.red, .green, .blue], tone: Bool = false) async throws -> URL {
        try await clip(w: w, h: h, seconds: seconds, tone: tone) { i, ctx in
            ctx.setFillColor(colors[min(colors.count - 1, i / 30)].cgColor)
            ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        }
    }

    static func edit(_ clips: [Clip]) -> VideoEdit {
        VideoEdit(trimEnd: VideoSequence.total(clips), clips: clips, frame: CGSize(width: clips[0].w, height: clips[0].h))
    }

    // (r, g, b) at a top-left-origin point.
    static func rgb(_ img: CGImage, _ x: Int, _ y: Int) -> (CGFloat, CGFloat, CGFloat) {
        let c = img.color(atPixel: CGPoint(x: x, y: y))!.usingColorSpace(.sRGB)!
        return (c.redComponent, c.greenComponent, c.blueComponent)
    }

    static func frame(_ asset: AVAsset, at t: Double, video: AVVideoComposition? = nil, upright: Bool = true) async throws -> CGImage {
        let gen = AVAssetImageGenerator(asset: asset)
        gen.appliesPreferredTrackTransform = upright
        gen.videoComposition = video
        gen.requestedTimeToleranceBefore = .zero
        gen.requestedTimeToleranceAfter = .zero
        return try await gen.image(at: CMTime(seconds: t, preferredTimescale: 600)).image
    }
}
