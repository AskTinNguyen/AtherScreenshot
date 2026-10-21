# Bug Report: Progress

- Working under rev: 2
- Worker: `bug-report-worker`
- Current step: S1: Windows bug logic, bundle, voice commands and app wiring (`src/bugreport.*`), first CI run
- Next step: S1: Windows bug state machine and bundle (`src/bugreport.*`) with self-tests
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

- S0 (rev 1→2, 2026-10-21): baseline before any feature code.
  - Windows (CI, commit 3caeb54): https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39611602014 → `Built build\AtherScreenshot.exe`, self-tests `110 tests, 1674 checks, 12 failed`. **Baseline failures (on `main` too, not this feature):** `video_editor_plays_and_seeks`, `video_editor_steps_every_frame_at_60fps`, `video_editor_readout_and_go_to`, `video_editor_review_playback_jkl`, `video_editor_notes_add_edit_pin_jump_undo`, `video_editor_notes_kept_next_to_the_video`, `video_editor_timeline_zoom`, `video_editor_saves_review_video_notes_list_and_sheet`, `video_frame_times_match_the_decoded_frames`, `video_export_trim_speed_crop_captions` (first-frame color after a trim), `video_export_joins_pieces_from_several_encoders`, `video_review_export_holds_notes_with_cards_and_burn_in`. All are frame-exact video checks on the runner's software video path. `video_transcribe_speech_with_word_times` passed (22 s): the runner has a SAPI recognizer and a TTS voice, so A2 can be proven there.
  - Mac: `cd macos && ./build.sh` → `Built build/Ather Screenshot.app (0.0.2)`; `xcrun swift test` → `Executed 87 tests, with 9 tests skipped and 0 failures (0 unexpected)`.

## Owner-level calls

<!-- D9: calls the spec leaves open, with the options considered. -->

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

- rev 1 -> 2 (2026-10-21, before any feature code): still valid D1–D9 and A1–A6, A8, A9 as written; nothing to redo (no code yet). Added: D10 (Mac-only opt-in `[Bugs] CloudVietnamese=0`, `vi-VN` server transcription of `voice.wav` at "bug stop"), built with the Mac side under A7, with a seam test that records the request's locale and on-device flag and makes no network call. F-2 is accepted. CI artifacts may now be downloaded (own branch only, into `/private/tmp/claude-501/bug-report-ci/<run id>/`).
