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

## F8 — rev 3: short command spans and independent HUD tests

The parser formerly required two command words and advanced by two words. One-word commands therefore need both a grammar change and an explicit end index, or the first description word would be lost. Parser and actual SAPI generated-speech RED/GREEN tests verify "bug" / "stop" and preserve the description. Longer aliases remain accepted.

A full release run while the installed app was running exposed a global FindWindow lookup in the HUD test. It could select the installed app's panel. The test now uses its own module panel and checks the current PID while retaining production painter/focus/style/destruction coverage.

Latest rev 3 full release result is `179 tests, 2940 checks, 7 failed`, exit 24; all seven are recurring clipboard integrations. Focused voice is `7 tests, 69 checks, 0 failed`, parser `1 test, 9 checks, 0 failed`; both also pass in the full run. The historical rev 2 full PASS does not establish a current full PASS. Native OpenClipboard error 5 has recurred; its cause remains unproven.

## F9 — rev 4: low-confidence stop, stale feedback and neutral controller data

The old voice filter rejects every low-confidence command, including stop for an open report. A regression demonstrates this; a narrow exception now accepts only a command-grammar stop while dictation/report capture is active. This is a plausible contributor to the reported symptom, not proof of the owner's actual recognition confidence. The command rule receives priority over dictation when supported. Accepted commands found in dictation also update the listener's dictation state. Actual SAPI generated speech passes, but the owner's voice/accent is not synthesized evidence.

Last-heard text was checked before current speech activity, so the HUD could keep showing "bug" instead of recognizing a new utterance. A RED/GREEN test verifies current speech takes precedence. The MIC LIVE/quiet percentage is explicit even when last-heard text exists. A controller-only HUD is available with voice off.

Primary-only XInput reading has no way to retain input available through another Windows XInput version when the primary reports connected-but-neutral. Native fallback compares real system-reader states and never replaces active primary input with an idle alternate. The neutral-primary regression passes after a recorded RED. Microsoft documents focus-related neutral XInput behavior and the system-provided legacy API: https://learn.microsoft.com/en-us/windows/win32/api/xinput/nf-xinput-xinputenable and https://learn.microsoft.com/en-us/windows/win32/xinput/xinput-versions . This motivates a background-reading mitigation; it does not prove that focus or Steam Input caused this specific machine's neutral state. A native read-only probe sees one connected pad but no buttons under either reader at probe time. No physical presses were injected or claimed. The new live indicator makes disconnected, connected-without-input, receiving controls, and idle-after-input distinguishable.

Final rev 4 source builds with only the two existing C4706 warnings. Its initial full run had a transient DIB/PNG clipboard comparison failure (`183 tests, 2982 checks, 1 failed`), followed immediately by an unchanged focused pass (`1 test, 29 checks`). A fresh complete run of that exact binary then passes **183 tests, 2984 checks, 0 failed**, exit 0, no skips. No assertions/source/host settings were changed between those runs. The comparison failure's cause remains unknown. Evidence: `rev4-final-release-full-tests.log`, `rev4-frame-copy-retry.log`, `rev4-final-no-build-full-tests.log`.

## F10 — rev 5: Done, recording-only HUD and recording gain

Changing fixtures to Done reproduces a parser failure and actual SAPI failure before grammar/parser support. The old recognizer transcribes Done as dictation without finishing the report. After registration, the same generated speech recognizes Done through the command grammar and completes both reports. This does not establish that the owner's live stop failure was caused by pronunciation, nor prove live Done robustness.

The HUD previously appeared whenever voice/controller monitoring was enabled. New RED/GREEN coverage gates the actual window on the recording lifecycle, preserving voice-off controller monitoring, click-through/focus/capture exclusion and pause behavior. BeginCapture opens the gate; stop/finalization and shared completion close it. Listening can remain enabled while the badge is hidden.

Microphone gain is app-local WASAPI processing only. The owner chose default unchanged: 0 dB returns existing samples untouched, while 0..20 dB is configurable for new recordings and bug voice.wav. System loopback receives 0 dB regardless of the setting. Boosted microphone samples use a symmetric soft knee before resampling. SAPI's separate input and Windows volume are untouched. Gain improves recorded level; it is not a promise of clearer transcription or repaired clipping/noise.

