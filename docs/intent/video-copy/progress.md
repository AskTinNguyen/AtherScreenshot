# Video Copy: Progress

- Working under rev: 1
- Worker: `video-copy-worker`
- Current step: none
- Next step: the orchestrator's review of A8 (docs) and F-1 (A4 on the runner); A9 is the owner's (paste into Slack on a PC and a Mac)
- PR: https://github.com/AskTinNguyen/AtherScreenshot/pull/10 (draft)

## Acceptance

<!-- One row per acceptance id in prompt.md, same order. Verdict is exactly met or open; met needs evidence (commit / command and its result line). -->

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | S5: CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39617583897 → `ok   video_editor_marks_a_range_to_copy`; the run's line `114 tests, 1770 checks, 11 failed` is exactly the runner's baseline set from S0 (F-1) |
| A2 | met | S5: same run → `ok   video_editor_copies_the_marked_range_as_a_clip` (120 frames, the editor's own first and last frames, CF_HDROP = that path, H.264 + AAC, the clip saved with the clipboard held elsewhere) and `ok   video_editor_copies_the_selected_clip`; same gate note (F-1) |
| A3 | met | S5: same run → `ok   video_editor_copies_the_frame_on_screen` (CF_DIB and PNG pixel-equal to `FrameRenderer`'s cropped frame with a caption and a box, and to the export's own first frame there; a focused field's Ctrl+C copies its text); same gate note (F-1) |
| A4 | open | Waits on F-1: S5's run fails exactly main's 11 runner-baseline tests (S0) and nothing else; Save, Save GIF, Save smaller and the review video are untouched (`ExportMp4` is only called) |
| A5 | met | S2: `--video-snapshots` on the runner (run 39615745081), opened: `docs/intent/video-copy/proof/a5-range.png` (band from frame 300 to 671, white grips, "6.2 s · Ctrl+C copies") and `proof/a5-save-menu.png` (the real Save ▾ menu: Copy clip Ctrl+C, Copy frame Ctrl+Shift+C) |
| A6 | met | S3: `cd macos && ./build.sh && xcrun swift test` → `Executed 91 tests, with 10 tests skipped and 0 failures`, with `VideoCopyTests` (⇧-drag, grips, ⇧I/⇧O, Esc; H.264 + AAC clip of 30 frames 20…49, file URL on a private pasteboard; selected clip; ⌘C/⌘⇧C frame equal to the export's frame, difference 0 of 255; the general pasteboard's changeCount unchanged) |
| A7 | met | S3: `ATHER_TEST_OUT=… xcrun swift test --filter VideoCopyTests` → `proof/a7-mac-range.png` (opened: band, grips, "6.2 s · ⌘C copies", Copy ▾ next to Save smaller) |
| A8 | open | S4: written (README Features row, Video editor guide, shortcut line, Platform differences; `macos/README.md`; `RELEASE_NOTES.md` "Coming in the next update"); ready for review |
| A9 | open | owner: paste a copied clip and a copied frame into Slack on a PC and a Mac |

## Steps

<!-- S<n> (rev <r>, <YYYY-MM-DD>): <what was done>. Evidence: <commit / command and result line / PR>. Acceptance: <A-ids moved>. -->

- S0 (rev 1, 2026-10-21): baseline before any change.
  - Windows, the CI run on the intent commit 8856254 (main's code): https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39611596945 → `110 tests, 1674 checks, 11 failed`. These 11 are the runner's baseline failures, not this intent's: video_editor_steps_every_frame_at_60fps, video_editor_readout_and_go_to, video_editor_review_playback_jkl, video_editor_notes_add_edit_pin_jump_undo, video_editor_notes_kept_next_to_the_video, video_editor_timeline_zoom, video_editor_saves_review_video_notes_list_and_sheet, video_frame_times_match_the_decoded_frames, video_export_trim_speed_crop_captions, video_export_joins_pieces_from_several_encoders, video_review_export_holds_notes_with_cards_and_burn_in. Their notes show the runner (no GPU) reading every frame of the test clips one frame later than its number ("0:00:31 (frame 30)", "output 180 shows 14, wanted 15"), so checks that tie a frame's content to `index / fps` fail there; frame counts, order and exporting work. The snapshot step rendered nothing (relative folders; fixed by the orchestrator in 01308a9).
  - Mac: `cd macos && ./build.sh && xcrun swift test` → `Executed 87 tests, with 9 tests skipped and 13 failures`, all 13 in two tests: `testClipLaneEdgesKnobAndKeys` (the known flaky one, "timed out: scrubbed to 2 s") and `testEditorJoinsSplitsAndReordersClips` ("timed out: seeked", then its follow-on asserts), while two other workers built and tested on this Mac. Rerun alone: `xcrun swift test --filter ClipEditorTests` → `Executed 4 tests, with 1 test skipped and 0 failures`.

- S1 (rev 1, 2026-10-21): Windows, all in `src/videoeditor.cpp` (plus a `withPng` option on `CopyImageToClipboard` in `src/output.*`).
  - The range: `marked` (a selection outside `VideoEdit`), Shift+drag anywhere on the timeline, grips on its edges in the thumbnail row, Shift+I/Shift+O (`MarkAtPlayhead`), snapped to frame boundaries (`SnapEdge`: frame starts and the timeline's end) and clamped to the trim (`SetMarked`, also after any edit); Esc clears it first, a plain click outside it clears it, a clip change clears it, Esc mid-drag puts it back. Painted by `PaintMarked` through `TX`, so it follows zoom and pan.
  - Ctrl+C (`Copy`): the range, else the selected clip (its part inside the trim), else the frame. `CopyClip` runs `ExportMp4` on the edit with the trim set to the range (Save's pattern: temp file, `MakeCapturePath`, `NoteEdit`, `WM_SAVED`), then `CopyFileToClipboard`; a second Ctrl+C while saving is ignored (`busy`); if the clipboard can't be opened the file is still saved and the toast says so. Ctrl+Shift+C (`CopyFrame`): `FrameRenderer(edit, videoSize, false)` on the frame on screen, CF_DIB + PNG, toast with the readout and the picture; right after a step it waits for that frame's decode.
  - Save ▾ gets Copy clip (greyed without a range or selected clip) and Copy frame; the hint line gets "Shift+drag mark · Ctrl+C copy".
  - Self-tests: `video_editor_marks_a_range_to_copy`, `video_editor_copies_the_marked_range_as_a_clip`, `video_editor_copies_the_selected_clip`, `video_editor_copies_the_frame_on_screen`. Snapshots: `video-editor-range.png`, `video-editor-save-menu.png` (the real menu, opened with `TrackPopupMenu` and printed while it's up).
  - MinGW type-check of both files: no errors, no new warnings (`-Wall -Wshadow`; renamed two locals that shadowed the member `s`).
  - Evidence: commit c1dd8ee; CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39614953241 → `114 tests, 1770 checks, 12 failed`: the 11 baseline failures plus `video_editor_copies_the_marked_range_as_a_clip` ("the editor shows 40…-1": the runner's first decode took over 2 s, so the test's sample of the last frame came back empty); the other three new tests `ok`. No video snapshots: `--video-snapshots` never created its folder.
- S2 (rev 1, 2026-10-21): fixes from S1's run: `VideoEditorSnapshots` creates its folder (as the editor and gallery tools do); the new tests wait up to 8 s for a frame (`FrameOnScreen`). Evidence: commit 73eede2; CI run https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39615745081 → `114 tests, 1770 checks, 12 failed`, all four new tests `ok`; the 12 are the 11 baseline failures plus `video_editor_plays_and_seeks` ("rawT after playing 0.600000", real-time playback; it passed in the S0 and S1 runs with the same playback code). Snapshots rendered and opened: `proof/a5-range.png`, `proof/a5-save-menu.png`. Acceptance: A1, A2, A3, A5.
- S3 (rev 1, 2026-10-21): Mac, with ⌘.
  - `VideoEditor.swift`: `marked` (a `Range<Double>`), ⇧-drag and grips on the `Timeline` (a `.range` drag; Esc puts the old range back), ⇧I/⇧O, Esc, frame snapping at the first clip's rate (the composition's), the band with grips and "6.2 s · ⌘C copies", a Copy ▾ menu left of Save smaller (Copy clip greyed via `menuNeedsUpdate`), ⌘C (`copyPicked`: range, else selected clip, else frame), ⌘⇧C (`copyFrame`), and `VideoExport.frame(_:at:)`: the export's composition read at `t` with `AVAssetImageGenerator` (zero tolerance), at the export's size. The editor writes to `pasteboard` (`.general` by default; tests pass a named one).
  - `Compress.clip`: the edit rendered as Save renders it, frame for frame, encoded by Compress's H.264 + AAC writer (`encodeEdit`) at full size (scaled only past 4096 × 2304), at Windows' export rate (w × h × fps × 0.12, 2–50 Mbps). Save and Save smaller are unchanged.
  - `Util.swift`: `copyImage` and `copyFile` take a pasteboard (default `.general`).
  - Tests: `VideoCopyTests` (4 tests; the snapshot one only with `ATHER_TEST_OUT`). The test clip shows each frame's number as eight black-and-white stripes, since 16-step color codes drifted by one step through the Mac's color handling.
  - Evidence: `cd macos && ./build.sh && xcrun swift test` → `Executed 91 tests, with 10 tests skipped and 0 failures` (exit 0; `testClipLaneEdgesKnobAndKeys`, `testEditorJoinsSplitsAndReordersClips` and `testCancelLeavesNoPieces` passed in this run); `xcrun swift test --filter VideoCopyTests` with `ATHER_TEST_OUT` → `Executed 4 tests, with 0 failures`, printing "the copy differs from the renderer's frame by at most 0 of 255" at 1.5 s and 2.0 s; `proof/a7-mac-range.png` opened. Build warnings: 30 before and after. Acceptance: A6, A7.
- S4 (rev 1, 2026-10-21): docs. README: a "Copy a clip" Features row; in the Video editor guide, "Copy a clip or a frame (to paste into Slack)"; the shortcut line (Shift+drag, Shift+I/O, Ctrl+C, Ctrl+Shift+C, Esc clears the range first); Platform differences (Save ▾ vs the Mac's Copy menu, H.264 on the Mac, its frame-rate snapping). `macos/README.md`: a Video editor bullet and its shortcut line. `RELEASE_NOTES.md`: "Coming in the next update" › "Copy a clip or a frame, then paste it into Slack". Acceptance: A8 ready for review.

- S5 (rev 1, 2026-10-21): draft PR #10. CI on the final code (d4a4971): https://github.com/AskTinNguyen/AtherScreenshot/actions/runs/39617583897 → `114 tests, 1770 checks, 11 failed`, the 11 being exactly S0's baseline list; `ok` for all four new tests and for `video_editor_plays_and_seeks` (so S2's failure of it was runner timing); MSVC `Built build\AtherScreenshot.exe` with no `warning C` / `error C` (run 39615745081); the range and Save-menu snapshots rendered again. No include order changed (the orchestrator's min/max note doesn't apply). Acceptance: A1–A3 re-confirmed.

## Reconciliations

<!-- rev <old> -> <new>: still valid <...>; redo <...>; dropped <...>. -->

## Owner-level calls

<!-- D7: calls the contract reserves for the owner, made by the worker. Each: the call, the options weighed, why. -->

- **Range look:** a translucent lime band from the thumbnails down through the caption and markup lanes, 2 px lime edges, white grips with a dark outline in the thumbnail row, and a lime pill with the label in the middle of the band (beside it when the band is too narrow). Weighed: a band over the thumbnails only (easy to miss next to the lanes), an outline only (reads like the trim, which is lime-outlined with lime handles). White grips keep them apart from the trim's handles.
- **Label:** the copied clip's length, the range divided by the edit's speed, so it matches the toast and the file; `6.2 s`, and `m:ss.d` from a minute. Windows says `Ctrl+C copies`, the Mac `⌘C copies`.
- **Shift+O:** like O, the end goes at the playhead's time, so the frame on screen is the first one left out. Weighed: including the frame on screen (common in other editors), but I and O already work the other way here, and the two should agree.
- **Grips over trim handles:** where a range edge sits on a trim handle, the press takes the range's grip (the newer selection); Esc clears the range and frees the handle. Weighed: the trim first, which would make a range that starts at the trim impossible to resize from that side.
- **Clip changes clear the range** (split, reorder, trim, add, undo of those), rather than moving it with its footage like captions: the range is a quick selection, and a range that silently jumps to other footage would copy the wrong part.
- **Dragging an edge shows that frame:** the range's first frame while its start moves, its last while its end moves, so it can be placed on an exact frame.
- **Toasts:** "Copying clip…" with "6.2 s long" and the percentage; "Clip copied · 6.2 s · 3.4 MB" with "Paste it in Slack or anywhere · click to show in Explorer" (Finder on the Mac); "Clip saved, but the clipboard was busy" with the file name and "click to show it in Explorer and drag it in instead"; "Frame copied" with `0:12:37 · frame 757 · 1280 × 720` and the picture (the Mac shows `0:12.6 · 1280 × 720`, as it has no frame readout); "This clip is outside the trim" when the selected clip has nothing inside it (rather than copying the frame instead).
- **Save ▾ order:** Save video, Save smaller, Save GIF | Copy clip, Copy frame | Save review video. Copy frame is greyed only before the first frame is shown.
- **Mac:** one "Copy" pull-down (doc.on.doc icon) left of Save smaller, rather than two buttons, so the toolbar still fits at the 980 px minimum width.
- **Hint lines:** "Shift+drag mark · Ctrl+C copy" after "S split" (Mac: "⇧-drag mark · ⌘C copy"); it fits at 1180 px (see `proof/a5-range.png`).
- **Editor closed while a clip saves:** it's still moved into the captures, noted in the gallery and put on the clipboard, as Save finishes without its editor.

## Engineering notes

- The new Windows tests compare with what the editor itself shows and renders on the same machine (`FrameOnScreen` at the range's ends, `FrameRenderer` for the frame, the export's own first frame through `g_exportTap`), never with `frame index / fps`, so the runner's one-frame offset (S0) neither fails nor passes them by itself.
- The copied clip's trim ends 0.1 ms before the range's end (both platforms), so 100-ns rounding in frame times can never add the next frame; the frame count is exact.
- A frame copy pressed right after a step waits for that frame's decode (Windows), so the picture is the frame the readout names; starting playback drops a copy still waiting.
- `CopyImageToClipboard` only added "PNG" for pictures with transparency; frames are opaque, so it gained a `withPng` option (other callers unchanged).
- The Mac tests use a pasteboard of their own (`NSPasteboard(name: "ather-test-<UUID>")`, released in `tearDown`) and check that the general pasteboard's `changeCount` doesn't move.

## Dev tools

- None installed. MinGW for the Windows type-check; the CI artifacts downloaded with the owner's permission (L-2) into `/private/tmp/claude-501/video-copy-ci/<run id>/`.
