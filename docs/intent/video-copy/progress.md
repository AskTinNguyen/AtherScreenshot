# Video Copy: Progress

- Working under rev: 1
- Worker: `video-copy-worker`
- Current step: S1: Windows range, Copy clip, Copy frame, Save menu, self-tests and snapshots (waiting for the CI run)
- Next step: S2: read the CI run (tests, snapshots); then the Mac side
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
| A9 | open | |

## Steps

<!-- S<n> (rev <r>, <YYYY-MM-DD>): <what was done>. Evidence: <commit / command and result line / PR>. Acceptance: <A-ids moved>. -->

- S0 (rev 1, 2026-10-21): baseline before any change.
  - Windows, the CI run on the intent commit 8856254 (main's code): https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39611596945 → `110 tests, 1674 checks, 11 failed`. These 11 are the runner's baseline failures, not this intent's: video_editor_steps_every_frame_at_60fps, video_editor_readout_and_go_to, video_editor_review_playback_jkl, video_editor_notes_add_edit_pin_jump_undo, video_editor_notes_kept_next_to_the_video, video_editor_timeline_zoom, video_editor_saves_review_video_notes_list_and_sheet, video_frame_times_match_the_decoded_frames, video_export_trim_speed_crop_captions, video_export_joins_pieces_from_several_encoders, video_review_export_holds_notes_with_cards_and_burn_in. Their notes show the runner (no GPU) reading every frame of the test clips one frame later than its number ("0:00:31 (frame 30)", "output 180 shows 14, wanted 15"), so checks that tie a frame's content to `index / fps` fail there; frame counts, order and exporting work. The snapshot step rendered nothing (relative folders; fixed by the orchestrator in 01308a9).
  - Mac: `cd macos && ./build.sh && xcrun swift test` → `Executed 87 tests, with 9 tests skipped and 13 failures`, all 13 in two tests: `testClipLaneEdgesKnobAndKeys` (the known flaky one, "timed out: scrubbed to 2 s") and `testEditorJoinsSplitsAndReordersClips` ("timed out: seeked", then its follow-on asserts), while two other workers built and tested on this Mac. Rerun alone: `xcrun swift test --filter ClipEditorTests` → `Executed 4 tests, with 1 test skipped and 0 failures`.

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

## Owner-level calls

<!-- D7: calls the contract reserves for the owner, made by the worker. Each: the call, the options weighed, why. -->

## Engineering notes

- The new Windows tests compare with what the editor itself shows and renders on the same machine (`ShownFrame` at the range's ends, `FrameRenderer` for the frame, the export's own first frame through `g_exportTap`), never with `frame index / fps`, so the runner's one-frame offset (S0) neither fails nor passes them by itself.
