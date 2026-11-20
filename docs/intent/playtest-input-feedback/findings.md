# Findings

## F1 — controller ownership

The poller previously selected the first connected XInput slot and never checked whether another connected slot received input. A simulated idle slot followed by an active slot can establish the defect. It does not establish the exact device setup on the owner's other PCs.

## F2 — recognition lifecycle and observability

The SAPI listener ignores `Next()` returning false after `SPEI_END_SR_STREAM`, leaving listening state true after the stream ends. Its interests omit audio-level, false-recognition and phrase-start events. Low-confidence microphone commands are silently discarded, whereas file tests bypass that policy. These are directly inspectable code defects; quiet narration alone cannot establish what the recognizer heard, because SAPI and narration capture choose their microphones separately.

## F3 — short taps, analog activity and recording pauses

Regression tests reproduced short taps disappearing before a readable duration, slow analog pulls failing per-sample activity thresholds, and previous controller highlights leaking across ownership changes. The fixed poller follows deliberate cumulative activity and clears historical state on a switch. Buttons and trigger peaks have a documented 120 ms visibility hold.

A decoded-video regression then reproduced four stale A/trigger samples after a tap entirely inside a short recording pause. Resume now discards history while retaining currently held controls. The regression passes with zero stale samples and six frames of held input.

## F4 — full-suite environment limitation

Before implementation, baseline a686c12 produced `164 tests, 2787 checks, 7 failed`. These failures concern clipboard-dependent app/video tests. A direct native diagnostic found `OpenClipboard(NULL)` denied with Win32 error 5; it did not establish that the desktop was locked. Do not weaken those tests or alter unrelated clipboard/security code. Functional filtered passes cannot satisfy the contract's full-suite acceptance gate.

Final release full suite produced `176 tests, 2916 checks, 8 failed`: the same seven clipboard cases plus `video_editor_loop`. A targeted loop retry failed on both the new build and the unchanged main checkout baseline executable: each `1 tests, 15 checks, 1 failed`, with playback remaining at zero. This establishes that the additional failure also reproduces without this feature, but does not diagnose its environmental/player cause. Logs: `final-full-tests.log`, `final-loop-recheck.log`, `baseline-loop-recheck.log` in the external artifact folder.

## F5 — physical/acoustic validation boundary

The owner explicitly requested simulation while away. The controller demo uses injected native XInput responses and a synthetic backdrop through the real poller, renderer, recorder, encoder and decoder. The voice demo uses synthetic status transitions through the production HUD painter; a separate actual SAPI synthesized-speech test covers accepted commands. Neither establishes physical controller/Steam Input compatibility or recognition of the owner's microphone/accent. No new dependency or voice service was introduced.

## F6 — rev 2: playback end clock

The source player advances/stops within 20 ms of a clip out-point, while the editor checks its loop boundary within 10 ms. The last sequence clock still exposed the stopped engine's slightly early position, so the editor could stop instead of looping. A new native real-player test with source trim [0.5, 1.0] reproduced `finished sequence clock=0.489065, duration=0.500000` before the fix. A successfully finished sequence now reports its exact timeline duration; seeks clear the end state, and failed engines retain their error position. Both this regression (`1 tests, 5 checks, 0 failed`) and `video_editor_loop` (`1 tests, 15 checks, 0 failed`) pass in the focused debug run after the fix.

## F7 — rev 2: clipboard ownership and remaining access denial

`OpenClipboard(NULL)` followed by `EmptyClipboard` leaves a NULL owner and can make `SetClipboardData` fail, as documented by Microsoft: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-openclipboard . The app's no-window copy paths and QuickPrompts now resolve a persistent per-thread message-only owner; image/text/file writes also check `EmptyClipboard` before handing memory to Windows. The native owner regression failed three checks with the former NULL pass-through and now passes all seven checks.

This does not explain or remove the host's earlier failure at **OpenClipboard itself**: an independent C#/Win32 probe with a valid HWND still receives error 5. The same happens in a small native executable, after reopening the existing station, and on a temporary private station/desktop. The diagnostic restores/closes its own handles; no host ACL, policy or service was changed. The session reports Active, the input/thread desktops are Default, AppContainer is false, and queried job UI restrictions are zero. The cause of the access denial is still unestablished; do not label it a locked desktop, policy denial or application lock without further proof.

Correction to earlier commentary: `GetOpenClipboardWindow()==NULL` does **not** establish that nobody has the clipboard open; an application can open it with a NULL HWND. Microsoft documents this explicitly: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-getopenclipboardwindow . The external `clipboard-diagnostic.log` retains the reproducible API results. Integration assertions remain unchanged and the full-suite gate must stay open while they fail.

### Access restored after owner clarification

The owner confirmed ordinary Notepad/Explorer copy/paste works. Before any additional source or host change, rerunning the same standalone probe succeeded for both NULL and valid HWND (`OpenClipboard=True, error=0`), and the unchanged release executable passed `quickprompts_clipboard_round_trip_keeps_formats`: `1 tests, 8 checks, 0 failed`. Evidence: `clipboard-after-owner-confirmation.log`, `clipboard-after-owner-test.log`. The host access state demonstrably changed; the cause of the earlier denial is not proven. Do not attribute recovery to a new code fix, a policy change, unlocking the screen or a service restart.

Fresh complete release `test.bat` then exited 0: `178 tests, 2942 checks, 0 failed`, with no skips. All seven clipboard cases and the loop case pass on production source `404b025`. Evidence: `rev2-access-restored-full-tests.log`. This resolves the acceptance blocker; it does not establish the earlier access denial's cause.
