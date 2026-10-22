# Bug Report: Prompt Log

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

Wish 3 became this intent (issue #8), rev 1 with proposed defaults D1–D9. Wishes 1 and 2 are `video-copy` (#6, #7), the Art wish is `capture-tray` (#9).

## L-2 (2026-10-21 10:55) | class: decision | -> rev 2

> Q: To check the Windows screens, the workers need to download the test log and UI snapshot PNGs (a few MB per run) that this repo's own GitHub Actions "Check" runs produce. May they download those artifacts during this session?
> A: Yes, allow (Recommended)
>
> Q: Bug reports: Windows' offline speech recognition doesn't understand Vietnamese, so a description spoken in Vietnamese won't show up as text. The voice recording is always kept in the bug folder either way. Should the Mac offer Apple's cloud recognition for Vietnamese, behind its own opt-in?
> A: Yes, Mac opt-in

Rev 2: D10 (opt-in Mac cloud transcription for Vietnamese narration), F-2 (d) accepted. CI artifacts may be downloaded.

## R-1 (2026-10-21 16:20) | orchestrator review at the PR head (#12) | -> rev 3

- **A5, A6, A7 (snapshots):** pass.
  - The Settings › Bug reports section lists the four settings and three shortcuts, and its wording says the microphone stays open and nothing leaves the PC.
  - `bug.png` has the `BUG · date · app` header and the wrapped description on a dark bar.
- **A8 (docs):** passes.
- **A1–A5 gate lines:** the runner's 11–12 failures are the baseline video tests. They're caused by Windows' software H.264 encoder writing B-frames one frame late, and fixed in `intent/runner-video-tests`. The items stay met on the evidence that every bug-report test passes and the failures equal the baseline set. A Check run with that fix merged in should read `0 failed`.
- **F-4:** accepted as D11 (on-device `vi-VN` first), since it only reduces what is sent.
