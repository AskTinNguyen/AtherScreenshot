# Frame Review: Step 60 fps Video Frame by Frame, Leave Notes, Export a Review Video

- Rev: 2
- Status: active
- Area: Video
- Owner: Tin Nguyen
- Skill: `none`
- Branch: `intent/frame-review`
- Started: 2026-10-10

## Goal

The team uses Ather Screenshot's video editor to review recordings made at **60 frames per second**. They want to:
- scrub and step through them frame by frame;
- leave comments and notes on exact frames;
- export a video the other team can watch, with those notes in it.

Make the video editor good at that, without getting in the way of its normal edit-and-save use.

What the editor does today (from the code, 2026-10-10):
- ←/→ steps `frames / 30` seconds (`Step` in `src/videoeditor.cpp`). On a 60 fps recording, one press therefore moves **two** frames, and half the frames can't be reached by stepping.
- The time readout is `m:ss.t` (tenths of a second): no frame number, and no way to tell frames apart.
- Captions and markup exist, but nothing is meant as a reviewer's note: no author, no list of notes, nothing kept with the video (edits are lost when the editor closes), and no export that puts the notes in front of another team.

## Non-Goals

- **The Mac app.** It gets a handoff afterwards, like `docs/HANDOFF-mac-faster-export.md`.
- **Online collaboration:** syncing notes between people live, accounts, or replying in threads.
- **Freehand drawing on frames.** Notes can point at a spot; the existing markup (arrows, boxes) covers more.
- **Changing what Save and Save GIF produce** for a video without notes, or for anyone who doesn't use notes.

## Decisions

These are the orchestrator's proposed defaults for rev 1. They are calls the contract reserves for the owner: look, wording, saved files and export look. The owner may change any of them by saying so; until then, the worker builds them as written.

- **D1 (rev 1): exact frames.** A frame step uses the frame rate of the clip under the playhead (from the file). It lands on that frame's own timestamp and shows that exact decoded frame, not the nearest key frame.
- **D2 (rev 1): keys.**
  - ←/→ step one frame; Shift+←/→ one second.
  - Home/End go to the start/end of the trim.
  - J/K/L: reverse/pause/forward. Pressing L again cycles 0.25× → 0.5× → 1× preview speed.
  - Ctrl+G: go to a frame number or time.
  - M: add a note at the playhead.
  - [ and ]: previous/next note.
  - These were all free in `VideoEditor::Key`.
