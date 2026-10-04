# AtherScreenshot

A tiny, fast, personal ShareX replacement for Windows 11. Native C++20 + Win32/GDI/GDI+/WIC/Media Foundation/WinRT. A single exe, no dependencies, a few MB of memory at idle.

**macOS:** a native app lives in [`macos/`](macos/README.md) (`cd macos && ./build.sh`). Download: [AtherScreenshot-0.0.1-macOS.dmg](downloads/AtherScreenshot-0.0.1-macOS.dmg) (macOS 14+, Apple silicon and Intel; checksum in [`downloads/SHA256SUMS-macOS.txt`](downloads/SHA256SUMS-macOS.txt)). The build isn't notarized: after dragging it to Applications, open it once, then allow it in System Settings › Privacy & Security › "Open Anyway".

**Download (Windows):** [AtherScreenshot-Setup-0.0.1.exe](downloads/AtherScreenshot-Setup-0.0.1.exe) (Windows 10/11, x64), or the [portable zip](downloads/AtherScreenshot-0.0.1-portable.zip); checksums in [`downloads/SHA256SUMS-Windows.txt`](downloads/SHA256SUMS-Windows.txt). See [INSTALL.md](INSTALL.md). The exe isn't code-signed, so SmartScreen asks once: More info › Run anyway. To build it yourself, run `package.bat` (below).

## Build

```
build.bat          # release -> build\AtherScreenshot.exe
build.bat debug
```

## Package for the team

```
package.bat            # -> dist\AtherScreenshot-Setup-<ver>.exe, -portable.zip, SHA256SUMS.txt, INSTALL.md
package.bat publish    # also creates a GitHub Release with gh (needs a git repo with a GitHub remote)
```

- **Setup exe:** the exe is its own installer. Run it from anywhere and it offers INSTALL (per-user, no admin), UPDATE, or "Run without installing". Install adds a Start menu shortcut, an optional startup entry, and an uninstall entry in Settings › Apps (which runs `AtherScreenshot.exe --uninstall`). Captures and settings are kept on uninstall.
- **Version:** bump it in `src/version.h`. The exe metadata, the installer and the file names all follow it.
- **Icon:** `res/app.ico` is generated from the vector A⁵ logo in `src/logo.cpp`. Delete the file and `build.bat` regenerates it.
- **Signing:** builds aren't code-signed, so teammates see a SmartScreen "unknown publisher" prompt the first time. With a code-signing certificate, `set SIGN_PFX=…` and `set SIGN_PASSWORD=…` before `package.bat` to sign.
- **Dev builds:** `build\AtherScreenshot.portable` marks the dev build folder as portable, so `build\AtherScreenshot.exe` runs in place without offering to install.

Requires Visual Studio with the C++ workload (found automatically through vswhere).

## Default hotkeys

| Hotkey | Action |
|---|---|
| `Ctrl+Alt+K` | Command palette (every command below is in it; Ctrl+K inside closes it) |
| `PrintScreen` | Capture region |
| `Ctrl+PrintScreen` / `Ctrl+Shift+PrintScreen` | Capture all monitors / current monitor |
| `Alt+PrintScreen` | Capture active window |
| `Ctrl+Alt+E` | Capture region and annotate |
| `Ctrl+Alt+S` | Scrolling capture (long pages) |
| `Ctrl+Alt+U` | Capture region and upload (link copied) |
| `Ctrl+Alt+H` | Capture gallery |
| `Shift+PrintScreen` / `Ctrl+Alt+PrintScreen` | Record MP4 / GIF (press again to stop) |

Everything is configurable in the **Settings** window (palette → "Settings", or the tray menu):
- **Searchable:** type to filter the settings.
- **Saved instantly:** every change applies right away, no restart.
- **Reset:** right-click any setting to reset it.
- **Shortcuts:** click a shortcut field and press the keys (Esc cancels, Backspace clears). A shortcut already used by another command moves over to the new one. A shortcut taken by another app or Windows shows a warning on its row.

Values are stored in `%APPDATA%\AtherScreenshot\settings.ini`.

## Capturing

- **Region selector:** click snaps to the window or monitor under the cursor, drag selects a region, `Space` takes the monitor, `Ctrl+A` takes everything, arrows nudge 1 px (`Shift` = 10 px), `Esc` or right-click cancels.
- **Scrolling capture:** select the scrollable area. The app scrolls it with the mouse wheel and stitches the frames, keeping sticky headers and footers only once. It stops at the end, on `Esc`, or after `[Scrolling] MaxFrames`.
- **Screen ruler:** drag to measure W × H, length and angle (`Shift` snaps). Drag again for a new measurement; `C` copies it.
- **Auto-redact:** "Capture region with auto-redact", or `AutoRedact=1` for every capture. It runs OCR and pixelates emails, IPs, API keys/tokens/JWTs, card numbers (Luhn-checked) and phone numbers. It can only redact what Windows OCR reads; it skipped a line of repeated `1111` digits in testing, so check important redactions.
- **File names:** `FileNameTemplate` with `{yyyy} {MM} {dd} {HH} {mm} {ss} {ms} {app} {window} {w} {h}`. `AskForName=1` prompts after each capture; "Rename last capture…" renames afterwards.

