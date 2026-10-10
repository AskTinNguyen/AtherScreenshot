# Frame Review: Progress

- Working under rev: 1
- Worker: `frame-review-worker`
- Current step: S3 (A2), the frame readout `m:ss:ff · frame n · fps` and Ctrl+G go to.
- Next step: S4 (A3), J/K/L review playback with 0.25×/0.5×/1× preview speeds.
- PR: none yet

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S2: `test.bat` → `86 tests, 1055 checks, 0 failed` with `video_editor_steps_every_frame_at_60fps` (fails before the fix, S1) and `video_frame_times_match_the_decoded_frames` |
| A2 | open | |
| A3 | open | |
| A4 | open | |
| A5 | open | |
| A6 | open | |
| A7 | open | |
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

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->
