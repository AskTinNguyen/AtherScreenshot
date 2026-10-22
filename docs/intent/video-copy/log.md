# Video Copy: Prompt Log

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

Wishes 1 and 2 became this intent (issues #6 and #7), rev 1 with proposed defaults D1–D7. Wish 3 is `bug-report` (#8), the Art wish is `capture-tray` (#9).

## L-2 (2026-10-21 10:55) | class: decision | -> rev unchanged

> Q: To check the Windows screens, the workers need to download the test log and UI snapshot PNGs (a few MB per run) that this repo's own GitHub Actions "Check" runs produce. May they download those artifacts during this session?
> A: Yes, allow (Recommended)
>
> Q: Bug reports: Windows' offline speech recognition doesn't understand Vietnamese, so a description spoken in Vietnamese won't show up as text. The voice recording is always kept in the bug folder either way. Should the Mac offer Apple's cloud recognition for Vietnamese, behind its own opt-in?
> A: Yes, Mac opt-in

CI artifacts may be downloaded, so Windows snapshot proofs can be checked from the runner's artifact. (The Vietnamese answer applies to `bug-report`.)

## R-1 (2026-10-21 16:05) | orchestrator review at the PR head (#10) | -> rev unchanged

- **A5, A7 (snapshots):** `proof/a5-range.png` passes. It shows the lime band with grips and the label `6.2 s · Ctrl+C copies`, and the hint line names Shift+drag and Ctrl+C. The readout there (`0:05:01 · frame 300`) is off by one frame; that's the runner's software-encoder offset, fixed separately in `intent/runner-video-tests`.
- **A8 (docs):** passes. README covers the Features row, the guide section, the shortcut line and Platform differences; RELEASE_NOTES has the "Coming in the next update" entry.
- **F-1:** resolved. The runner's 11 baseline failures came from Windows' software H.264 encoder writing B-frames that the MP4 stores one frame late. `intent/runner-video-tests` (Mp4Writer without B-frames) brings the runner to `0 failed`. A4 is met once a Check run of this branch with that fix merged in reports `0 failed`.

## L-3 (2026-10-22) | class: decision | -> completed

> Do the following at the same time:
>
> 1. merge and release new version
> 2. create a short 30s marketing video with real recordings of the new features

Merged into `main` (with #13's fix) and released as build 0.0.2.5. The owner's real-device acceptance row wasn't run before the release; it stays the owner's.
