# Handoff: bring the Windows app to parity with macOS 0.0.1

You're picking up the **Windows** build of Ather Screenshot (C++20 + Win32/GDI+/WIC/Media Foundation/WinRT, `src/`, built with `build.bat`). A native macOS app was developed in `macos/` and is now ahead. Your job is to give the Windows app the same features and behavior, written the Windows way. Don't port the Swift line by line.

- **Reference implementation:** `macos/Sources/AtherScreenshot/*.swift`. Behaviors, constants and algorithms below are taken from it. When this document and the Swift code disagree, the Swift code is right.
- **Version:** both platforms are at **0.0.1** (`src/version.h` is the single source of truth). Keep it at 0.0.1 until the owner says otherwise.
- **Product philosophy (important):** this is deliberately *not* a CapCut or Eagle clone. Hide controls until they're needed, give one good default instead of a set of options, and keep advanced options one level down. When unsure, pick the quieter option.

## Ground rules

1. **Keep the Windows conventions:** single exe, no new third-party dependencies, settings in `%APPDATA%\AtherScreenshot\settings.ini` with the same key names, and the existing file layout.
2. **New data goes in `%APPDATA%\AtherScreenshot\`:**
   - `library.json`: gallery metadata, same schema as macOS (see "Data contracts").
   - `features.bin`: image feature vectors.
   - Existing `ocr-index.txt` entries should be imported into `library.json` once.
3. **Never write metadata into image files.** Captures stay plain PNG/GIF/MP4.
4. **Fail closed on privacy.** If auto-redact can't run (OCR error), don't copy, save or upload the unredacted image: open it in the editor instead and say why.
5. **Work in small, tested steps.** Each milestone below ends with tests plus a build that runs. Keep `build.bat` warning-free.

## Milestones (in this order)

| # | Milestone | macOS reference |
|---|---|---|
| 1 | Hardening fixes (see "Hardening" below) | `App.swift`, `Recorder.swift`, `Library.swift`, `Output.swift` |
| 2 | Gallery: library store, indexer, folder watching | `Library.swift` |
| 3 | Gallery UI: canvas-first layout, filters, inspector, preview | `Gallery.swift` |
| 4 | Suggested tags and search by related words and dates | `Smart.swift` |
| 5 | Version stacks and before/after compare | `Gallery.swift` (stacks, `CompareOverlay`) |
| 6 | Editor: image layers, canvas space, text wrap | `Annotations.swift`, `Editor.swift` |
| 7 | Collage | `Collage.swift`, `Editor.openCollage` |
| 8 | Video editor: trim, crop, speed, mute, captions, auto captions | `VideoEditor.swift` |
| 9 | Video markup, animations and caption styling | `VideoRender.swift`, `VideoEditor.swift` |
| 10 | Package 0.0.1 (`package.bat`), update README and INSTALL | — |

---

## 1. Hardening (do these first; they were real bugs on macOS)

- **URL scheme and command line:**
  - Commands arriving from another process with no console attached (Shortcuts-style tools, browser links via a custom protocol) must show a confirmation first.
  - They may never upload, edit, pin or open files, and they must not inherit `AfterCapture=upload`.
  - Commands typed in a console keep working as today.
  - Store a random per-install token (`cli-token`, user-only ACL) for trusted forwarding between instances.
- **Library safety:** never save the library before it has loaded. If `library.json` can't be parsed, move it aside to `library.unreadable-<epoch>.json` and start empty; never overwrite it.
- **Crop fallback:** a crop that ends up empty (outside the image) must keep the annotations. Never export the unannotated original.
- **Recorder:**
  - Stopping during the countdown cancels cleanly, with no orphan capture session.
  - A capture error mid-recording finalizes and keeps the footage.
  - Pause/resume keeps timestamps increasing per track, and drops samples from the paused period.
  - Quitting while recording waits for the file to finish (with a 20 s fallback).
  - Quitting while PNGs are still being written waits for the pending saves (5 s), then flushes the library.
- **Stale OCR and index data:** when a file's modification time changes, clear its OCR text and re-index it.
- **Rename and delete during a scan:** bump a "generation" counter on every rename or delete. A folder scan that started earlier is discarded and redone, so it can't undo the rename.
- **Folder scoping:** prune metadata only for paths inside the captures folder, compared component by component. `C:\Caps2` is not inside `C:\Caps`.
- **Text editing:** clicking anywhere in the editor commits the text being typed.
- **Save As** clears the "unsaved changes" flag only if the write succeeded.
- **Hotkeys:** require Ctrl, Alt or Win unless the key is an F-key.
- **Dates:** use invariant-culture, Gregorian date formatting for file names and S3 signing.

## 2. Gallery: data contracts

`library.json` (UTF-8). Unknown keys must be ignored and missing keys defaulted, so both versions can read each other's files:

```json
{
  "version": 1,
  "items": {
    "<absolute path>": {
      "mtime": 1759561200.0, "size": 751234, "w": 1800, "h": 1000, "duration": null,
      "tags": ["bug"], "rating": 0, "comment": "", "collections": ["<uuid>"],
      "app": "Safari", "window": "Checkout", "text": "OCR text or null",
      "colors": [{"r": 250, "g": 10, "b": 10, "ratio": 0.42}],
      "dhash": 1234567890123, "indexed": 1,
      "editedFrom": "<path or null>", "includes": ["<path>"], "dismissed": ["chat"]
    }
  },
  "collections": [{"id": "<uuid>", "name": "Sprint 42", "autoTags": ["sprint"]}],
  "smartFolders": [{"id": "<uuid>", "name": "Red things", "filter": { /* Filter */ }}]
}
```

`Filter` fields: `text`, `types` (image/gif/video), `tags`, `anyTag`, `untagged`, `minRating`, `color` (hex), `shape` (landscape/portrait/square/wide/tall), `date` (today/week/month/year), `size` (small/medium/large/huge), `apps`, `minWidth`, `minHeight`.

**Store behavior:**
- **Saving** is debounced (0.8 s) and written on a background thread; flush on quit.
- **New files** (from captures, edits or recordings) get pending metadata registered *before* the file is written, keyed by path: source app and window, inherited tags and so on. The scan applies it when the file shows up.
- **Folder watching:** watch the captures folder recursively with `ReadDirectoryChangesW` (or `FindFirstChangeNotification`) and debounce changes by 0.3 s into a rescan. Everything written there must appear in an open gallery within about a second.
- **Indexer:** background, one file at a time, with a progress count. For each file:
  - **Size** in pixels; duration for video.
  - **Palette:** k-means with k = 6 on a downscaled copy, stored with each color's share of the image.
  - **dHash:** 64-bit difference hash of a 9×8 grayscale thumbnail.
  - **Visual feature vector:** see the Windows technology table.
  - **OCR text.**
  - **Discard the result** if the file's modification time changed while it was being indexed.
- **Duplicates:** group captures whose dHash Hamming distance is ≤ 10 *and* whose feature distance is ≤ 0.3. Without feature vectors, require dHash ≤ 3. Within a group, the newest is first and the action is "keep newest, trash the rest".
- **Find similar:** rank every capture by feature distance and show a percentage, `similarity = 100 × (1 − d / 1.25)` clamped to 0–100.
- **Imports:** dropping files into the gallery copies them to `<captures>\Imported\`.

## 3. Gallery UI (replaces History, `Ctrl+Alt+H`)

The default is a canvas-first layout: the window is just captures in an edge-to-edge grid under one floating toolbar.

- **Floating toolbar** (centered, rounded, translucent):
  - Sidebar toggle, then a scope menu ("All captures ▾": library views, types, collections, smart folders, "New collection…").
  - Search field with the placeholder "Search N captures".
  - A filter button that shows a small dot while filters are on.
  - A "⋯" view menu: Rows / Grid / List, Sort, larger and smaller thumbnails, Show names, Stack edited versions, Sidebar, Inspector, Keyboard shortcuts.
- **Active filter chips** appear under the toolbar only while a filter is on. Each has × to remove it, plus "Save as smart folder".
- **Filter popover:** chips for Type, Tags (any, all, untagged), Rating, Shape, Dimensions, Date, File size and App, plus a row of color swatches and a custom color.
- **Floating action bar** at the bottom while something is selected:
  - Deselect, "N selected", Copy, Tags (T), Collection (F), Rating, Compare (exactly 2 selected), Collage (2 or more), Pin, Upload, Trash, Details.
  - **First-run hint:** until the inspector has been opened once, the Details button reads **"Details ⌘I"**. On Windows use **Ctrl+I**. After that it's just an ⓘ icon.
- **Sidebar** (hidden by default, `Ctrl+Shift+S`; macOS uses ⌃⌘S):
  - Library views (All, Last 7 days, Rated, Uncategorized, Duplicates).
  - Collections, with drop targets.
  - Smart folders.
  - A Tags section, collapsed by default. Right-click a tag to rename or delete it everywhere.
  - Empty sections are hidden. A single "+" at the bottom creates a collection or smart folder.
- **Inspector** (hidden by default, Ctrl+I). It shows only what the capture has:
  - Name (double-click to rename), and one line with size, file size and date.
  - Stars.
  - Tag and collection chips; double-click a chip to filter by it.
  - Suggested-tag chips, dashed: click adds the tag, right-click "Don't suggest".
  - When something is empty, a "+ Tag", "+ Collection" or "+ Comment" button instead of an empty field.
  - Palette as a thin bar: click filters by that color, right-click copies the hex.
  - App, Window, "Edited from" (a link) and "Includes" (links).
  - A Versions list.
  - "Text in image", folded to one line with a word count and a copy button.
  - Annotate / Edit video, and Find similar.
- **Tiles:**
  - No borders or card backgrounds.
  - Badges: GIF or duration, stars, similarity %, and a version-stack count (top right; click it to expand).
  - The file name appears on hover.
  - The selection outline is the only use of the accent color.
- **Layouts:**
  - **Rows:** justified rows (aspect-preserving, Flickr/Eagle-style).
  - **Grid:** aspect-fit cells.
  - **List:** a table with Name, Type, Dimensions, Size, Date, Rating and Tags.
- **Preview** (Space): full window, with zoom, GIF and video playback, ←/→ to move through captures, and a slideshow.
- **Shortcuts sheet** on Ctrl+/ or ?. It replaces the old always-visible shortcut footer.
- **Keys:**

  | Key | Action |
  |---|---|
  | Space | Preview |
  | ↩ | Open |
  | T | Tags |
  | F | Collection |
  | 1–5, 0 | Rate, clear rating |
  | C | Compare |
  | / or Ctrl+F | Search |
  | Ctrl+A | Select all |
  | Ctrl+C | Copy |
  | Ctrl+T | Copy text (OCR) |
  | Ctrl+P | Pin |
  | Ctrl+R | Rename, or batch rename (`{n} {name} {date} {app}`) |
  | Ctrl+U | Upload |
  | Ctrl+O | Show in Explorer |
  | Del | Move to the Recycle Bin |
  | Ctrl+Shift+S | Save smart folder |
  | Ctrl+G | Collage |
  | Ctrl+I | Inspector |
  | Ctrl+= / Ctrl+- | Thumbnail size |
  | Ctrl+K | All actions |
  | Esc | Close similar → deselect → clear filters → close window |

- **Focus:** shortcuts must work right after the window opens and after clicking a tile. Take keyboard focus away from the search box in both cases; on macOS it kept focus and silently swallowed shortcuts.
- **Collections:** a capture can be in several. Each collection has optional "auto tags" that are added to anything dropped into it.

## 4. Suggested tags and search by related words and dates

Port `macos/Sources/AtherScreenshot/Smart.swift` faithfully. It's plain logic.

- **Suggested tags** are computed, not stored, and cached per path keyed by (mtime, OCR length, app, dimensions). They come from:
  - **The source app:** code, terminal, chat, email, design, web, document, spreadsheet, calendar, settings, mobile.
  - **Words in the OCR text, matched as whole words:** error, receipt, login, dashboard, settings.
  - **Code and terminal signals:** at least 3 hits each.
  - **Email:** "from:" and "subject:" both present. **Web:** a URL is present.
  - **Mobile:** height/width ≥ 1.8 and width ≤ 1400.
  - **Dark:** the dominant palette color covers more than 35% of the image and has luminance below 0.18.
  - Copy the app lists and word lists exactly.
- **Where suggestions go:**
  - They're searchable immediately.
  - The inspector shows them as dashed chips.
  - The setting **"Tag captures automatically"** (`AutoTag=0` by default, in the new **Gallery** section of Settings) applies them as real tags. Turning it on tags the whole library once.
- **Search:**
  - Drop stop words.
  - Parse date phrases: today, yesterday, this/last week, past week, this/last month, this/last year.
  - Each remaining word matches exactly (substring), or through its related words. Related words are whole-word matches only, so "ui" doesn't match "build".
  - Related words come from the curated synonym groups in `Smart.swift`, plus a simple stem.
  - Rank exact matches first, then related ones. Show the note "Plus N related by meaning, after the exact matches" (or "No exact matches; showing N related by meaning").
- **Searchable text** for each capture: the file name, date, type words, OCR text, comment, tags, suggested tags, app and window.
- **On macOS** the related words also include on-device word-model neighbors (`NLEmbedding`, cosine distance < 0.95). Windows has no equivalent built in, so ship the curated groups only. Don't add a model dependency.

## 5. Version stacks and compare

- **Stacking:** each edit stores `editedFrom`. Follow the chain to its root, so each original plus all its edits form one stack. While browsing (no search text), show only the newest version, with a stack badge giving the count. Clicking the badge expands the stack in place.
- **Inspector Versions list:** newest first, with markers for this version / the original / edits. Click one to jump to it. Jumping to a hidden version expands its stack first.
- **Compare (C):**
  - Opens with two selected captures, or with one edit (compared to its original, otherwise its previous version).
  - **Slider mode:** both images fitted into one frame, with a draggable divider and handle. **Side-by-side mode** is the alternative. Swap and Esc.

## 6. Editor additions (`Annotations.swift`, `Editor.swift`)

- **Document frame:** `crop` is now the document frame in image coordinates. Inside the image it crops; beyond the image it adds space. It has a `fill` (edge color, white, black or transparent).
  - **Edge color:** the average color of the image's border at 32×32.
  - **Draw order:** fill → base image → image layers → blur, pixelate and magnify → spotlight → vector annotations.
  - **Raster:** cache the result as a raster covering the frame, and redraw it when layers or pixel tools change.
- **Canvas tool (K):**
  - Drag any edge (Alt moves both sides). Keep the view still while dragging by freezing zoom and origin.
  - Floating bar: **Space below** (+max(160 px × scale, 35% of height)), **Space right** (+max(240 px × scale, 40% of width)), **Even margin** (max(32 × scale, 6% of the short side)), Fill, Reset.
  - The crop tool still only trims.
- **Text wrap:** new text gets `wrap = frame.maxX − x − 12 × scale` (at least 80 × scale), so notes wrap at the canvas edge. While typing, draw the wrap edge faintly.
- **Image layers ("Insert image", I):**
  - **Adding one:** drop a file or a gallery tile onto the editor, paste an image or file with Ctrl+V, or press I for a searchable list of recent captures plus "Choose a file…". The gallery also has "Add to open editor".
  - **Default size:** at most 60% of the canvas, centered on the drop point, with an 8 px × scale radius and a shadow.
  - **Editing:** drag to move, drag corner handles to resize (proportional unless Shift), Ctrl+] / Ctrl+[ for front and back.
  - **Right-click menu:** rounded corners, shadow, border (uses the current color and size), opacity 100/75/50/25%, actual size, delete.
  - Layers are part of the raster, so blur and pixelate apply to them. **Everything is flattened on save.**
- **Saving an edit:**
  - **Pending metadata:** before writing, register pending metadata that inherits the original's tags, collections, rating, comment, app and window, and sets `editedFrom`.
  - **"edited" tag:** added only if something actually changed (annotations exist, or the size differs).
  - **`includes`:** the layer sources other than the original.

## 7. Collage (`Collage.swift`)

- **Entry:** select 2 or more images in the gallery, then Collage (Ctrl+G). This opens the editor on an empty 1×1 base with one image layer per screenshot, the frame sized by the layout and the fill set to the background.
- **Layouts:**
  - **Auto:** justified rows all the same width. Width = `min(8000, max(1.15 × √(Σ area), widest image))`. Try every row count from 1 to n, split rows at cumulative aspect boundaries, and keep the count whose overall width/height is closest to 4:3 (minimize `|log((W/H)/(4/3))|`).
  - **Grid:** columns = ⌈√n⌉; cells are the median width × median height; each image is aspect-fit and centered in its cell.
  - **Row:** median height. **Column:** median width.
  - **Feature:** the first image large on the left at height `clamp(600…2400, its height)`; the rest stacked on the right with the same total height.
- **Options, in a floating bar:**
  - Gap and margin: None / S / M / L = 0/8/16/32 pt × scale; margin is 2× that.
  - Background: white, light gray, dark, black, transparent.
  - Corners: rounded (12 × scale × clamp(0.35…1, scale factor)) or square.
  - Shadow.
  - Size: fit, 1920 px wide, or square 2048 px (scale everything and center it).
- **Swapping:** dropping one collage tile onto another swaps them in the order and re-runs the layout.
- **Saving:** tag "collage", keep only the tags and collections *all* sources share, and fill `includes`.
- **Tests:** for every layout, check no overlaps, aspect kept within 3%, everything inside the canvas, and the size presets.

## 8. Video editor (`VideoEditor.swift`)

Opening an MP4 (from the gallery, the "Video saved" toast, or open recent) opens an editor window. GIFs still open in the viewer.

- **Model `VideoEdit`:**

  | Field | Meaning |
  |---|---|
  | `trimStart`, `trimEnd` | Source seconds kept |
  | `crop` | Video pixels, top-left origin; optional |
  | `speed` | 0.5, 1, 1.5, 2 or 4 |
  | `muted` | Remove audio |
  | `captions` | List of captions |
  | `marks` | Markup items |
  | `captionLook` | Pill, outline or bar |
  | `captionStyle` | Caption animation |
  | `captionSize` | 0–4; scale = 0.03 / 0.037 / 0.045 / 0.055 / 0.068 × output height |
  | `captionColor` | Default white (6) |
  | `captionEdge` | Default black (7) |
  | `highlightWords` | Default on |

  All item timing is stored in **source** seconds. Output time = (source − trimStart) / speed.
- **Window layout:**
  - **Toolbar:** Crop (C) with a shape menu (Free, 16:9, 4:3, 1:1, 9:16), Speed, Mute, **Add ▾**, Auto captions, **Captions ▾**, Save GIF, Save.
  - The video stage.
  - A per-item settings row (or a hint line when nothing is selected).
  - Play button and time, then the timeline: 16 thumbnails, yellow trim handles (I/O set them at the playhead), playhead, a caption lane, and markup lanes that grow one row per overlapping item, up to 8.
- **Keys:** Space play/pause, ←/→ step a frame (Shift: a second), T caption, A arrow, R box, E emoji, N step, X blur, Z zoom, C crop, Del delete, Ctrl+Z undo, Ctrl+S save MP4, Ctrl+Shift+S save GIF, Esc (exit crop → deselect → close).
- **Overlay order on the stage:** draw guides and handles in a layer *above* the video surface. On macOS they were drawn under the player and were invisible during playback.
- **Saving:** a new file in the captures folder (MP4 H.264 + AAC with pitch-preserving speed, or GIF at 12 fps and ≤ 960 px from the rendered MP4). Register pending metadata from the source (tagged "edited") so it stacks with the original. The original is never modified.
- **Auto captions:**
  - Mix the audio of the trimmed range and transcribe it on device.
  - Group words into captions: a new caption after a pause > 0.7 s, more than 42 characters, or more than 3.5 s.
  - Each caption holds until the next one (at most 0.8 s more).
  - **Keep each word's start and end time** (`words`); word highlighting uses them.

## 9. Video markup, animations and caption styling (`VideoRender.swift`)

**One renderer for preview and export.** A `FrameRenderer(edit, fullSize, preview)` turns a source frame plus a source time into an output frame. The preview runs it on every frame as well, so what you see is what's saved. Don't build a second drawing path for the preview.

- **Render order:**
  1. Blur and pixelate regions.
  2. Markup on the video (it moves with zoom).
  3. Crop or zoom transform into the output.
  4. Captions and title cards in output coordinates (they stay put on screen).
- **Preview differences:**
  - The output stays in place inside the full frame, so crop guides line up.
  - Placeholders show ("Type a caption…", "Text", 🙂).
  - **Zoom is applied only while playing,** so editing stays put.
- **Mark kinds:** text, bubble, emoji, arrow, box, ellipse, step, blur, pixelate, zoom, title.
  - Arrows store tail → head. Other kinds store a rect.
  - Strokes use the screenshot editor's drawing code, colors and sizes, scaled by `max(1, height / 720)`.
  - **Bubble:** a rounded box with a tail; its text shrinks to fit.
  - **Emoji:** sized to its box.
  - **Title card:** covers the frame; background color, centered title at 8.5% of the height, subtitle at 4%.
  - **Blur:** strength levels 3/5/8/12/18% of the region's short side. **Pixelate:** blocks 4/7/10/14/20%.
  - **Zoom:** the target is the box grown to the output's aspect ratio (at least 1/6 of the width) and clamped inside the frame. It eases in and out with smoothstep over 0.45 s, or 0.18 s for "Snappy".
- **Defaults when adding** (geometry relative to the visible frame): see `addMark` in `VideoEditor.swift`. Each new item lasts 3 s from the playhead (title cards 2.5 s). Step numbers count up on their own.
- **Animation:** one choice per item.
  - **Styles:** Auto, None, Fade, Pop, Scale, Slide up, Wipe, Blur in, Draw on, Typewriter. The chosen style plays in and is mirrored out; Draw on and Typewriter leave with Fade.
  - **Auto by kind:** arrow, box and ellipse → Draw on; step, emoji and bubble → Pop; text → Slide up; title → Fade; blur, pixelate and zoom → None.
  - **Each menu lists only the styles that suit the kind** (copy `MarkKind.styles`).
  - **Durations in:** Pop 0.24 s, Draw on min(0.6 s, length/2), Typewriter clamp(0.3 s, chars × 0.035 s, 60% of the length), Wipe 0.4 s, others 0.3 s. **Out:** Pop 0.16 s, others 0.22 s. Both are capped at half the item's length.
  - **Curves:**
    - Pop: scale 0.55 → 1 with ease-out overshoot `1 + 2.2(p−1)³ + 1.2(p−1)²`, alpha min(1, 2.5p).
    - Scale: 0.85 → 1, ease-out cubic.
    - Slide: from 3.5% of the height below.
    - Blur in: from 12 px × scale.
    - Wipe: reveals left to right.
    - Draw on: arrows grow from the tail; box and ellipse strokes trace their outline.
    - Typewriter: reveals characters, while the layout box stays at full-text size so nothing drifts.
  - **While on screen (optional):** Pulse (±4.5%, 1.2 s), Bounce (|sin| at 0.55 s, 1.8% of height), Shake (a 0.45 s burst every 2 s), Ping (a white ring spreading to 1.5× and fading, every 1.4 s).
  - **Advanced, at the bottom of the same menu:** "Different exit animation" (adds an Exit submenu), and "Apply to all {kind}" (confirm with a toast).
  - **Menu feedback:** picking a style must update the menu's label and checkmark **immediately** and replay the item from 0.4 s before it starts. Not doing this was a bug on macOS; there's a test for it.
  - A ▶ button next to the menu replays the item.
- **Captions:**
  - **Looks:** Pill (box radius 0.35 × font size, alpha 0.62), Outline (heavy weight, 1.25× size, stroke −2.5 in the edge color, soft shadow, no box), or Bar (full width, alpha 0.7).
  - **"All captions" settings:** text color, box/outline color, size and animation (Auto = Fade, None, Fade, Pop, Slide up, Typewriter). They're in the Captions ▾ menu and also in the selected caption's row, labeled "All captions:".
  - **Spoken-word highlight:** yellow `#FFD60A`, or white/black if the caption text itself is yellow. Only when the caption text still equals its words joined with spaces.
  - **Position:** Top, Middle, Bottom (6% margin), or **dragged anywhere**. A dragged position is stored as a fraction of the frame, kept on screen, and shows as "Custom" in the menu; choosing a preset clears it.

