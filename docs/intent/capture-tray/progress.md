# Capture Tray: Progress

- Working under rev: 1
- Worker: `capture-tray-worker`
- Current step: none
- Next step: review of the docs (A7); the owner's check on a real PC (A8)
- PR: https://github.com/AskTinNguyen/AtherScreenshot/pull/11 (draft)

## Acceptance

<!-- One row per acceptance id in prompt.md, same order. Verdict is exactly met or open; met needs evidence (commit / command and its result line). -->

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S1: CI https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39615326089 (bc7c936): `Built build\AtherScreenshot.exe`, `117 tests, 1819 checks, 11 failed`; the 11 are runner baseline video failures (S0), `editor_tray_routes_captures_to_the_open_editor` ok |
| A2 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_keeps_each_pictures_document` ok |
| A3 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_save_selected_and_done` and `editor_tray_close_prompt_answers` ok (the prompt's answers injected) |
| A4 | met | S1: same run, `117 tests, 1819 checks, 11 failed` (baseline video failures only); `editor_tray_strip_clicks_keys_and_wheel`, `editor_tray_holds_at_most_100_pictures`, `editor_tray_memory_for_40_captures` ok; measured `40 pictures of 2560 x 1440 in one tray: private bytes 679 MB, 593 MB more than before` |
| A5 | met | S2: snapshot `editor-tray.png` from `--editor-snapshots` on CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39619548766 (S5), opened and checked: status bar "Picture 3 of 5", five captures, the third outlined in lime, the second unticked, the fourth "✓ Saved", the fifth with the unsaved dot, × on the first under the mouse, "5 pictures · 4 selected", "Save selected (3)"; kept as `docs/intent/capture-tray/proof/a5-win-editor-tray.png` (and `a5-win-editor-tray-scrolled.png`: 14 pictures, the strip scrolled to the newest) |
| A6 | met | S3: `cd macos && ./build.sh && xcrun swift test` → `Built build/Ather Screenshot.app (0.0.2)`, `Executed 95 tests, with 10 tests skipped and 0 failures`, with 7 new `TrayTests` (A1–A4 mirrored); snapshot `docs/intent/capture-tray/proof/a6-mac-editor-tray.png` (`ATHER_TEST_OUT`, `UISnapshotTests.testEditorTray`), opened and checked |
| A7 | open | S4: README (Features row, Annotation editor › Capture tray, the keys), macos/README.md (Editor extras, Differences), RELEASE_NOTES (Coming in the next update) written; waits for a review |
| A8 | open | owner: on a real PC, capture eight, annotate three, switch, save five together |

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
- S2 (rev 1, 2026-10-21): tray snapshots (A5). Two states added after the existing ones in `--editor-snapshots`: `editor-tray` (five captures: the third current, with notes; the second unticked; the fourth saved; the fifth with unsaved changes; the mouse over the first) and `editor-tray-scrolled` (14 pictures, the newest current and scrolled into view, the mouse on Save selected). The existing states are untouched (same list and order, single-picture code paths unchanged).
  - First render (run 39615326089): the empty tick read as a dark blob, and the saved picture was a tall one, so its badge showed only ✓. Fixed in bd79c46: the empty tick is a translucent box with a light border, and the saved picture is a wide one.
  - Evidence: CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39616296019 (bd79c46): `Built build\AtherScreenshot.exe`, no `warning C`; `117 tests, 1819 checks, 12 failed`, which is exactly the 12 baseline video failures of S0. All 7 `editor_tray_*` tests ok; memory line `(40 pictures of 2560 x 1440 in one tray: private bytes 640 MB, 593 MB more than before)`.
  - Artifact downloaded to the scratch folder (owner's permission, L-2), PNGs opened. Kept: `docs/intent/capture-tray/proof/a5-win-editor-tray.png`, `a5-win-editor-tray-scrolled.png`.
  - The runner's display is 1024×768, so every editor snapshot (the existing ones too) is 1028×749 with a crowded toolbar. That's the runner, not this change.
  - Acceptance: A5.
- S3 (rev 1, 2026-10-21): the Mac tray (A6).
  - **Pictures:** `TrayPicture` in Editor.swift. The current picture's document lives in the editor and canvas, and is copied to its slot when saving or switching away (`syncCurrent`); the canvas swaps documents with `load(base:state:undo:redo:)`. `base` became a `var` in `Editor` and `EditorCanvas`.
  - **Routing:** `Editor.capture` (App.swift's `deliver` for After = edit) joins the most recently used window opened for a capture (`takesCaptures`; `windowDidBecomeKey` keeps `Editor.instances` most-recent-last), never a collage or a window on a file.
  - **Setting:** `Tray` (default on), Settings › General › Collect captures in one editor.
  - **Strip:** `TrayStrip`, the same layout and marks as Windows. A click opens a picture, the tick toggles, × removes (a sheet asks if the picture has changes), the wheel scrolls; ⌃⇥ / ⌃⇧⇥ and ⌘} / ⌘{ switch.
  - **Saving:** Save selected, Done (asks if an unticked picture would be lost), and the close sheet (Save Selected and Close (default) / Close Without Saving / Cancel). Saves report back by picture id; auto-redact results go to the picture they came from.
  - **Test plumbing:** `copyImage(_:to:)` takes a pasteboard, so tests use a uniquely named one, never the general one. The editor's library and the prompts' answers can be injected. `Editor.keepOffscreen` keeps test windows off screen.
  - **Tests:** `TrayTests` (7: routing, each picture's document, Save selected and Done with ⌘S pixel-identical, the close prompt's answers, the strip, the 100 limit, memory with 40 random 2560×1440 pictures) and `UISnapshotTests.testEditorTray`.
  - **Evidence:** `cd macos && ./build.sh && xcrun swift test` → `Built build/Ather Screenshot.app (0.0.2)`, `Executed 95 tests, with 10 tests skipped and 0 failures`, exit 0. Memory line: `TrayTests: 40 pictures of 2560 x 1440 in one tray: footprint 846 MB, 388 MB more than before`. The whole test process's footprint stays under 1.5 GB; growth is checked only as at least the pictures held, since memory freed by earlier tests is reused.
  - **Flaky runs:** three earlier full runs each failed `ClipEditorTests.testClipLaneEdgesKnobAndKeys` ("timed out: scrubbed to 2 s"), the known flake. Alone it passed 3 of 3. On the base commit 82383f5 (`git archive` into the scratch folder, `--filter ClipEditorTests`), it failed 3 of 5 runs too (load average ~5, with two other workers building), so it isn't this change. One run before the memory check was fixed failed `testTrayMemoryFor40Captures` on the growth check (388 MB; memory reuse), which led to the absolute check above.
  - **Snapshot:** `ATHER_TEST_OUT=/private/tmp/claude-501/capture-tray-snapshots/mac xcrun swift test --filter UISnapshotTests.testEditorTray` → `docs/intent/capture-tray/proof/a6-mac-editor-tray.png`, opened: the same five states as Windows.
  - Acceptance: A6.
- S5 (rev 1, 2026-10-21): fixes from the orchestrator's review of the A5 image.
  - **Picture number:** the status bar's "Picture 1 of 5" was the tip for the thumbnail under the mouse, and it read as the current picture. The right side of the status bar now names the current picture ("Picture 3 of 5 · 1400 × 900 · …"; one picture: as before), and the tip reads "Click to open picture 1". `editor_tray_strip_clicks_keys_and_wheel` checks the label follows Ctrl+Tab / Ctrl+Shift+Tab.
  - **Toolbar overlap:** the overlap at the top right isn't from the tray. In run 39616296019 the toolbar rows of `editor-tray.png` and the one-picture `editor-annotations.png` are pixel-identical (PIL difference bbox `None`). It comes from the runner's 1028 px wide window, and the toolbar code is unchanged from `main`. Left as is.
  - **Evidence:** CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39619548766: `Built build\AtherScreenshot.exe`, no `warning C`, `117 tests, 1823 checks, 11 failed` (baseline video failures only), all `editor_tray_*` ok. New `editor-tray.png` opened: "Picture 3 of 5" with the third outlined. Proof images replaced.
- S4 (rev 1, 2026-10-21): docs (A7).
  - **README:** a Capture tray row in Features; the Ctrl+Tab / Ctrl+PageDown keys in the editor's shortcuts; a "Capture tray" part in the Annotation editor guide (what joins and what doesn't, "set After capture to Open in editor to collect every capture", the strip, ticks, marks, Save selected, Done, closing, the 100 limit, `[Editor] Tray=0`).
  - **macos/README.md:** a Capture tray item in Editor extras (⌃⇥ / ⌘} keys, the setting) and a line in Differences.
  - **RELEASE_NOTES.md:** a new "Coming in the next update (Windows and macOS)" section.
  - Acceptance: A7 is ready for review.

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
- **Mac (S3):** the same calls as Windows. The setting is under Settings › General, next to After capture, because the Mac groups After capture there. The prompt buttons are in title case, as macOS does. ⌘} / ⌘{ switch too: on Windows Ctrl+PageDown / Ctrl+PageUp, on the Mac the usual next and previous tab keys. Pictures in a tray share the window's display scale.
- **Memory (S1, D6):** pictures that aren't current keep their screenshot uncompressed in memory, plus a thumbnail, and nothing else. Measured: 40 captures of 2560×1440 = 679 MB private bytes. 100 captures of 4K would be about 3.3 GB. Weighed: PNG in memory or a temp folder for inactive pictures. That's slower to switch and has more to go wrong, and it isn't needed for D6's bound.
