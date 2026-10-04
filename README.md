# AtherScreenshot

A tiny, fast, personal ShareX replacement for Windows 11. Native C++20 + Win32/GDI/GDI+/WIC/Media Foundation/WinRT. A single exe, no dependencies, a few MB of memory at idle.

**macOS:** a native app lives in [`macos/`](macos/README.md) (`cd macos && ./build.sh`). Download: [AtherScreenshot-0.0.1-macOS.dmg](downloads/AtherScreenshot-0.0.1-macOS.dmg) (macOS 14+, Apple silicon and Intel; checksum in [`downloads/SHA256SUMS-macOS.txt`](downloads/SHA256SUMS-macOS.txt)). The build isn't notarized: after dragging it to Applications, open it once, then allow it in System Settings › Privacy & Security › "Open Anyway".

**Download (Windows):** there's no published release yet; build it with `package.bat` (below), which produces `AtherScreenshot-Setup-<version>.exe`, and see [INSTALL.md](INSTALL.md).

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
| `Ctrl+Alt+H` | Capture history |
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

Open it with `Ctrl+Alt+E`, by clicking the capture toast, from history, from a pin's menu, with "Open image in editor…", or with `AtherScreenshot.exe edit <file>`.

| Key | Tool |
|---|---|
| `V` | Select: drag to move, `Delete` removes, `Ctrl+D` duplicates |
| `A` `L` `R` `E` `P` | Arrow, line, rectangle, ellipse, pen (`Shift` snaps) |
| `H` `T` `N` | Highlighter, text, step numbers |
| `B` `X` | Blur, pixelate |
| `S` | Spotlight: dims everything outside the areas you drag |
| `M` | Magnifier: drag from a detail to where the 2× bubble goes |
| `C` | Crop |
| `1`–`8`, `[` `]`, wheel | Color, size |

Other shortcuts:
- `Ctrl+Z`/`Ctrl+Y` undo/redo, `Ctrl+C` copy, `Ctrl+S` save, `Ctrl+P` pin, `Enter` Done (copy + save + close).
- `Ctrl+R` auto-redact (adds editable pixelate boxes).
- `Ctrl+E` styled export: gradient backdrop, padding, rounded corners and a shadow.
- `Ctrl+Shift+C` / `Ctrl+Shift+V` copy or paste annotations between editors.
- `Ctrl+K` lists every editor command.

## Recording

Select a region, or use "Record a window", which follows one window wherever it moves, even when it's covered. A 3-2-1 countdown runs first (click it to cancel). The control bar has Pause/Resume, Stop and Discard. The bar, frame and countdown never appear in the video.

- **Capture:** frames come from Windows.Graphics.Capture (GPU), falling back to GDI for regions that span monitors or when `GpuCapture=0`.
- **MP4:** H.264 with the hardware encoder when available, plus AAC audio of the system sound (`RecordSystemAudio`) and/or the microphone (`RecordMicrophone`), mixed and kept in sync. Pauses are cut out.
- **GIF:** per-frame palettes; identical frames are merged.
- **Click and keystroke overlay:** click ripples (`ShowClicks`) and a keystroke pill (`ShowKeys`, off by default because it would show anything you type, including passwords).
- When a recording finishes, the file is copied to the clipboard so you can paste it into chats.

## Capture history

`Ctrl+Alt+H`. A thumbnail grid of every PNG, GIF and MP4, newest first.

- **Search:** type to filter by name, date, type, or the text inside screenshots (OCR'd in the background and cached in `ocr-index.txt`).
- **Actions:** `Enter` annotates or opens, `Ctrl+C` copy, `Ctrl+P` pin, `Ctrl+R` rename, `Ctrl+U` upload, `Ctrl+T` copy text, `Ctrl+O` show in Explorer, `Del` moves to the Recycle Bin, `Ctrl+K` / right-click for everything.

## Upload

Set `[Upload] Uploader=` to one of:
- `imgur` + `ImgurClientId`
- `custom`: a multipart POST to `CustomUrl` with optional `CustomHeaders` (`Name: value; Name2: value2`). The link comes from the JSON path `CustomResponseUrl` (e.g. `data.link`), or from the plain-text body if that's empty.
- `s3`: AWS S3, Cloudflare R2, MinIO… signed with SigV4. Uses `S3Endpoint`, `S3Bucket`, `S3Region`, the keys, and an optional `S3PublicUrl`.

The link is copied to the clipboard. You can also set `AfterCapture=upload`.

## Command line

```
AtherScreenshot.exe region|fullscreen|monitor|window|last|scroll|ruler|ocr|color
                    record|gif|recordwindow|stop|pause|history|palette|folder|quit
                    [--pin|--edit|--upload|--redact|--ocr] [--delay N]
AtherScreenshot.exe edit|pin|upload <file>
```

Any `[Hotkeys]` key name works as a command too (e.g. `CaptureRegionPin`). If the app is already running, the command goes to that instance, so you can drive it from AutoHotkey, Stream Deck, scripts, or "Open with".

## Layout

- `src/main.cpp`: commands, hotkeys, tray, capture pipeline, CLI.
- Capture and recording: `overlay` (region, color picker, ruler), `recorder` (MP4/GIF sessions), `wgc` (GPU capture), `audio` (WASAPI), `inputviz` (click and key overlay), `scroll` (scrolling capture), `capture`.
- UI: `palette` (Cmd+K and text prompt), `editor` (GDI+ annotation), `history`, `toast`, `pin`.
- Output and services: `output` (clipboard, PNG, naming), `ocr` (text, words, redaction), `upload` (Imgur, custom, S3), `settings`.
