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

## Capture gallery (⌃⌥H)

An Eagle-style library for everything in the captures folder. Metadata lives in `~/Library/Application Support/AtherScreenshot/library.json` (feature prints in `features.plist`); the image files are never modified.

- **Organize:** tags (T), collections (F, or drag captures onto a collection). A capture can be in several collections, and a collection can add tags automatically to everything dropped into it. Smart folders save the current search and filters as a live folder. Also star ratings (1–5, 0 clears), comments, batch rename (`{n} {name} {date} {app}`), and Duplicates (identical or near-identical captures, with "keep newest, trash the rest").
- **Search:** text (file name, OCR text, tags, comments, source app, window title), plus filters for type, tags (all/any/untagged), rating, color, shape (landscape, portrait, square, panorama, long page), minimum dimensions, date, file size and source app. **Find similar** is a reverse image search using Vision feature prints.
- **Browse:** justified, grid or list layout with a thumbnail size slider. Sort by date, name, size, dimensions, rating or random (shuffle). The inspector shows tags, rating, comment, collections, color palette (click a color to search by it), info and OCR text. Space opens a full-window preview with zoom, playback for GIFs and videos, ←/→ navigation and a slideshow. Drag captures out to other apps, or drop files in to import them.
- **Indexing:** dimensions, palette, perceptual hash, feature print and OCR text are computed in the background; progress shows at the bottom of the sidebar. Captures remember which app and window they came from.

| Key | Action |
|---|---|
| Space | Preview (←/→ next/previous, Space/Esc close) |
| T / F | Add tags / add to collection |
| 1–5, 0 | Rate / clear rating |
| ↩ | Annotate or open |
| ⌘F or / | Search |
| ⌘A, ⇧-arrows, ⌘/⇧-click | Select several |
| ⌘C ⌘T ⌘P ⌘R ⌘U ⌘O ⌘⌫ | Copy, copy text, pin, rename/batch rename, upload, show in Finder, Trash |
| ⌘⇧S | Save the filters as a smart folder |
| ⌘I | Toggle the inspector |
| ⌘K | All actions |

## Differences from Windows

- In the editor, ⌘ replaces Ctrl (⌘Z, ⌘C, ⌘S, ⌘P, ⌘R redact, ⌘E styled export, ⌘K commands). Single-key tools, 1–8 colors, `[` `]` sizes and ↩ Done are unchanged.
- History became the capture gallery above (⌘⌫ moves to Trash, ⌘O shows in Finder).
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

Commands typed in a terminal run directly. Commands from `atherscreenshot://` links (which any web page can open) or from tools without a terminal (Shortcuts, Raycast, cron…) ask for confirmation first. They can never upload, or open, pin or edit files, even when "After capture" is set to upload.

## Layout (`Sources/AtherScreenshot`)

`App` (commands, menu bar, hotkeys, capture pipeline, CLI) · `Capture` (ScreenCaptureKit snapshots, window list) · `Overlay` (region, window, color, ruler) · `Editor` + `Annotations` · `Library` (gallery metadata, indexer, filters) · `Gallery` · `Recorder` (MP4/GIF, countdown, control bar, click/key overlay) · `Scroll` · `OCR` (Vision, redaction) · `Upload` (Imgur, custom, S3 SigV4) · `Palette` · `Pin` · `Toast` · `SettingsWindow` · `Hotkeys` · `Logo` · `Output`.
