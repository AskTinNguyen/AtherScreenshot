# Frame Review: Progress

- Working under rev: 2
- Worker: `frame-review-worker`
- Current step: S6 (A5), notes kept in `<video>.notes.json` next to each video.
- Next step: A4 (notes in the editor, with D11 kinds and status built in); then A11, A5, A9, A10, A6, A12, A7, A13, A8 (rev 2 order).
- PR: https://github.com/AskTinNguyen/AtherScreenshot/pull/2

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S2: `test.bat` → `86 tests, 1055 checks, 0 failed` with `video_editor_steps_every_frame_at_60fps` (fails before the fix, S1) and `video_frame_times_match_the_decoded_frames` |
| A2 | met | S3: `test.bat` → `88 tests, 1117 checks, 0 failed` with `video_frame_readout_and_go_to` (60, 30, 29.97 fps, joined) and `video_editor_readout_and_go_to`; snapshot `docs/intent/frame-review/proof/a2-readout.png` (opened: "0:12:37 · frame 757 · 60 fps" readable under the video) |
| A3 | open | |
| A4 | in progress | S5: `video_editor_notes_add_edit_pin_jump_undo` → `1 tests, 35 checks, 0 failed`; snapshot `docs/intent/frame-review/proof/a4-a11-notes.png` (opened, checked); full gate pending |
| A5 | open | |
| A6 | open | |
| A7 | open | |
| A9 | open | |
| A10 | open | |
| A11 | in progress | S5: kinds, Resolved, Open filter, colored ticks and list (same test and snapshot); sidecar, card, `.md` and sheet still to come |
| A12 | open | |
| A13 | open | |
| A8 | open | |

## Steps

<!-- S<n> (rev <r>, <YYYY-MM-DD>): <what was done>. Evidence: <commit / command and result line / PR>. Acceptance: <A-ids moved>. -->

- S0 (rev 1, 2026-10-10): baseline in this worktree before any change. Evidence: `test.bat` → `84 tests, 1021 checks, 0 failed`, exit 0 (66 s with the build).
- S1 (rev 1, 2026-10-10): test first for A1. New self-test `video_editor_steps_every_frame_at_60fps` writes a 2 s 60 fps clip whose every frame has its own color (and a 2 s 30 fps one with blue), presses ←/→ (and Shift, Home, End) through the editor's key handler (`Key`, split out of `OnKey` so tests can pass modifiers), and reads each decoded frame's color back. Run before the fix, it fails the way the intent says: `test.bat --nobuild video_editor_steps` → `1 tests, 14 checks, 1 failed`, with "60 fps forward: a2 a4 a6 a8 …" (every other frame). Evidence: commit 0e1777c. Acceptance: A1 (test in place, failing).
- S2 (rev 1, 2026-10-10): exact stepping. `FrameTimes(path)` (videoio) reads every frame's time from the file without decoding (Source Reader with no output type, sample times sorted); `TimelineFrames` (videoedit) lists every frame of the timeline with its time, clip and source time, following SequenceReader's rules at clip edges, with a grid at the clip's rate while a file's times are still being read. The editor reads each file's times on a background thread; ←/→ move to the next/previous frame in that list and the paused preview asks the fetcher for exactly that frame (by its own time, 0.5 ms tolerance). Shift+←/→ go to the frame nearest a second away, Home/End to the first/last frame of the trim. The fetcher now keeps the frames it decoded on the way to a step (up to ~150 MB), so stepping back doesn't decode from the key frame each time (the step test went from 49 s to 16 s). Evidence: `test.bat --nobuild video_editor_steps` → `1 tests, 14 checks, 0 failed`; new `video_frame_times_match_the_decoded_frames` passes; full `test.bat` → `86 tests, 1055 checks, 0 failed`, exit 0. Acceptance: A1 met.

