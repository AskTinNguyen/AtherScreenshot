# Ather Screenshot for macOS

Native Swift/AppKit port of the Windows app: a menu-bar agent that uses ScreenCaptureKit for capture and recording, Vision for OCR, AVFoundation for MP4, ImageIO for GIF and Carbon for global hotkeys. It has no dependencies, needs macOS 14 or later, and builds to a ~1.6 MB binary.

## Build

```
./build.sh            # release → build/Ather Screenshot.app
./build.sh debug
./build.sh package    # universal arm64 + x86_64, plus dist/*.zip, *.dmg, SHA256SUMS.txt
xcrun swift test      # unit tests; ATHER_TEST_OUT=<dir> also renders the editor, palette, toast and settings to PNGs
```

Requires Xcode (or the Command Line Tools). The version comes from `../src/version.h`, shared with the Windows build. Builds are ad-hoc signed; set `SIGN_IDENTITY="Developer ID Application: …"` (and `NOTARY_PROFILE` for `package`) to sign and notarize.

## Permissions

- **Screen & System Audio Recording** (required): macOS asks on first capture. Ad-hoc builds get a new signature on every rebuild, so macOS may ask again after rebuilding. Sign with a stable identity to avoid that.
- **Accessibility**: only for scrolling capture (it posts scroll events) and for showing keystrokes in recordings.
- **Microphone**: only when "Record microphone" is on (macOS 15 or later).

## Default shortcuts

macOS reserves ⌘⇧3/4/5, so the defaults use ⌃⌥:

| Shortcut | Action |
|---|---|
| ⌃⌥K | Command palette (every command; ⌘K inside closes it) |
| ⌃⌥4 | Capture region |
| ⌃⌥3 / ⌃⌥⇧3 | Capture all displays / current display |
| ⌃⌥W | Capture active window |
| ⌃⌥E | Capture region and annotate |
| ⌃⌥S | Scrolling capture |
| ⌃⌥U | Capture region and upload |
| ⌃⌥T | Capture text (OCR) |
| ⌃⌥H | Capture history |
| ⌃⌥5 / ⌃⌥G | Record MP4 / GIF (press again to stop) |

All shortcuts can be changed in Settings. Settings live in UserDefaults (`com.ather.screenshot`) under the same key names as `settings.ini`, so `defaults write com.ather.screenshot Uploader s3` works.

## Differences from Windows

- In the editor, ⌘ replaces Ctrl (⌘Z, ⌘C, ⌘S, ⌘P, ⌘R redact, ⌘E styled export, ⌘K commands). Single-key tools, 1–8 colors, `[` `]` sizes and ↩ Done are unchanged.
- History: ⌘⌫ moves to Trash (instead of Del), and ⌘O shows the file in Finder.
- System audio and microphone are recorded as two AAC tracks instead of one mixed track.
- A recorded region that spans displays is clamped to the display under its center.
- There is no installer: drag the app to /Applications. "Launch at login" uses SMAppService.

## Command line and URL scheme

```
"Ather Screenshot.app/Contents/MacOS/AtherScreenshot" region|fullscreen|monitor|window|last|scroll|ruler|ocr|color
     record|gif|recordwindow|stop|pause|history|palette|folder|settings|quit [--pin|--edit|--upload|--redact|--ocr] [--delay N]
AtherScreenshot edit|pin|upload <file>
open "atherscreenshot://region?pin"
```

If the app is already running, the command goes to that instance.

## Layout (`Sources/AtherScreenshot`)

`App` (commands, menu bar, hotkeys, capture pipeline, CLI) · `Capture` (ScreenCaptureKit snapshots, window list) · `Overlay` (region, window, color, ruler) · `Editor` + `Annotations` · `Recorder` (MP4/GIF, countdown, control bar, click/key overlay) · `Scroll` · `OCR` (Vision, redaction) · `Upload` (Imgur, custom, S3 SigV4) · `Palette` · `Pin` · `Toast` · `History` · `SettingsWindow` · `Hotkeys` · `Logo` · `Output`.
