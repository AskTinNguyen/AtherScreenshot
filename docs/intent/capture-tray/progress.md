# Capture Tray: Progress

- Working under rev: 1
- Worker: `capture-tray-worker`
- Current step: S2: tray snapshots on the runner (A5); S3: the Mac tray (A6)
- Next step: S3: the Mac tray (A6), then docs (A7)
- PR: none yet

## Acceptance

<!-- One row per acceptance id in prompt.md, same order. Verdict is exactly met or open; met needs evidence (commit / command and its result line). -->

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S1: CI https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39615326089 (bc7c936): `Built build\AtherScreenshot.exe`, `117 tests, 1819 checks, 11 failed`; the 11 are runner baseline video failures (S0), `editor_tray_routes_captures_to_the_open_editor` ok |
| A2 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_keeps_each_pictures_document` ok |
| A3 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_save_selected_and_done` and `editor_tray_close_prompt_answers` ok (the prompt's answers injected) |
| A4 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_strip_clicks_keys_and_wheel`, `editor_tray_holds_at_most_100_pictures`, `editor_tray_memory_for_40_captures` ok; measured `40 pictures of 2560 x 1440 in one tray: private bytes 679 MB, 593 MB more than before` |
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
- S1 (rev 1, 2026-10-21): the Windows tray, commit bc7c936.
  - **Per-picture documents:** a `Picture` holds a picture's document (base, source, `DocState`, undo/redo, dirty, version) plus the tray's own fields (id, tick, saved, thumbnail). The current picture's document stays in the editor's members, so the editor code is unchanged; switching swaps it with the slot (`Stash`/`Load`) and drops the composed bitmaps of the picture left. One picture: `tray_` has one slot, no strip, everything as before.
  - **Routing:** `OpenCaptureInEditor` (new, editor.h) is what `Deliver` calls for captures headed for the editor. It adds to the most recently used window opened for a capture (`takesCaptures_`), so collages, windows on a file, the toast's and the pin's "edit" never receive captures. `[Editor] Tray=1` (settings.ini template, upgrade key, `Settings::editorTray`, Settings › Capture › Collect captures in one editor) is passed through `EditorOptions::tray`.
  - **Strip (D2):** from two pictures, 96 px above the status bar: thumbnails at their own shape in 120×68 boxes, the current one in a lime ring, tick top-left, × top-right on hover, a dot for unsaved changes, a "✓ Saved" badge, the count and Save selected (n) at the right. A click opens a picture, the wheel scrolls the strip sideways, Ctrl+Tab / Ctrl+Shift+Tab / Ctrl+PageDown / Ctrl+PageUp switch, the status bar explains what's under the mouse, and Ctrl+K lists Save selected / Next / Previous picture.
  - **Saving (D4, D5):** `SavePicture(i)` is Save for any picture (the stored ones composed with the same `Compose`); saves report back by picture id, so a save finishing after a switch marks the right picture. Save selected saves the ticked pictures that are unsaved or changed since. Done in a tray saves those, copies the current one, closes; it asks first when an unticked picture would be lost. Closing asks "n pictures aren't saved" (TaskDialog: Save selected and close (default) / Close without saving / Cancel). Auto-redact results go to the picture they were found in.
  - **Limit and memory (D6):** 100 pictures; the 101st capture opens its own window with a toast. Only the current picture keeps composed bitmaps.
  - **Tests (7 new):** `editor_tray_routes_captures_to_the_open_editor`, `editor_tray_keeps_each_pictures_document`, `editor_tray_save_selected_and_done`, `editor_tray_close_prompt_answers`, `editor_tray_strip_clicks_keys_and_wheel`, `editor_tray_holds_at_most_100_pictures`, `editor_tray_memory_for_40_captures`. They drive hidden editors through their mouse and key handlers (`Key(vk, ctrl, shift)` split out of `OnKeyDown`), with the prompts' answers, the clipboard and the library injected. MinGW type-check clean.
  - **Evidence:** CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39615326089: `Built build\AtherScreenshot.exe`, no `warning C`; `117 tests, 1819 checks, 11 failed`. The 11 are baseline video failures from S0 (`video_editor_plays_and_seeks` passed this time). All 16 `editor_*` tests ok, the existing 9 included. Memory line: `(40 pictures of 2560 x 1440 in one tray: private bytes 679 MB, 593 MB more than before)`.
  - Acceptance: A1, A2, A3, A4.

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

## Owner-level calls

<!-- D8: calls the contract reserves for the owner, made by the worker. Each: the call, the options weighed, why. -->

- **Which window collects (S1):** the most recently used window that was opened for a capture. A window opened on a file, by the capture toast or from a pin never collects, and neither does a collage. Weighed: any editor that isn't a collage, as D1's first line reads. But then one "edit last capture" or a file opened from the gallery would start collecting session captures, and D1's own reason ("a saved file isn't mixed with session captures") argues against that.
- **The auto-redact failure path (S1):** it still opens its own window. It's a warning ("redact it yourself"), not a capture you asked to annotate.
- **× (S1):** removes at once, unless the picture has changes that aren't saved; then it asks "Remove this picture and discard your changes?", with No as the default. That mirrors the one-picture editor, which only asks on close when there are changes. Weighed: always asking (a question on every unwanted capture), and never asking (one slip loses annotations).
- **Back to one picture (S1):** removing pictures down to one hides the strip, and the editor behaves as it always did: Done copies and saves per Save to file, and closing asks only when there are changes. Weighed: keeping the tray's rules for one picture, but then it would look like a plain editor and act differently.
- **Done saves the ticked pictures even with Save to file off (S1):** the ticks are the explicit choice. Weighed: following Save to file, but then Done would just close and lose the ticked pictures.
- **Save selected (n) (S1):** n counts the ticked pictures that aren't saved yet or changed since, so a picture already saved isn't written twice. "4 pictures · 3 selected" counts ticks. At 0 the button is dimmed and a click says "Nothing new to save". Weighed: n = all ticked pictures, which would save duplicates of saved pictures.
- **The 101st capture (S1):** opens in a window of its own, with a toast saying the editor holds 100 pictures and to save or remove some. The next captures join that window. Weighed: refusing the capture (it isn't saved anywhere else, so it would be lost), and a modal question (it would interrupt the capture flow).
- **Strip look (S1):** a 96 px band in the bars' surface color with a top hairline. Thumbnails are centered in 120×68 boxes at their own shape; very tall or wide ones show their top or left part, at least half a box wide and two thirds of a box high, so the controls fit. The current one has a 3 px lime ring, the others a hairline (lighter under the mouse).
  - **Tick:** a lime box with a dark check, or an empty box with a light border.
  - **Unsaved changes:** a white dot with a dark ring, bottom right.
  - **Saved:** a "✓ Saved" pill bottom left, just ✓ on narrow pictures.
  - **×:** a dark circle top right, red under the mouse.
  - **Right end:** the count above a lime "Save selected (n)" button.
  - Weighed: Snagit's plain thumbnails without ticks. But D2 asks for ticks, the dot and the badge.
- **Close prompt (S1):** a TaskDialog: "3 pictures aren't saved", a line saying unsaved pictures are gone once the editor closes, and Save selected and close (default) / Close without saving / Cancel. If TaskDialog isn't available it falls back to Yes/No/Cancel. Done's version of the prompt says the pictures aren't ticked.
- **Title and toasts (S1):** the window title reads "Annotate — picture 2 of 5 — 1600 × 900" (one picture: as before). Save selected ends with one toast, "Saved 3 pictures", naming the first file and "and 2 more"; a click shows them in Explorer.
- **Memory (S1, D6):** pictures that aren't current keep their screenshot uncompressed in memory, plus a thumbnail, and nothing else. Measured: 40 captures of 2560×1440 = 679 MB private bytes. 100 captures of 4K would be about 3.3 GB. Weighed: PNG in memory or a temp folder for inactive pictures. That's slower to switch and has more to go wrong, and it isn't needed for D6's bound.