Engineering decisions (S2):
- Frame times come from the file (demuxed sample times), not `t × fps`; decoding gives frames the same times (tested). A grid at the clip's rate is used only until a file's times have been read, or when they can't be.
- The paused preview snaps its fetch to the nearest frame, so the frame shown and the frame number always agree; the playhead itself is not moved by scrubbing.
- Our own encoder evens out irregular frame times, so there is no variable-rate test clip; the test checks file times against decoded times instead.
- S3 (rev 2, 2026-10-10): the frame readout and Ctrl+G. A readout row under the video shows `m:ss:ff · frame n · fps` of the frame on screen (`TimelineFrames::Readout`; the timeline's time also reads `m:ss:ff`), with a key hint on the right; clicking it or Ctrl+G turns the inspector field into "Go to", which takes a frame number (757), a timecode (0:12:37, or h:mm:ss:ff) or seconds (12.6, 0:12.6); Enter lands on that frame (a toast says when the text isn't one), Esc or clicking away cancels, nothing goes on the undo stack. Evidence: `video_frame_readout_and_go_to` (formatting, round trip of every frame's timecode and number at 60, 30 and 29.97 fps, no repeated timecodes, joined clips) and `video_editor_readout_and_go_to` pass; full `test.bat` → `88 tests, 1117 checks, 0 failed`, exit 0; `--video-snapshots` renders `video-editor-readout.png` (copied to `docs/intent/frame-review/proof/a2-readout.png`), opened and checked. Acceptance: A2 met.
- S4 (rev 2, 2026-10-10): J/K/L review playback. L plays forward, L again cycles the preview speed 0.25× → 0.5× → 1×, J plays backward (J again cycles too), K (or Space) pauses. Forward at 1× is the usual media-engine playback with sound; slower, or backward, the paused preview steps through every frame in turn, each decoded exactly, at that pace (never skipping: it waits for each frame), silent. A pill in the readout row shows "▶ Preview 0.25×" / "◀ Reverse 0.5×" while it plays that way. The speed is the preview's only (never written into `VideoEdit`). Evidence: `video_editor_review_playback_jkl` (cycle order, every frame in order at 0.25× and 0.5×, backward, stops at the trim start, `VideoEdit` unchanged, nothing on the undo stack) → `1 tests, 19 checks, 0 failed`, three reruns the same. Acceptance: A3 (full gate pending, with S5).
- S5 (rev 2, 2026-10-10): review notes in the editor, with D11's kinds and status built in. Model: `Note` in `VideoEdit::notes` (file path, the frame's own source time, optional range end, text, author, kind, resolved, optional pin as fractions of the file's upright frame, created time); tied to footage, so clip edits never move or drop them (a note whose footage isn't on the timeline is just not shown), and undo covers them because they live in the edit. `FrameOfSource` places each on the timeline. Editor: M (or Add ▾ › Review note) adds one on the frame on screen with its text field focused; the inspector has the kind (Note/Issue/Question/Looks good, palette lime/red/blue/green), text, author, Resolve, Clear pin or "Click the video to pin a spot", Range to here / No range, Delete; a click on the video with a note selected pins that spot; the timeline shows each note as a flag and line in its kind's color (faint when resolved, a band for a range); a notes list right of the video (shown once there are notes) has timecode, author, kind and first line per note, All/Open filter, Copy (A7) and wheel scrolling; clicking a row or a flag goes to that frame; [ and ] go to the previous/next listed note. The author is Windows' display name (`GetUserNameExW`) until one is typed in a note's author field, which is then remembered in settings (`[VideoEditor] NoteAuthor`, through `SetVideoEditorAuthor` from main.cpp). Evidence: `video_editor_notes_add_edit_pin_jump_undo` (add with field ready, typing, pin by click, kind/resolved with undo, order and [ ] jumps, Open filter, row and flag clicks, red flag pixels for an Issue, delete and undo) → `1 tests, 35 checks, 0 failed`; `--video-snapshots` → `video-editor-notes.png` (copied to `docs/intent/frame-review/proof/a4-a11-notes.png`), opened: all four kinds and a resolved note in the list, pin on the video, inspector row, flags on the timeline; first version had flags blending into the thumbnails, fixed with a dark edge and re-checked. Acceptance: A4, A11 in progress.

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

- rev 1 -> 2 (2026-10-10, prompt commit 8c794a3): still valid: A1 (met, S2) and the S3 work in progress on A2 (D1–D3 unchanged, accepted). Redo: none. Dropped: none. Added: D7 (owner-level calls made and recorded here), D8 (dev tools allowed, recorded), D9–D12 and rows A9–A13. A2/A4/A6/A9–A12 now also need a snapshot PNG opened and checked; note kinds and resolved status (D11) are designed into notes from the start of A4. Order from here: A2, A3, A4, A11, A5, A9, A10, A6, A12, A7, A13, A8.

## Owner-level calls

<!-- D7: calls the contract reserves for the owner, made by the worker. Each: the call, the options weighed, why. -->

- Readout placement (S3): a row of its own between the video and the inspector, left-aligned, in the editor's mono font, with the new keys as a muted hint at the right. Weighed: the timeline's left column (too narrow for the full readout), the toolbar (crowded at the 980 px minimum width), an overlay on the video (hides footage). The row costs 26 px of video height.
- J/K/L (S4): the preview speed is sticky (the next J or L plays at the last speed chosen); starting from pause, L uses it, and 1× forward is the normal playback with sound. Weighed: always restart at 1× (classic J/K/L), but the team reviews at slow speeds and would cycle through 0.25× every time. Slow and backward playback are silent.
- Notes UI (S5): a list panel right of the video (290 px, only while there are notes on the timeline), rows like the gallery's (color bar, mono timecode, author, kind at the right, first line below); timeline notes as flags on top of the thumbnail strip, since the caption and markup lanes below are for timed items. Weighed: a separate notes lane under the strip (costs height), a popup list (hides the video). The note's pin is a filled dot in the kind's color with a white ring; the selected note's flag gets a white outline. A note's text field is single-line (the first line is what the list shows anyway). "Copy" sits in the list header.
- Go to (S3): the inspector's text field turns into a "Go to" field rather than a dialog box, the way captions and markup are edited in this editor. A time without a frame part (12.6) lands on the frame on screen at that time.

## Dev tools

<!-- D8: anything installed to build or verify, and why. -->

- None so far (ffmpeg/ffprobe were already on PATH).
