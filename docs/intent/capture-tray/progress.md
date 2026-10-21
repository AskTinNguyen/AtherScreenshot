# Capture Tray: Progress

- Working under rev: 1
- Worker: `capture-tray-worker`
- Current step: S1: the Windows tray (per-picture documents, routing, strip, Save selected, close prompt) with self-tests
- Next step: S2: tray snapshots on the runner (A5)
- PR: none yet

## Acceptance

<!-- One row per acceptance id in prompt.md, same order. Verdict is exactly met or open; met needs evidence (commit / command and its result line). -->

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

- S0 (rev 1, 2026-10-21): baseline before any change.
  - Windows: CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39611610200 (commit 3482e85). `Built build\AtherScreenshot.exe`; self-tests `110 tests, 1674 checks, 12 failed`. All 12 are video tests that fail on the runner without any change here (baseline failures, not this intent's): `video_editor_plays_and_seeks`, `video_editor_steps_every_frame_at_60fps`, `video_editor_readout_and_go_to`, `video_editor_review_playback_jkl`, `video_editor_notes_add_edit_pin_jump_undo`, `video_editor_notes_kept_next_to_the_video`, `video_editor_timeline_zoom`, `video_editor_saves_review_video_notes_list_and_sheet`, `video_frame_times_match_the_decoded_frames`, `video_export_trim_speed_crop_captions`, `video_export_joins_pieces_from_several_encoders`, `video_review_export_holds_notes_with_cards_and_burn_in`. All 9 `editor_*` tests pass.
  - The run's Snapshots step rendered nothing ("File Not Found"): the tools got relative folders, and SHCreateDirectoryExW needs an absolute one. Reported to the orchestrator, who fixed check.yml on this branch (82383f5).
  - Mac: `./build.sh` built; `xcrun swift test`: `Executed 87 tests, with 9 tests skipped and 1 failure`. The failure is `testCancelLeavesNoPieces`, which counts `ather-pieces-*` folders in the temp folder that three workers share. Run alone (`xcrun swift test --filter testCancelLeavesNoPieces`): `Executed 1 test, with 0 failures`. Treated as a known parallel-run flake.

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->
