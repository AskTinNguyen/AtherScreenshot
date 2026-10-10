# Frame Review: Progress

- Working under rev: 2
- Worker: `frame-review-worker`
- Current step: S4 (A3), J/K/L review playback with 0.25×/0.5×/1× preview speeds.
- Next step: A4 (notes in the editor, with D11 kinds and status built in); then A11, A5, A9, A10, A6, A12, A7, A13, A8 (rev 2 order).
- PR: none yet

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S2: `test.bat` → `86 tests, 1055 checks, 0 failed` with `video_editor_steps_every_frame_at_60fps` (fails before the fix, S1) and `video_frame_times_match_the_decoded_frames` |
| A2 | met | S3: `test.bat` → `88 tests, 1117 checks, 0 failed` with `video_frame_readout_and_go_to` (60, 30, 29.97 fps, joined) and `video_editor_readout_and_go_to`; snapshot `docs/intent/frame-review/proof/a2-readout.png` (opened: "0:12:37 · frame 757 · 60 fps" readable under the video) |
| A3 | open | |
| A4 | open | |
| A5 | open | |
| A6 | open | |
| A7 | open | |
| A9 | open | |
| A10 | open | |
| A11 | open | |
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

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

- rev 1 -> 2 (2026-10-10, prompt commit 8c794a3): still valid: A1 (met, S2) and the S3 work in progress on A2 (D1–D3 unchanged, accepted). Redo: none. Dropped: none. Added: D7 (owner-level calls made and recorded here), D8 (dev tools allowed, recorded), D9–D12 and rows A9–A13. A2/A4/A6/A9–A12 now also need a snapshot PNG opened and checked; note kinds and resolved status (D11) are designed into notes from the start of A4. Order from here: A2, A3, A4, A11, A5, A9, A10, A6, A12, A7, A13, A8.

## Owner-level calls

<!-- D7: calls the contract reserves for the owner, made by the worker. Each: the call, the options weighed, why. -->

- Readout placement (S3): a row of its own between the video and the inspector, left-aligned, in the editor's mono font, with the new keys as a muted hint at the right. Weighed: the timeline's left column (too narrow for the full readout), the toolbar (crowded at the 980 px minimum width), an overlay on the video (hides footage). The row costs 26 px of video height.
- Go to (S3): the inspector's text field turns into a "Go to" field rather than a dialog box, the way captions and markup are edited in this editor. A time without a frame part (12.6) lands on the frame on screen at that time.

## Dev tools

<!-- D8: anything installed to build or verify, and why. -->

- None so far (ffmpeg/ffprobe were already on PATH).