## Annotation editor

Open it with `Ctrl+Alt+E`, by clicking the capture toast, from the gallery, from a pin's menu, with "Open image in editor…", or with `AtherScreenshot.exe edit <file>`.

| Key | Tool |
|---|---|
| `V` | Select: drag to move, `Delete` removes, `Ctrl+D` duplicates |
| `A` `L` `R` `E` `P` | Arrow, line, rectangle, ellipse, pen (`Shift` snaps) |
| `H` `T` `N` | Highlighter, text, step numbers |
| `B` `X` | Blur, pixelate |
| `S` | Spotlight: dims everything outside the areas you drag |
| `M` | Magnifier: drag from a detail to where the 2× bubble goes |
| `C` | Crop |
| `K` | Canvas: add space around the screenshot |
| `I` | Insert image |
| `1`–`8`, `[` `]`, wheel | Color, size |

Other shortcuts:
- `Ctrl+Z`/`Ctrl+Y` undo/redo, `Ctrl+C` copy, `Ctrl+S` save, `Ctrl+P` pin, `Enter` Done (copy + save + close).
- `Ctrl+R` auto-redact (adds editable pixelate boxes).
- `Ctrl+E` styled export: gradient backdrop, padding, rounded corners and a shadow.
- `Ctrl+Shift+C` / `Ctrl+Shift+V` copy or paste annotations between editors.
- `Ctrl+K` lists every editor command.

### Editor extras

- **Insert image (I):** drop a screenshot from the gallery or Explorer onto the editor, paste one with `Ctrl+V`, or press I to pick from recent captures.
  - It becomes a layer: drag to move, drag a corner to resize (`Shift` for free resize), and `Ctrl+]` / `Ctrl+[` to bring it forward or send it back.
  - Right-click it for rounded corners, shadow, border, opacity and actual size.
  - Blur and pixelate apply to layers too. Everything is flattened when you save.
- **Canvas (K):** drag any edge to add space around the screenshot (`Alt` moves both sides), or use Space below, Space right and Even margin.
  - The space is filled with the screenshot's edge color, white, black or transparent (saved as a 32-bit PNG).
  - Text wraps at the canvas edge, so notes in the margin stay on the image.
- **Collage (`Ctrl+G` in the gallery):** select two or more screenshots and press Collage.
  - Choose a layout (auto, grid, row, column, feature), gap, margin, background, corners, shadow and size (fit, 1920 px wide, square).
  - Drag one screenshot onto another to swap them; everything else in the editor works on top.
- **Saved results join the gallery:**
  - Edits are tagged "edited" and keep the original's metadata.
  - Collages are tagged "collage" and keep the tags and collections their screenshots share.
  - Both list the screenshots they include.

## Recording

Select a region, or use "Record a window", which follows one window wherever it moves, even when it's covered. A 3-2-1 countdown runs first (click it to cancel). The control bar has Pause/Resume, Stop and Discard. The bar, frame and countdown never appear in the video.

- **Capture:** frames come from Windows.Graphics.Capture (GPU), falling back to GDI for regions that span monitors or when `GpuCapture=0`.
- **MP4:** H.264 with the hardware encoder when available, plus AAC audio of the system sound (`RecordSystemAudio`) and/or the microphone (`RecordMicrophone`), mixed and kept in sync. Pauses are cut out.
- **GIF:** per-frame palettes; identical frames are merged.
- **Click and keystroke overlay:** click ripples (`ShowClicks`) and a keystroke pill (`ShowKeys`, off by default because it would show anything you type, including passwords).
- When a recording finishes, the file is copied to the clipboard so you can paste it into chats.

## Video editor

Opening an MP4 (from the gallery, the "Recording saved" notification, or Open recent in the palette) opens a small editor instead of a player:

- **Trim:** drag the yellow handles on the timeline, or press I and O at the playhead.
- **Crop:** press C and drag on the video; pick Free, 16:9, 4:3, 1:1 or 9:16 to keep a shape.
- **Speed** 0.5–4× (audio keeps its pitch) and **Mute**.
- **Captions:**
  - T adds one at the playhead. Type in the field, drag it on the timeline to move it or its edges to retime it, and choose top, middle or bottom.
  - **Auto captions** transcribes the speech on this PC with Windows speech recognition and splits it into short captions.
  - If no recognizer is installed for your language, it says so and points to Settings › Time & language › Speech.
