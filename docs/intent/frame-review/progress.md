# Frame Review: Progress

- Working under rev: 1
- Worker: `frame-review-worker`
- Current step: S1 (A1), the self-test that makes a 60 fps clip with a different color on every frame and steps through it with the editor's keys, run before fixing `Step` to see it fail.
- Next step: S2, exact stepping (frame times read from the file, a timeline frame list, `Step` on it).
- PR: none yet

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | open | |
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
- S1 (rev 1, 2026-10-10): test first for A1. New self-test `video_editor_steps_every_frame_at_60fps` writes a 2 s 60 fps clip whose every frame has its own color (and a 2 s 30 fps one with blue), presses ←/→ (and Shift, Home, End) through the editor's key handler (`Key`, split out of `OnKey` so tests can pass modifiers), and reads each decoded frame's color back. Run before the fix, it fails the way the intent says: `test.bat --nobuild video_editor_steps` → `1 tests, 14 checks, 1 failed`, with "60 fps forward: a2 a4 a6 a8 …" (every other frame). Evidence: commit of S1 (see git log). Acceptance: A1 (test in place, failing).

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->