- **D3 (rev 1): timecode** reads `m:ss:ff` (frames within the second, at the clip's rate), followed by `frame <n>` counted from the start of the timeline. Example: `0:12:37 · frame 757 · 60 fps`.
- **D4 (rev 1): what a note is.**
  - A frame (or optional range), text, an author, and an optional point on the video.
  - The author is Windows' display name by default, changeable once in the editor and remembered in settings.
  - Notes aren't part of the normal Save output.
- **D5 (rev 1): where notes are kept.**
  - A sidecar next to the source video: `<video file name>.notes.json`.
  - Plain JSON, one entry per note, with source-file time and frame number, so it survives the editor being closed and can be read by other tools.
  - The source video is never modified.
  - A note on a joined clip is stored with the file it's on.
- **D6 (rev 1): the review video.** **Save review video** in the Save menu (Ctrl+Alt+S) writes an MP4 of the current edit, plus:
  - a small running burn-in (timecode and frame number) in a corner;
  - at each note, the video **holds the noted frame for 3 s**, showing a note card (author, timecode and frame, text, a pin at the note's point), then plays on;
  - silence during the hold.
  - It is saved next to a `.md` list of the notes, as `<name> review.mp4` and `<name> review notes.md`.
- **D7 (rev 2): no waiting on the owner.** The owner is away and said not to stop for permission (L-2). D1–D6 are accepted as written.
  - The worker makes any further owner-level call itself: wording, look, or a detail the spec leaves open. It picks the option that best fits the app's existing style, and records it in `progress.md` under "Owner-level calls", with the options it considered.
  - Findings are still written for anything that changes the intent, but none is `blocking` unless continuing would be destructive, a release, or a security or credential matter.
  - Merge authority is unchanged (none): the PR is left ready, and the owner merges.
- **D8 (rev 2): tools.** Dev-only tools may be installed if they help verify or build, for example an image-diff helper or a Python package used by a test script. Each one is recorded in `progress.md` with why. Nothing is added to the app's own build or dependencies.
- **D9 (rev 2): timeline zoom.**
  - Ctrl+wheel over the timeline, or Ctrl+= and Ctrl+−, zooms around the playhead, down to about 14 px per frame. Ctrl+0 fits the whole trim.
  - When zoomed in, the timeline draws a tick per frame and labels frame numbers at a readable interval. Dragging the empty timeline, or the wheel, pans.
  - Notes, captions and markup bars stay on their frames at every zoom.
- **D10 (rev 2): pixel magnifier.** The preview can zoom for inspecting detail:
  - the wheel over the video zooms 1× to 8× around the cursor;
  - right-drag pans;
  - F (or a double-click) goes back to fit;
  - from 2× up, pixels are drawn sharp (nearest neighbour), and a small "4×" label shows the zoom.
  - It's for viewing only: it never changes the crop or the export.
- **D11 (rev 2): note kinds and status.**
  - Each note has a kind: **Note** (lime, the default), **Issue** (red), **Question** (blue) or **Looks good** (green). Use the editor's existing palette colors.
  - Each note has a **Resolved** toggle. The notes list can show All or only Open notes.
  - Kind and status are saved in the sidecar, and appear on the note card, in the `.md` list, on the contact sheet and as the tick color on the timeline.
  - The review video includes resolved notes, marked "Resolved", so the other team sees the whole history.
- **D12 (rev 2): review extras.**
  - The review video opens with a 3 s summary card: the video's name, the date, reviewers, counts by kind, and the notes with their timecodes, up to as many as fit.
  - Save review video also writes `<name> review sheet.png`: a contact sheet with one tile per note. Each tile has the noted frame (with its pin), the timecode and frame number, the kind, the author and the text. It is readable when posted in a chat.

## Acceptance

- **A1: exact stepping.** ←/→ moves exactly one frame at the clip's own rate, and the editor shows that exact frame.
  - On a 60 fps clip, stepping forward from the start visits every frame once and in order; stepping back does the same in reverse. This also holds across two joined clips with different rates (60 and 30 fps).
  - Shift steps one second; Home/End go to the trim's ends.
  - Proof: `gate: test.bat`. A new self-test makes a 60 fps clip with a different color on every frame, steps through it with the editor's step logic, and checks every frame index is seen once, in order.
- **A2: frame and time readout.** Shows `m:ss:ff · frame n · fps` (D3) for the frame on screen. Ctrl+G accepts a frame number or a time and lands on that frame.
  - Proof: `gate: test.bat` (formatting and go-to tests at 60, 30 and 29.97 fps). A `snapshot` of the editor shows the readout.
- **A3: review playback.** J/K/L play back, pause and play forward. Preview speeds of 0.25×, 0.5× and 1× don't change the edit's speed or what Save writes.
  - Proof: `gate: test.bat`. A test shows that changing the preview speed leaves `VideoEdit` (and so the export) unchanged, and that 0.25× playback presents successive frames without skipping.
- **A4: notes in the editor.**
  - M adds a note at the playhead's frame, with a text field, focused.
  - A click on the video while a note is selected sets its pin.
  - Notes show as ticks on the timeline and in a notes list (timecode, author, first line). Clicking one goes to its frame.
  - [ and ] go to the previous and next note.
  - Notes can be edited and deleted, and undo covers them.
  - Proof: `gate: test.bat` (add, edit, delete, undo and jump tests), plus a `snapshot` of the editor with notes on the timeline and in the list.
- **A5: notes are kept.**
  - Notes save to the sidecar (D5) as they change.
  - Reopening the video shows them at the same frames, including in joined clips and after trimming.
  - A missing or damaged sidecar never stops the video from opening.
  - Proof: `gate: test.bat` (round trip, joined clips, trimmed range, damaged file).
- **A6: review video.** Save review video writes the MP4 described in D6, plus the notes list.
  - The video's length is the edit's length plus 3 s per note.
  - The noted frame is held under its note card, and the burn-in shows the right frame number on the frames around each note.
  - A plain Save of the same edit stays byte-for-byte identical to before this feature.
  - Proof: `gate: test.bat` (an export of a generated clip with notes checks duration, held frames, card pixels and burn-in via `g_exportTap`, and that plain-Save frame hashes are unchanged). A `snapshot` of a note card frame.
- **A7: notes list.** The `.md` list has one line per note: `m:ss:ff (frame n), Author: text`. A "Copy notes" button puts the same text on the clipboard.
  - Proof: `gate: test.bat`.
- **A9: timeline zoom (D9).**
  - Zooming keeps the playhead's frame under the same spot.
  - At full zoom, each frame has its own tick, and clicking a tick lands on that frame.
  - Ctrl+0 fits the trim.
  - Notes, captions and bars stay aligned with their frames.
  - Proof: `gate: test.bat` (pixel-to-frame and frame-to-pixel round trip at several zooms, and clicking a tick lands on its frame), plus a `snapshot` of the zoomed-in timeline with frame ticks and a note tick.
- **A10: pixel magnifier (D10).**
  - The zoom stays centered on the cursor.
  - At 2× and above, pixels are sharp: a 1-pixel checkerboard in a test clip shows as hard-edged blocks.
  - F resets the zoom.
  - Crop and export are unchanged.
  - Proof: `gate: test.bat` (view math, nearest-neighbour pixels, and `VideoEdit` unchanged), plus a `snapshot` at 4×.
- **A11: note kinds and status (D11).**
  - Kind and resolved status can be set, can be undone, are saved in the sidecar and are filtered in the list.
  - They are colored on the timeline, the card, the `.md` list and the sheet.
  - Proof: `gate: test.bat`, plus a `snapshot` of the notes list showing all four kinds and a resolved note.
- **A12: review extras (D12).**
  - The review video starts with the 3 s summary card, and its length matches.
  - The contact sheet PNG has one tile per note, in timeline order, with the right frame in each tile: its pixels match that frame from the video, scaled.
  - Proof: `gate: test.bat`, plus a `snapshot` of the summary card and the contact sheet.
- **A13: visual proof of the whole flow.**
  - The worker renders the new UI the way people see it: the editor with a 60 fps clip, the readout, the zoomed timeline with notes of several kinds, the notes list, the magnifier at 4×, a note card frame from the review video, and the contact sheet.
  - It opens each image itself, checks it (text readable, nothing clipped, colors right), fixes what's wrong, and lists every image path in `progress.md`.
  - The orchestrator then reviews the same images.
  - Proof: `review`, recorded as a step with the image paths and the orchestrator's verdict in `log.md`.
- **A8: docs.**
  - README's Video editor guide covers stepping, the readout, review playback, notes and the review video, and the shortcut list is updated.
  - `RELEASE_NOTES.md` gets a line under the next update.
  - Proof: `review`.

## Constraints

- **The contract's Never list:**
  - no owner recordings in git; tests generate their own clips;
  - never touch the real support folder;
  - no releases or version changes;
  - no third-party dependencies.
- **The fast export must stay fast and unchanged** for plain saves. Review-video rendering goes through the same `FrameRenderer` and export path, without slowing exports that have no notes.
  - Check with `--bench-export`, best of 3, old and new builds run alternately, before the PR.
- **Decode exactness:**
  - Seeking for a step must land on the exact frame. GPU and software decoding both report frame timestamps; use them, not `t * fps` guesses.
  - Mind the GPU-reader rule in `VideoReader::Seek` (decode a frame before seeking a fresh reader).
- **Windows only.** Use Media Foundation and Win32, as the rest of the editor does.

## Changelog

- rev 1 (2026-10-10): created from L-1, with the orchestrator's proposed feature set (D1–D6, A1–A8) for the owner to review.
- rev 2 (2026-10-10): from L-2 (owner away 8 h, full autonomy):
  - D1–D6 accepted.
  - D7: owner-level calls are made and recorded, not waited on.
  - D8: dev tools allowed, if recorded.
  - D9–D12 and A9–A13: timeline zoom, pixel magnifier, note kinds and status, a summary card and contact sheet, and visual proof of the whole flow.
  - Merge stays with the owner.
