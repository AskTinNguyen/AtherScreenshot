import CoreVideo
import XCTest
@testable import AtherScreenshot

final class GamepadTests: XCTestCase {
    // Port of gamepad_latch_keeps_a_tap_between_frames.
    func testLatchKeepsATapBetweenFrames() {
        var l = PadLatch()
        let a = PadState(connected: true, buttons: .a)
        let none = PadState(connected: true)
        l.feed(a)
        l.feed(none)  // released before the frame was drawn
        XCTAssertTrue(l.take().buttons.contains(.a))
        XCTAssertEqual(l.take().buttons, [])  // shown once, then gone
        var t = none
        t.lt = 0.8
        l.feed(t)
        l.feed(none)
        XCTAssertEqual(l.take().lt, 0.8, accuracy: 1e-6)
        l.feed(PadState())  // unplugged
        XCTAssertFalse(l.take().connected)
    }

    // Port of gamepad_stick_dead_zone_and_layout.
    func testStickDeadZoneAndLayout() {
        XCTAssertTrue(PadState.stick(3000 / 32767, -3000 / 32767, dead: 7849 / 32767) == (0, 0))
        XCTAssertEqual(PadState.stick(1, 0, dead: 7849 / 32767).0, 1, accuracy: 1e-4)
        XCTAssertEqual(PadState.trigger(0.05), 0)
        XCTAssertEqual(PadState.trigger(1), 1, accuracy: 1e-6)
        XCTAssertEqual(PadCorner.parse("TopLeft"), .topLeft)
        XCTAssertEqual(PadCorner.parse("nonsense"), .bottomRight)
        let big = Gamepad.layout(frameW: 1920, frameH: 1080, corner: .bottomRight, scale: 1)
        XCTAssertEqual(big.u * 240, 220, accuracy: 0.01)
        XCTAssertTrue(big.x + 240 * big.u <= 1920 && big.y + Gamepad.boxH * big.u <= 1080 && big.x > 1500 && big.y > 800)
        let tiny = Gamepad.layout(frameW: 300, frameH: 120, corner: .topLeft, scale: 2)  // never covers most of a small frame
        XCTAssertTrue(tiny.u * 240 <= 100 && tiny.u * Gamepad.boxH <= 60.01 && tiny.x < 10 && tiny.y < 10)
    }

    private func frame(_ w: Int = 640, _ h: Int = 360, gray: UInt8 = 128) -> CGContext {
        let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
        ctx.setFillColor(CGColor(srgbRed: CGFloat(gray) / 255, green: CGFloat(gray) / 255, blue: CGFloat(gray) / 255, alpha: 1))
        ctx.fill(CGRect(x: 0, y: 0, width: w, height: h))
        return ctx
    }

    // (r, g, b) at a top-left-origin pixel.
    private func px(_ c: CGContext, _ p: CGPoint) -> (Int, Int, Int) {
        let row = c.data!.assumingMemoryBound(to: UInt8.self) + Int(p.y) * c.bytesPerRow + Int(p.x) * 4
        return (Int(row[2]), Int(row[1]), Int(row[0]))
    }

    private func untouched(_ c: CGContext) -> Bool {
        let b = c.data!.assumingMemoryBound(to: UInt8.self)
        for i in stride(from: 0, to: c.bytesPerRow * c.height, by: 4) where b[i] != 128 || b[i + 1] != 128 || b[i + 2] != 128 { return false }
        return true
    }

