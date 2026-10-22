# Video Copy: Ctrl+C Copies the Marked Part as a Clip, or the Frame on Screen

- Rev: 1
- Status: active
- Area: Video
- Owner: AskTinNguyen
- Issue: #6, #7
- Skill: `none`
- Branch: `intent/video-copy`
- Started: 2026-10-21

## Goal

From the team's playtest wishlist (L-1). Playtests are recorded full screen, and the team posts moments from them to Slack. Today a clip means: split it in the video editor, Save, find the file, drag it into Slack. Two wishes:

1. **Clip (#6):** pick the part of the video, press Ctrl+C, and paste it straight into Slack.
2. **Frame (#7):** stop on any frame of the timeline, press Ctrl+C, and the picture of that frame is copied.

Both live in the video editor on Windows (the team's platform), then the Mac app gets the same with ⌘.

What the editor does today (from the code, 2026-10-21):
- Ctrl+C does nothing in the Windows video editor (`VideoEditor::Key` in `src/videoeditor.cpp`: the Ctrl switch has S, O, Z, G, +, −, 0 and W).
- On the Mac, `VideoEditor.key(_:)` in `macos/Sources/AtherScreenshot/VideoEditor.swift` doesn't handle ⌘C either.
- There is no range on the timeline:
  - I and O set the trim.
  - S splits the clip at the playhead.
  - A selected clip in the clip lane is the only "part" that can be picked.
- Helpers that exist:
  - `CopyFileToClipboard` (CF_HDROP) and `CopyImageToClipboard` (CF_DIB + PNG) in `src/output.h`.
  - `ExportMp4` (fast, H.264 + AAC).
  - `FrameRenderer` (the same frame drawing as the preview and the export).

## Non-Goals

- **Uploading to Slack** or any chat service by API. The clipboard is the hand-off; people paste.
- **GIF clips.** Save GIF stays as it is.
- **Changing Save, Save GIF, Save smaller or the review video.** Their output must not change.
- **Copying from the gallery** (it already copies files and pictures).

## Decisions

These are the orchestrator's proposed defaults (rev 1). Looks, wording and keys are owner calls by the contract. The owner asked to build everything (L-1), so the worker builds them as written; the owner may change any of them by saying so.

- **D1 (rev 1): Ctrl+C copies what's picked, in this order.**
  1. A marked range on the timeline → the clip (D3).
  2. With no range: the clip selected in the clip lane, if there is one and no markup item is selected → that clip as edited (from its in to its out, inside the trim).
  3. Otherwise → the frame on screen (D5).
  - **Ctrl+Shift+C** always copies the frame, also while a range is marked.
  - While a text field (a caption, a note, Go to) has focus, Ctrl+C is the field's own text copy.
- **D2 (rev 1): marking a range.**
  - **Shift+drag** on the timeline or filmstrip marks a range. **Shift+I** / **Shift+O** set its start or end at the playhead (I and O alone still set the trim).
  - It snaps to frame boundaries and stays inside the trim.
  - It's drawn as a translucent accent band over the timeline, with grab handles on its edges and a small label: `6.2 s · Ctrl+C copies`.
  - Dragging an edge resizes it. Esc clears it (before Esc's other meanings), and so does a plain click on the timeline outside it.
  - The range is a selection, not an edit: it isn't undoable, isn't saved and doesn't change Save.
  - It follows timeline zoom and pan like notes do.
- **D3 (rev 1): the copied clip.**
  - It's the edit over the range, exactly as Save would write that part: crop, speed, mute, captions and markup. Review notes are not burnt in.
  - MP4, H.264 + AAC, via the existing fast export (`ExportMp4` on Windows). On the Mac use H.264 + AAC too (not the HEVC Save uses), so it plays inline in Slack and on Windows PCs without the HEVC extension.
  - Saved in the captures folder under the same name scheme as Save (`MakeCapturePath`), noted in the Library as an edit of the source (it stacks with the original in the gallery).
  - Then the **file** is put on the clipboard (Windows: CF_HDROP; Mac: the file URL on `NSPasteboard.general`), so Ctrl+V in Slack, Teams, Discord or Explorer attaches it.
  - While it saves, a toast shows progress. When done: "Clip copied · 6.2 s · 3.4 MB" with "Paste it in Slack or anywhere"; a click shows the file in Explorer/Finder.
  - A second Ctrl+C while one is saving is ignored, like Save.
  - If the clipboard can't be opened, the file is still saved and the toast says so.
- **D4 (rev 1): the Save menu lists both.** Save ▾ gets **Copy clip** (Ctrl+C, greyed without a range or selected clip) and **Copy frame** (Ctrl+Shift+C), so the keys can be found. On the Mac, the matching menu or buttons next to Save.
- **D5 (rev 1): the copied frame.**
  - It's the frame on screen as the export draws it (`FrameRenderer`: crop, captions and markup at that time), at the export's full size, not the preview's size or magnifier.
  - Windows: CF_DIB + PNG (`CopyImageToClipboard`). Mac: PNG + TIFF.
  - Not saved to a file.
  - Toast: "Frame copied" with the readout (`0:12:37 · frame 757` on Windows) and a thumbnail.
- **D6 (rev 1): Windows first, then the Mac.**
  - Windows gates run on GitHub's Windows runner (`.github/workflows/check.yml`, on every push to `intent/**`). The run's URL and the self-test result line from its log are the evidence for `gate: test.bat` and `build`.
  - MinGW (`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -DUNICODE -D_UNICODE -Dsmall=char`) is a quick local type-check, never a proof.
  - The Mac part is proven with `gate: mac` on this Mac.
- **D7 (rev 1): no waiting.** Further owner-level calls (wording, look, details the spec leaves open) are the worker's. It picks what fits the app's existing style and records each, with the options it considered, in `progress.md` under "Owner-level calls". Findings are still written for anything that changes the intent.

## Acceptance

- **A1: range marking (Windows).**
  - Shift+drag and Shift+I/O mark a range.
  - It snaps to frames, stays in the trim, and its edges drag.
  - Esc and a click outside clear it.
  - It doesn't change `VideoEdit` and isn't in undo.
  - Proof: `gate: test.bat`, with new self-tests that drive the editor's key and mouse handlers on a generated clip: pixel↔time at two zooms, snapping, Shift+I/O, edge drag, clear. `VideoEdit` stays equal before and after.
- **A2: Copy clip (Windows).**
  - Ctrl+C with a range writes an MP4 into the captures folder whose duration is the range's (within one frame) and whose frames are the edit's frames at those times.
  - The clipboard then holds CF_HDROP with exactly that path.
  - With no range and a clip selected in the clip lane, that clip is copied the same way.
  - Proof: `gate: test.bat`, with a self-test that marks a range on a clip whose frames each have their own color and checks the output's frame count, its first and last frames' colors, and the CF_HDROP path. A second test covers the selected clip.
- **A3: Copy frame (Windows).**
  - Ctrl+C with no range (and no selected clip), and Ctrl+Shift+C at any time, put the frame on screen on the clipboard as CF_DIB and PNG.
  - Its pixels equal `FrameRenderer`'s output for that frame (with a caption and a box on it in the test), at the export's size.
  - A focused text field keeps its own Ctrl+C.
  - Proof: `gate: test.bat`.
- **A4: nothing else changes (Windows).**
  - Every existing self-test still passes.
  - A plain Save of an edit writes the same frames as before. The export isn't touched; the existing Save and review-video tests stand as proof.
  - Proof: `gate: test.bat` (the full run, 0 failed).
- **A5: visual proof (Windows).** The video editor with a marked range (band, handles, label) and the Save menu with Copy clip and Copy frame.
  - Proof: `snapshot`, rendered by `--video-snapshots` on the runner, opened by the worker, and named in `progress.md`.
- **A6: Mac.**
  - ⇧-drag on the timeline marks a range.
  - ⌘C copies the clip (H.264 + AAC file URL on the pasteboard) or, without a range or selected clip, the frame (PNG on the pasteboard, equal to the renderer's frame). ⌘⇧C always copies the frame.
  - Proof: `gate: mac`, with new XCTests: range → clip duration and codec, the pasteboard's file URL, frame pixels.
- **A7: Mac visual proof.** The Mac video editor with a marked range.
  - Proof: `snapshot` (`ATHER_TEST_OUT` rendering, opened by the worker).
- **A8: docs.**
  - README's Video editor guide and its shortcut line cover the range, Copy clip and Copy frame, and so does `macos/README.md`.
  - `RELEASE_NOTES.md` gets a line under the next update.
  - Proof: `review`.
- **A9: pasting into Slack.** On a real PC and Mac, a copied clip and a copied frame paste into Slack and play or show there.
  - Proof: `owner`.

## Constraints

- **The contract's Never list:**
  - no owner recordings in git; tests make their own clips;
  - never touch the real support folder;
  - no releases, tags, `latest.json` or version changes;
  - no third-party dependencies;
  - don't stop the owner's running app.
- **The export path must not slow down or change** for Save, Save GIF, Save smaller and the review video. Copy clip calls the export; it doesn't modify it.
- The clipboard is shared with the rest of the system. Copy only on the user's key or menu choice, never on its own.
- **Shared files:** `README.md`, `RELEASE_NOTES.md` and `macos/README.md` are also edited by the `bug-report` and `capture-tray` intents. Keep edits to this feature's own sections and lines, so merges stay clean.
- `src/main.cpp`, `src/settings*` and `macos/.../App.swift` belong to the other intents' changes. Touch them only if you must, in small hunks.

## Changelog

- rev 1 (2026-10-21): created from L-1 (issues #6 and #7), with the orchestrator's proposed defaults D1–D7.
