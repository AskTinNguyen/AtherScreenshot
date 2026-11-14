# QuickPrompts: Findings

Discoveries that may change the intent. The worker adds entries; the orchestrator resolves them.

## F-1 (2026-11-14, rev 2) | blocking: no | status: accepted

**Found:** a `WH_KEYBOARD_LL` hook (D3) runs on the UI thread. When that thread is busy (a video export, a big gallery
scan), every keystroke on the PC waits for it, and Windows silently removes a hook that is slow more than
`LowLevelHooksTimeout`. Raw Input (`RIDEV_INPUTSINK`) on a thread of its own gets the same key presses without sitting
in anyone's input path, and marks software-typed keys (no device), which D3 wanted to skip anyway.
**Proposed amendment:** D3 uses Raw Input on a dedicated listener thread instead of a keyboard hook. Mouse buttons come
the same way, for the click reset.
**Resolution:** accepted as an engineering call inside the intent (rev 3). Side effect, documented: text typed by
software (Unikey/EVKey output, on-screen keyboards, remote-control tools) never counts toward a trigger.

## F-2 (2026-11-14, rev 2) | blocking: no | status: accepted

**Found:** the Mac part (D10/P4) can only be proven with `gate: mac` on a Mac. This machine is Windows, and the staging
workflow's Mac job runs `CompressTests` only.
**Proposed amendment:** ship Windows now (as 0.0.2.6 and 0.0.2.7 did); the Mac port is its own follow-up on a Mac.
README › Platform differences says QuickPrompts is Windows only for now.
**Resolution:** accepted (rev 3). A11 stays open.

## F-3 (2026-11-14, rev 2) | blocking: no | status: open

**Found:** on this PC two video tests fail with or without this change: `video_editor_loop` and
`video_editor_copies_the_frame_on_screen`. Unmodified `main` (ad65a28), built in a separate worktree, fails both the same
way (`1 tests, 15 checks, 1 failed` and `1 tests, 29 checks, 1 failed`).
**Proposed amendment:** none for this intent; worth its own look.