    // Port of gamepad_draws_pressed_buttons_lit.
    func testDrawsPressedButtonsLit() throws {
        var s = PadState()
        let off = frame()
        Gamepad.draw(into: off, frameW: 640, frameH: 360, state: s, corner: .bottomRight, scale: 1, opacity: 1)
        XCTAssertTrue(untouched(off), "nothing connected: untouched")
        s.connected = true
        let idle = frame()
        Gamepad.draw(into: idle, frameW: 640, frameH: 360, state: s, corner: .bottomRight, scale: 1, opacity: 1)
        s.buttons = [.a, .right, .rightShoulder]
        s.lt = 1
        s.rt = 0.4
        s.lx = 0.7
        s.ly = 0.5
        let pressed = frame()
        Gamepad.draw(into: pressed, frameW: 640, frameH: 360, state: s, corner: .bottomRight, scale: 1, opacity: 1)
        let L = Gamepad.layout(frameW: 640, frameH: 360, corner: .bottomRight, scale: 1)
        let probe = L.at(Gamepad.aButton.x - 5.5, Gamepad.aButton.y)
        let a0 = px(idle, probe), a1 = px(pressed, probe)
        XCTAssertTrue(a1.1 > a1.0 + 60 && a1.1 > a1.2 + 60, "green when pressed: \(a1)")
        XCTAssertLessThan(a0.1, 120, "dark when not")
        let tp = L.at(Gamepad.leftTrigger.x, Gamepad.leftTrigger.y)
        let t0 = px(idle, tp), t1 = px(pressed, tp)
        XCTAssertTrue(t1.0 > 220 && t1.1 > 120 && t1.1 < 200 && t1.2 < 110, "the pulled trigger fills orange: \(t1)")
        XCTAssertTrue(t0.0 == 128 && t0.2 == 128, "and isn't drawn when not")
        // Half opacity lands halfway between the video and the controller.
        let half = frame()
        Gamepad.draw(into: half, frameW: 640, frameH: 360, state: s, corner: .bottomRight, scale: 1, opacity: 0.5)
        XCTAssertLessThanOrEqual(abs(px(half, probe).1 - (a1.1 + 128) / 2), 12)
        let none = frame()
        Gamepad.draw(into: none, frameW: 640, frameH: 360, state: s, corner: .bottomRight, scale: 1, opacity: 0)
        XCTAssertTrue(untouched(none))

        if let out = ProcessInfo.processInfo.environment["ATHER_TEST_OUT"] {
            let big = frame(1280, 720, gray: 0)
            big.setFillColor(CGColor(srgbRed: 0.23, green: 0.35, blue: 0.47, alpha: 1))
            big.fill(CGRect(x: 0, y: 0, width: 1280, height: 720))
            Gamepad.draw(into: big, frameW: 1280, frameH: 720, state: s, corner: .topLeft, scale: 3, opacity: 1)
            Gamepad.draw(into: big, frameW: 1280, frameH: 720, state: s, corner: .topRight, scale: 3, opacity: 0.6)
            var ps = s
            ps.playStation = true
            ps.buttons = [.b, .up]
            Gamepad.draw(into: big, frameW: 1280, frameH: 720, state: ps, corner: .bottomLeft, scale: 3, opacity: 1)
            try big.makeImage()!.pngData()!.write(to: URL(fileURLWithPath: out).appendingPathComponent("gamepad-large.png"))
            let rest = frame(1500, 720, gray: 250)
            Gamepad.draw(into: rest, frameW: 1500, frameH: 720, state: PadState(connected: true), corner: .topLeft, scale: 2.3, opacity: 1)
            try rest.makeImage()!.pngData()!.write(to: URL(fileURLWithPath: out).appendingPathComponent("gamepad-idle.png"))
        }
    }

    // The recorder draws into ScreenCaptureKit's BGRA buffers, and resends a still frame with a clean copy of
    // what's under the pad pasted back first.
    func testPixelBufferDrawAndCleanPatch() throws {
        var px: CVPixelBuffer?
        CVPixelBufferCreate(nil, 640, 360, kCVPixelFormatType_32BGRA, [kCVPixelBufferIOSurfacePropertiesKey: [:]] as CFDictionary, &px)
        let buf = try XCTUnwrap(px)
        CVPixelBufferLockBaseAddress(buf, [])
        memset(CVPixelBufferGetBaseAddress(buf)!, 0x80, CVPixelBufferGetBytesPerRow(buf) * 360)
        CVPixelBufferUnlockBaseAddress(buf, [])
        let rect = Gamepad.footprint(frameW: 640, frameH: 360, corner: .bottomRight, scale: 1)
        let clean = try XCTUnwrap(Recorder.copyRows(buf, rect))
        Gamepad.draw(into: buf, state: PadState(connected: true, buttons: .a), corner: .bottomRight, scale: 1, opacity: 1)
        let copy = try XCTUnwrap(Recorder.copy(buf))
        XCTAssertNotEqual(Recorder.copyRows(copy, rect), clean, "the pad was drawn")
        Recorder.pasteRows(copy, rect, clean)
        XCTAssertEqual(Recorder.copyRows(copy, rect), clean, "pasting the clean patch removes it")
    }
}
