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

## Editor extras

- **Insert image (I):** drop a screenshot from the gallery or Finder onto the editor, paste one with ⌘V, or press I to pick from recent captures. It becomes a layer: drag to move, drag a corner to resize (⇧ for free resize), ⌘] / ⌘[ to bring forward or send back, and right-click for rounded corners, shadow, border and opacity. Blur and pixelate apply to layers too. Everything is flattened when you save.
- **Canvas (K):** drag any edge to add space around the screenshot (⌥ moves both sides), or use Space below, Space right and Even margin. The space is filled with the screenshot's edge color, white, black or transparent. Text wraps at the canvas edge, so notes in the margin stay on the image.
- **Collage (⌘G in the gallery):** select two or more screenshots and press Collage. Choose a layout (auto, grid, row, column, feature), gap, margin, background, corners, shadow and size (fit, 1920 px wide, square). Drag one screenshot onto another to swap them; everything else in the editor works on top.
- Saved results join the gallery: edits are tagged "edited" and keep the original's metadata; collages are tagged "collage" and keep the tags and collections their screenshots share. Both list the screenshots they include.

## Video editor

Opening an MP4 (from the gallery, the "Video saved" notification, or Open recent) opens a small editor instead of a player:

- **Trim:** drag the yellow handles on the timeline, or press I and O at the playhead.
- **Crop:** press C and drag on the video; pick Free, 16:9, 4:3, 1:1 or 9:16 to keep a shape.
- **Speed** 0.5–4× (audio keeps its pitch) and **Mute**.
- **Captions:** T adds one at the playhead; type in the field, drag it on the timeline to move it or its edges to retime it, choose top, middle or bottom and the text size. **Auto captions** transcribes the speech with macOS speech recognition (on this Mac when supported) and splits it into short captions.
- **Markup (Add ▾):** text, speech bubbles, emoji (quick picks or the full emoji picker), arrows, boxes, ellipses, step numbers, blur or pixelate (to hide private info for a stretch of time), zoom (smoothly zooms into a box, shown while playing) and full-screen title cards. Each item has its own bar on the timeline: drag to move, drag its edges to retime. On the video, drag to move and drag the handles to resize. Shortcuts: A arrow, R box, E emoji, N step, X blur, Z zoom.
- **Animation:** one menu per item. "Auto" picks a sensible style for the kind (arrows and boxes draw on, emoji and bubbles pop, text slides up, title cards fade); or choose Fade, Pop, Scale, Slide up, Wipe, Blur in, Draw on or Typewriter, which plays in and out. Optional while on screen: Pulse, Bounce, Shake or Ping. Advanced: a different exit, and apply to all items of the same kind. ▶ replays the item.
- **Captions ▾** sets captions for the whole video: look (pill, outline or bar), animation, size (five steps), text color, box or outline color, and highlighting the spoken word (auto captions). The same style controls show when a caption is selected.
- Drag a caption on the video to place it anywhere; Top, Middle and Bottom snap it back to a preset.
- The preview and the export use the same frame renderer, so what you see is what's saved.
- **Save** (⌘S) writes a new MP4; **Save GIF** (⌘⇧S) writes a GIF. The original stays untouched, and the result stacks with it in the gallery.

Space plays, ←/→ step a frame (⇧ a second), ⌫ deletes the selected caption, ⌘Z undoes.

## Capture gallery (⌃⌥H)

An Eagle-style library for everything in the captures folder. Metadata lives in `~/Library/Application Support/AtherScreenshot/library.json` (feature prints in `features.plist`); the image files are never modified.

- **Organize:** tags (T), collections (F, or drag captures onto a collection). A capture can be in several collections, and a collection can add tags automatically to everything dropped into it. Smart folders save the current search and filters as a live folder. Also star ratings (1–5, 0 clears), comments, batch rename (`{n} {name} {date} {app}`), and Duplicates (identical or near-identical captures, with "keep newest, trash the rest").
- **Search:** text (file name, OCR text, tags, comments, source app, window title), plus filters for type, tags (all/any/untagged), rating, color, shape (landscape, portrait, square, panorama, long page), minimum dimensions, date, file size and source app. **Find similar** is a reverse image search using Vision feature prints.
- **Layout:** the window is just your captures under one floating toolbar (scope menu, search, filters, view options). Filter chips appear only while a filter is active, and an action bar floats in while something is selected. The sidebar (⌃⌘S) and the details inspector (⌘I) are hidden until you ask for them.
- **Browse:** justified, grid or list layout; pinch or ⌘+/⌘− for thumbnail size. Sort by date, name, size, dimensions, rating or random (shuffle). The inspector shows tags, rating, comment, collections, color palette (click a color to search by it), info and OCR text. Space opens a full-window preview with zoom, playback for GIFs and videos, ←/→ navigation and a slideshow. Drag captures out to other apps, or drop files in to import them.
- **Suggested tags:** each capture gets suggestions from its source app, the text in it, its shape and colors (code, terminal, error, chat, email, design, web, receipt, login, dashboard, mobile, dark…). They're searchable right away; click one to add it, right-click to stop suggesting it, or turn on Settings › Gallery › Tag captures automatically.
- **Search by meaning:** words also match related words (on-device word model plus a screenshot vocabulary: "graph" finds charts, "login" finds sign-in screens), and phrases like "yesterday" or "last week" filter by date. Related matches come after exact ones.
- **Versions:** an edited capture stacks with its original; the badge shows how many versions there are and opens the stack. C compares two captures (or an edit with its original) with a before/after slider or side by side.
- **Indexing:** dimensions, palette, perceptual hash, feature print and OCR text are computed in the background; a small progress ring shows in the toolbar. Captures remember which app and window they came from.

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
| ⌘I / ⌃⌘S | Toggle the inspector / sidebar |
| ⌘+ ⌘− | Thumbnail size |
| ⌘/ or ? | Keyboard shortcuts |
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

`App` (commands, menu bar, hotkeys, capture pipeline, CLI) · `Capture` (ScreenCaptureKit snapshots, window list) · `Overlay` (region, window, color, ruler) · `Editor` + `Annotations` · `Collage` (layouts) · `VideoEditor` (trim, crop, speed, captions, markup UI) · `VideoRender` (frame renderer for preview and export) · `Smart` (suggested tags, related-word search) · `Library` (gallery metadata, indexer, filters) · `Gallery` · `Recorder` (MP4/GIF, countdown, control bar, click/key overlay) · `Scroll` · `OCR` (Vision, redaction) · `Upload` (Imgur, custom, S3 SigV4) · `Palette` · `Pin` · `Toast` · `SettingsWindow` · `Hotkeys` · `Logo` · `Output`.