First rev 5 release builds successfully (only existing C4706 warnings), with `185 tests, 3001 checks, 1 failed`, exit 1: only `video_editor_copies_the_selected_clip` failed its clipboard file-list comparison. It immediately passes unchanged in isolation (`1 test, 11 checks`). All Done/HUD/gain checks pass. No clipboard assertion or source was modified; full no-build rerun is pending. Logs: `done-hud-gain-full-release.log`, `done-selected-clip-retry.log`.

The unchanged full rerun has `185 tests, 3001 checks, 1 failed`, exit 3: clip copy passes, while QuickPrompts text/caret integration fails. That test immediately passes unchanged in isolation (`1 test, 8 checks`). No skips or weakened assertions; all feature regressions pass. This does not establish the integration failures' cause or a current full PASS. Evidence: `done-hud-gain-final-full.log`, `done-quickprompts-retry.log`. Full-suite acceptance remains open even though the owner-authorized local test build is installed.

## F11 — rev 6: hidden Settings and the remaining dictation finish filter

Read-only window enumeration on the installed process found an AtherScreenshotSettings window with Visible=false. The existing-window code only restored minimized windows; a hidden window was never shown. Hidden launchers can also override the process's first ShowWindow with SW_HIDE. A hidden/reopen regression fails its visibility assertion before the fix and passes after explicit show/restore and a first-show visibility check. Both computer-use providers were unavailable (native pipe error 2), so this is native window metadata and automated in-app testing, not a claimed visual desktop walkthrough.

The rev 4 exception only accepted low-confidence stop from the command grammar. Since dictation is enabled after Bug, an isolated Done/Stop returned through dictation still failed the guard. The new policy regression reproduces this rejection. A narrow exception now accepts a finish spanning the entire utterance while a report is open. New starts and low-confidence commands inside a description remain guarded. This identifies a real rejection path, not the proven cause of every live failure.

The listener writes bounded local bug-voice-diagnostics.log metadata (input/language, engine rejection, confidence, grammar source, action, word count, acceptance and report-open state). It writes no audio, transcript, game/window name or description. After roughly 64 KB it starts over. Tests use their temporary support folder. The owner's real mic/accent remains unverified; do not substitute TTS for that check.

## F12 — rev 7: small Sherpa KWS did not pass the owner's real sample

The owner explicitly selected Sherpa and free models. Downloaded official English GigaSpeech 3.3M and Chinese/English phoneme 3M KWS archives. The English archive README and author's phoneme ModelScope card declare Apache License 2.0. Runtime 2.0.3 and Python dependencies were installed with pip --target outside the repo. No model or binary is distributed by this change.

The phoneme model detects all 12 intended synthetic commands with score 1.5 / threshold .15 and no detections in four negative synthetic files. English default detects 8/12, and score 1.5 / threshold .15 detects 11/12. These are small diagnostic fixtures, not general accuracy claims.

Two owner WAVs, an owner-confirmed 60-second VR P10 capture, and tested sensitivity/gain/fp32 variants yield no KWS detections. The confirmed capture received exactly 60 seconds with zero drop/status blocks, so no-delivered-PCM is excluded for this run. Signal peak was 94.38%; that does not establish intelligibility of every utterance. The live English process used 4.046875 CPU seconds / 60.2141 wall seconds, approximately 6.7% of one core. The owner's first live trial is excluded from accuracy scoring because the owner said they were focused elsewhere, and then repeated the requested sequence on the second trial and confirmed completion.

A narrow SAPI classifier on the same WAV returns several Bug/Done/Mom hypotheses; neither its forced vocabulary nor that result is treated as ground truth. The small Sherpa trial does not justify replacing the app backend. Keep the experiment repeatable in tools/sherpa_trial.py, document its limitations, and retain audio/results only in the external artifact directory. No app rebuild/install is needed because native code is unchanged.

## F13 — rev 8: false voice starts

The installed diagnostic log showed repeated live command results for Bug at confidence 0/1 while no intentional start was reported. Confidence alone is insufficient: SAPI's command grammar can produce normal/high confidence false positives from the live input. A live start must now be confirmed by two standalone Bug utterances within 2.5 seconds. The first is observable through the existing HUD message and does not create evidence. Hotkey starts remain one press; Done/Stop for an open report remains one standalone command. This is a narrow false-positive guard, not a claim that SAPI transcription is generally reliable.
