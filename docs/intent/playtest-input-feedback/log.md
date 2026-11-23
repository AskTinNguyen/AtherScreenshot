# Request log

## 2026-11-20 — intent, rev 1

> Tôi chưa thể làm vì không ngồi ở máy, nhưng tôi có playtest và sử dụng tính năng này trên nhiều máy khác đều cho kết quả the same. Hãy tự check kĩ lại, đảm bảo button của controller được highlight, bạn có thể giả lặp và tự test gửi Video tôi xem nhé. Nhân tiện đây, tôi phát âm "bug start" hoặc  "bug stop" nhưng không work, không có indicator nào thể hiện đã thu được giọng nói và show badge Bug start. Hãy tự check, đề xuất hướng cải thiện và implement nó nhé. Tôi away 9h

## 2026-11-20 — intent amendment, rev 2

> Fix luôn các ca lỗi đó đi

## 2026-11-20 — clarification, rev 2

Asked whether normal Notepad/Explorer copy and paste worked on this machine. Owner answered:

> Có, copy/paste bình thường

## 2026-11-20 — installation and draft PR authorization

> Cài vào cho máy của tôi luôn đi

> Và mở draft PR vào main các thay đổi trên, dùng simple English để mô tả nhé

The explicit installation request authorizes gracefully stopping the old running copy, replacing the installed executable, and restarting it. Existing settings/data are preserved. The user also authorizes pushing the feature branch and opening a draft PR into main; it does not authorize merging or publishing a release.

## 2026-11-21 — intent amendment, rev 3

> Tôi muốn sửa lệnh Bug start và Bug stop, đổi thành Bug và stop thôi cho ngắn gọn

## 2026-11-23 — intent amendment, rev 4

> Tôi nói Bug thì được kích hoạt, nhưng nói stop thì không save bug, có vẻ không ghi nhận được. Cần indicator micro overlay để biết được tín hiệu đang được nhận.
> Controller input cũng không được highlight, cũng cần indicator trên màn hình biết được controller đang được connected và nhận input, scale controller image overlay trên video lên 50% nữa

Owner clarified: Xbox controller, USB and sometimes Bluetooth; support both. Steam Input is enabled.

## 2026-11-23 — intent amendment, rev 5

Owner reports voice still only hears "bug" and asks for an easier short command. Proposed keeping "Bug" to start and using the one-syllable "Done" to save, with no promise that a word change alone solves the live listener problem. Owner approved:

> Ok nhé, update dùm tôi

Existing authorization to update the local installation and draft PR remains in effect.

Follow-up:

> Cái badge info trên màn hình ghi nhận tín hiệu micro và controller connected, nên chỉ hiện khi recording
> Bạn có thể ghi nhận mic với volume lớn hơn không? để ghi nhận mô tả cho rõ

Asked whether to default to +6 dB, +10 dB, or preserve the level and expose Settings. Owner chose:

> Giữ mặc định, cho chỉnh trong Settings