- **Markup (Add ▾):**
  - Kinds: text, speech bubbles, emoji (quick picks; Win+. in the field for any other), arrows, boxes, ellipses, step numbers, blur or pixelate (to hide private info for a stretch of time), zoom (smoothly zooms into a box, shown while playing) and full-screen title cards.
  - Each item has its own bar on the timeline: drag to move, drag its edges to retime.
  - On the video, drag to move and drag the handles to resize.
  - Shortcuts: A arrow, R box, E emoji, N step, X blur, Z zoom.
- **Animation:** one menu per item.
  - "Auto" picks a sensible style for the kind: arrows and boxes draw on, emoji and bubbles pop, text slides up, title cards fade.
  - Or choose Fade, Pop, Scale, Slide up, Wipe, Blur in, Draw on or Typewriter, which plays in and out.
  - Optional while on screen: Pulse, Bounce, Shake or Ping.
  - Advanced: a different exit, and apply to all items of the same kind.
  - ▶ replays the item.
- **Captions ▾** sets captions for the whole video: look (pill, outline or bar), animation, size (five steps), text color, box or outline color, and highlighting the spoken word (auto captions). The same style controls show when a caption is selected.
- **Placing captions:** drag a caption on the video to place it anywhere; Top, Middle and Bottom snap it back to a preset.
- **Preview:** the preview and the export use the same frame renderer, so what you see is what's saved. While paused, the selected item is shown fully rather than mid-animation, so a new caption is visible at the playhead.
- **Saving:**
  - **Save** (`Ctrl+S`) writes a new MP4 (H.264 + AAC).
  - **Save GIF** (`Ctrl+Shift+S`) writes a GIF (12 fps, at most 960 px).
  - The original stays untouched, and the result stacks with it in the gallery.

Space plays, ←/→ step a frame (`Shift`: a second), `Del` deletes the selected item, `Ctrl+Z` undoes, `Esc` leaves crop, then deselects, then closes.

## Capture gallery

`Ctrl+Alt+H`. An Eagle-style library for everything in the captures folder. Metadata lives in `%APPDATA%\AtherScreenshot\library.json`, in the same format as the Mac app's, and image features in `features.bin`. The image files are never modified.

- **Organize:**
  - Tags (T) and collections (F, or drag captures onto a collection). A capture can be in several collections, and a collection can add tags automatically to everything dropped into it.
  - Smart folders save the current search and filters as a live folder.
  - Star ratings (1–5, 0 clears), comments, and batch rename (`{n} {name} {date} {app}`).
  - Duplicates finds identical or near-identical captures, with "keep newest, trash the rest".
- **Search:**
  - Text matches the file name, OCR text, tags, comments, source app and window title.
  - Filters cover type, tags (all/any/untagged), rating, color, shape (landscape, portrait, square, panorama, long page), minimum dimensions, date, file size and source app.
  - **Find similar** is a reverse image search on a perceptual hash plus a color and structure fingerprint.
- **Layout:**
  - The window is just your captures under one toolbar (scope menu, search, filters, view options).
  - Filter chips appear only while a filter is active, and an action bar floats in while something is selected.
  - The sidebar (`Ctrl+B`) and the details inspector (`Ctrl+I`) stay hidden until you ask for them.
- **Browse:**
  - Justified, grid or list layout; `Ctrl+=` / `Ctrl+−` change the thumbnail size.
  - Sort by date, name, size, dimensions, rating or random (shuffle).
  - The inspector shows tags, rating, comment, collections, the color palette (click a color to search by it), info and OCR text.
  - Space opens a full-window preview with zoom, playback for GIFs and videos, ←/→ navigation and a slideshow.
  - Drag captures out to other apps, or drop files in to import them.
- **Suggested tags:**
  - Each capture gets suggestions from its source app, the text in it, its shape and colors (code, terminal, error, chat, email, design, web, receipt, login, dashboard, mobile, dark…).
  - They're searchable right away.
  - Click one to add it, right-click to stop suggesting it, or turn on Settings › Gallery › Tag captures automatically.
- **Search by meaning:**
  - Words also match related words from a curated screenshot vocabulary: "graph" finds charts, "login" finds sign-in screens.
  - Phrases like "yesterday" or "last week" filter by date.
  - Related matches come after exact ones.
- **Versions:**
  - An edited capture stacks with its original; the badge shows how many versions there are and opens the stack.
  - C compares two captures (or an edit with its original) with a before/after slider or side by side.
- **Indexing:**
  - Dimensions, palette, perceptual hash, image fingerprint and OCR text are computed in the background, and a small progress ring shows in the toolbar.
  - New and moved files are picked up as they appear.
  - Captures remember which app and window they came from.

