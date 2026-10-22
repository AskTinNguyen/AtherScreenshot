# Capture Tray: Prompt Log

Append-only. Each user prompt verbatim, with credentials and machine paths redacted.

## L-1 (2026-10-21 10:40) | class: intent | -> rev 1

> Here's are the wishlists from my team about AtherScreenshot Features, backlog them as Issues as intents, then build them all
>
> "
>
> C hay có vài wish items, context là lúc playtest thì cho quay full luôn màn hình, khá Ok, ước có vài tính năng sau:
>
> 1. Chọn đoạn để split, nhấn Ctr+C, rồi tự copy vào clipboard và paste vào slack luôn, hiện tại manual split rồi export rồi save vào disk rồi kéo vào slack nó hơi tốn time.
> 2. Đứng tại 1 frame nào đó của timeline video, nhấn Ctrl+C là copy hình ở frame đó luôn
> 3. Tự thêm text overlay từ voice lên hình ảnh, cắt thành đoạn video nhỏ bằng 1 control voice (ví dụ: bug start -> bug stop), tự collect log từ đường dẫn config, gom thành 1 bộ meta data cho bug, gồm hình có text overlay, video (nếu có) và log của game. Đoạn này cuối tuần rồi C có draft requirement nhưng chưa kịp chạy
>
> <redacted> (Art)  [10:17 AM]
> Nhờ anh nhé, thank youuuu
> Chụp nhiều ảnh và quản lý trong dải thumbnail
>
> * Mỗi lần người dùng chụp ảnh, ứng dụng tự động thêm ảnh mới vào dải thumbnail ở cạnh dưới màn hình. Ảnh được giữ trong phiên làm việc để người dùng có thể chụp tiếp mà không phải lưu từng ảnh ngay lập tức. Dải thumbnail hiển thị các ảnh đã chụp; người dùng có thể chọn một ảnh để mở và chỉnh sửa, rồi chuyển sang ảnh khác hoặc tiếp tục chụp thêm. Khi hoàn tất, người dùng chọn ảnh muốn lưu và lưu chúng cùng lúc.
>
> Hình reference bên dưới`
> image.png
>
> "
>
> [image: a Snagit editor screenshot, the reference: a "Recent" thumbnail tray along the bottom of the editor window]

The Art team's wish became this intent (issue #9), rev 1 with proposed defaults D1–D8. Wishes 1 and 2 are `video-copy` (#6, #7), wish 3 is `bug-report` (#8).

## L-2 (2026-10-21 10:55) | class: decision | -> rev unchanged

> Q: To check the Windows screens, the workers need to download the test log and UI snapshot PNGs (a few MB per run) that this repo's own GitHub Actions "Check" runs produce. May they download those artifacts during this session?
> A: Yes, allow (Recommended)
>
> Q: Bug reports: Windows' offline speech recognition doesn't understand Vietnamese, so a description spoken in Vietnamese won't show up as text. The voice recording is always kept in the bug folder either way. Should the Mac offer Apple's cloud recognition for Vietnamese, behind its own opt-in?
> A: Yes, Mac opt-in

CI artifacts may be downloaded, so Windows snapshot proofs can be checked from the runner's artifact. (The Vietnamese answer applies to `bug-report`.)

## R-1 (2026-10-21 16:35) | orchestrator review at dda08ea (#11) | -> rev unchanged

- **A5 (snapshot):** passes after one fix. `editor-tray.png` shows the five-picture strip: the current picture outlined, one unticked, one saved, `Save selected (3)`, and "Picture 3 of 5" in the status bar, which earlier showed the hovered picture's tip.
  - The toolbar overlap at the top right comes from the runner's 1024×768 display. It's pixel-identical in the one-picture snapshot, so it isn't from this feature.
- **A7 (docs):** passes. README covers the Features row, the "Capture tray" guide part (with "set After capture to Open in editor") and the keys; `macos/README.md` and RELEASE_NOTES are updated too.
- **Runner baseline:** the 11 failures are fixed in #13 (Mp4Writer without B-frames, `110 tests, 1675 checks, 0 failed`). This branch should merge it and show `0 failed`.

## L-3 (2026-10-22) | class: decision | -> completed

> Do the following at the same time:
>
> 1. merge and release new version
> 2. create a short 30s marketing video with real recordings of the new features

Merged into `main` (with #13's fix) and released as build 0.0.2.5. The owner's real-device acceptance row wasn't run before the release; it stays the owner's.
