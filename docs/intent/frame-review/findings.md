# Frame Review: Findings

Discoveries that may change the intent. The worker adds entries; the orchestrator resolves them.

<!--
## F-<n> (<YYYY-MM-DD>, rev <r>) | blocking: yes | no | status: open | accepted | rejected

**Found:** <what, with evidence>
**Proposed amendment:** <the change to prompt.md>
**Resolution:** <decision and resulting rev, filled by the orchestrator>
-->

## F-1 (2026-10-10, rev 2) | blocking: no | status: open

**Found:** D2/D6 give "Save review video" the shortcut Ctrl+Alt+S, but Ctrl+Alt+S is already the app's global hotkey for Scrolling capture (`src/main.cpp`, `CmdScrolling`, default `Ctrl+Alt+S`; README's shortcut table). A global hotkey (RegisterHotKey) is taken before the editor window sees the key, so in the editor Ctrl+Alt+S would start a scrolling capture.
**What the worker did (D7):** the main window's WM_HOTKEY handler first asks `VideoEditorHotkey(mods, vk)`: when a video editor is the foreground window and the combination is exactly Ctrl+Alt+S, the editor runs Save review video and the hotkey goes no further. Everywhere else Ctrl+Alt+S is still scrolling capture. If scrolling capture is rebound, the editor still handles Ctrl+Alt+S itself.
**Proposed amendment:** note in D2 that Ctrl+Alt+S in the video editor takes precedence over the global scrolling-capture hotkey while the editor is in front; or pick another shortcut for the review video if the owner prefers scrolling capture to always win.
**Resolution:** <orchestrator>