| Key | Action |
|---|---|
| Space | Preview (←/→ next/previous, Space/Esc close) |
| T / F | Add tags / add to collection |
| 1–5, 0 | Rate / clear rating |
| Enter | Annotate, edit video, or open |
| C | Compare |
| `Ctrl+F` or `/` | Search |
| `Ctrl+A`, `Shift`+arrows, `Ctrl`/`Shift`+click | Select several |
| `Ctrl+C` `Ctrl+T` `Ctrl+P` `Ctrl+R` `Ctrl+U` `Ctrl+O` `Del` | Copy, copy text, pin, rename/batch rename, upload, show in Explorer, Recycle Bin |
| `Ctrl+G` | Make a collage |
| `Ctrl+Shift+S` | Save the filters as a smart folder |
| `Ctrl+I` / `Ctrl+B` | Toggle the inspector / sidebar |
| `Ctrl+/` or `?` | Keyboard shortcuts |
| `Ctrl+K` | All actions |

## Upload

Set `[Upload] Uploader=` to one of:
- `imgur` + `ImgurClientId`
- `custom`: a multipart POST to `CustomUrl` with optional `CustomHeaders` (`Name: value; Name2: value2`). The link comes from the JSON path `CustomResponseUrl` (e.g. `data.link`), or from the plain-text body if that's empty.
- `s3`: AWS S3, Cloudflare R2, MinIO… signed with SigV4. Uses `S3Endpoint`, `S3Bucket`, `S3Region`, the keys, and an optional `S3PublicUrl`.

The link is copied to the clipboard. You can also set `AfterCapture=upload`.

## Command line and links

```
AtherScreenshot.exe region|fullscreen|monitor|window|last|scroll|ruler|ocr|color
                    record|gif|recordwindow|stop|pause|history|palette|folder|quit
                    [--pin|--edit|--upload|--redact|--ocr] [--delay N]
AtherScreenshot.exe edit|pin|upload <file>
start atherscreenshot://region?pin
```

Any `[Hotkeys]` key name works as a command too (e.g. `CaptureRegionPin`). If the app is already running, the command goes to that instance, so you can drive it from AutoHotkey, Stream Deck, scripts, or "Open with".

Commands typed in a terminal run directly. Commands from `atherscreenshot://` links (which any web page can open) or from tools without a console ask for confirmation first. They can never upload, or open, pin or edit files, even when "After capture" is set to upload.

## Differences from the Mac app

- **Shortcuts:**
  - Ctrl replaces ⌘.
  - The gallery sidebar is `Ctrl+B`, because `Ctrl+Shift+S` saves a smart folder.
  - PrintScreen, Pause and ScrollLock work as shortcuts on their own; other keys need Ctrl, Alt or Win.
- **Gallery:**
  - Find similar uses a perceptual hash plus a color and structure fingerprint, because Windows has no built-in image feature print.
  - Related words come from a curated vocabulary only, without an on-device word model.
  - Windows process names (msedge, Code, WindowsTerminal…) count as the matching apps for suggested tags.
  - The library is scanned a few seconds after startup, so the app starts instantly.
  - The window is opaque dark rather than translucent.
- **Video editor:**
  - Auto captions use Windows speech recognition (SAPI dictation), on this PC.
  - The emoji field uses Win+. for the full picker.
  - The selected item shows fully while paused (the Mac version shows its first, faded frame).
- **Recording:** system audio and microphone are mixed into one AAC track (two tracks on the Mac).

## Layout

- `src/main.cpp`: commands, hotkeys, tray, capture pipeline, CLI and links.
- **Capture and recording:**
  - `overlay`: region, color picker, ruler
  - `recorder`: MP4/GIF sessions
  - `wgc`: GPU capture
  - `audio`: WASAPI
  - `inputviz`: click and key overlay
  - `scroll`: scrolling capture
  - `capture`
- **Gallery:**
  - `library`: library.json, indexer, folder watching, filters
  - `smart`: suggested tags, related words, dates
  - `gallery_model`: selection, stacks, layouts
  - `gallery`: the window
  - `json`
- **Editors:**
  - `editor`: annotation, image layers, canvas
  - `collage`: layouts
  - `videoedit`: the video model and the frame renderer
  - `videoio`: decode, export, preview playback, speech to text
  - `videoeditor`: the window
  - `textdraw`: DirectWrite text
  - `annot.h`: the shared drawing code
- **UI and output:**
  - `palette`: Ctrl+K and the text prompt
  - `toast`, `pin`, `settingsui`
  - `output`: clipboard, PNG, naming
  - `media`: image and video decoding, MP4/GIF writers
  - `ocr`: text, words, redaction
  - `upload`: Imgur, custom, S3
  - `settings`
- **Tests:** `AtherScreenshot.exe --selftest [filter]` or `test.bat [--nobuild] [filter]`. Tests use a temporary support folder and never touch `%APPDATA%`.