## Windows technology mapping

| Need | macOS used | Use on Windows |
|---|---|---|
| Folder watching | FSEvents | `ReadDirectoryChangesW` on a thread, recursive, debounced |
| OCR | Vision | `Windows.Media.Ocr` (already in `ocr.cpp`) |
| Image feature vector for similar and duplicates | Vision feature print | **No OS API.** Use dHash plus a 4×4×4 RGB color histogram plus a 16×16 grayscale DCT; cosine distance, with thresholds calibrated so a rescaled copy is close and different screens are far. Note the calibration in code. Don't ship a machine-learning model. |
| Palette, dHash | Core Graphics | WIC scale to 64×64, own k-means and hash (port as is) |
| Related words | NLEmbedding | Curated groups only (see section 4) |
| Thumbnails | QuickLook | `IShellItemImageFactory` / `IThumbnailProvider`; WIC for PNG |
| Gallery UI | SwiftUI | Win32 + Direct2D/DirectWrite custom-drawn views; the translucent look via `DWM_SYSTEMBACKDROP_TYPE` (Mica/Acrylic on Windows 11) |
| Video read and write | AVFoundation | Media Foundation: Source Reader → your renderer → Sink Writer (H.264/AAC); resample audio for speed with pitch kept (`IMFAudioProcessing` / SoundTouch-style WSOLA written by hand if needed) |
| Frame renderer | Core Image | Direct2D effects on DXGI surfaces (Gaussian blur, crop, affine transform, a "pixelate" effect via scale down and up with nearest-neighbor); text with DirectWrite |
| Preview with the same renderer | AVPlayer + video composition | Decode with the MF Source Reader (or `IMFMediaEngine` with frame-server mode), run `FrameRenderer`, present through a D2D swap chain |
| Speech to text | SFSpeechRecognizer (on device) | `Windows.Media.SpeechRecognition` with a file-based audio stream; if it can't do offline file transcription reliably, show "Auto captions need an internet connection" rather than failing silently |
| GIF export | ImageIO | WIC GIF encoder (reuse `recorder.cpp`'s GIF code) |

## Testing

- **Pure logic as unit tests** (a small test exe, or `AtherScreenshot.exe --selftest`):
  - Filter matching; suggested tags; query parsing and dates; collage layouts; duplicate thresholds; caption chunking.
  - Animation curves (`Motion.between`).
  - Edit inheritance and collage metadata; version stacks.
- **Render tests:** feed a synthetic frame (left half red, right half blue, white bar at x 300–340) to `FrameRenderer` and check pixels. Copy the assertions from `macos/Tests/AtherScreenshotTests/MarkupTests.swift` and `VideoTests.swift`, including that captions appear and disappear on time and that typewriter and draw on render partially.
- **Export test:** generate a 3 s clip, export with trim 1–3 s, 2× speed, a crop and a caption, then check the duration (~1 s), the even crop size, the first frame's color and the caption pixels.
- **Isolation:** tests must never touch the real `%APPDATA%` library. Honor an `ATHER_SUPPORT_DIR` override for tests.

## Definition of done

- Every milestone works from the keyboard and mouse, with no new dependencies, and `build.bat` and `package.bat` succeed.
- **Data:** the Windows gallery reads a `library.json` written by the Mac app, and the other way round. Paths differ, so the entries simply don't match on the other machine, but neither side crashes or loses data.
- **Docs:** README (Windows) documents the new features in the same style as `macos/README.md`. INSTALL.md is updated.
- **Release:** `package.bat` produces `AtherScreenshot-Setup-0.0.1.exe` and the portable zip. Don't publish a GitHub release unless the owner asks. (The old `v1.0.0` release and tag were deleted when the version was reset.)

## Don'ts

- No custom keyframes or easing curves, motion paths, 3D effects, multiple video tracks, transitions, music libraries, or filter packs.
- No cloud sync and no AI chat.
- No always-visible panels.
- Don't widen the trust of external commands.
