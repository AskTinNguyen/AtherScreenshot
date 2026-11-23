# Progress

- Working under rev: 5
- Worker: root; controller subtask: controller
- Current step: rev 5 release built, feature checks pass and exact binary installed/running; full gate remains open on an intermittent QuickPrompts integration
- Next step: update draft PR #25 and owner live voice/gain review; leave unmerged
- PR: https://github.com/AskTinNguyen/AtherScreenshot/pull/25 (draft, base main)

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | Full release test.bat: 178 tests, 2942 checks, 0 failed; active second slot, drift/gradual analog, ownership changes and short taps pass |
| A2 | met | Same full run verifies decoded MP4 pressed/released states for all 20 actions and pause/resume; zero stale paused highlights |
| A3 | met | Same full run passes all voice status/panel tests; root opened bugstarted-panel.png |
| A4 | met | Same full run passes actual SAPI synthesized speech, shared confidence policy and ended-stream reconnect regressions |
| A5 | met | Release build printed Built build\\AtherScreenshot.exe; controller-simulated-input.mp4 uses production recorder/encoder; voice-status-demo.mp4 uses production HUD painter. Simulations explicitly labelled. Root inspected decoded A/released images and BUG STARTED panel. Read-only review by review agent, 2026-11-20, final source revision including DiscardHistory/pause tests: no material findings |
| A6 | met | README guide/features and next release notes updated; findings document limits; external HUONG-DAN.md explains demos, separate build, input/retry and physical/acoustic limitations |
| A7 | open | Historical rev 4 full pass; latest rev 5 exact-binary full run: 185 tests, 3001 checks, 1 failed (QuickPrompts text integration), exit 3; unchanged focused retry passes |
| A8 | met | Same full run passes short-command parser and actual SAPI generated-speech reports |
| A9 | met | Observed RED for low-confidence explicit stop; GREEN in final full release run; start/dictation guard assertions preserved |
| A10 | met | Observed RED for stale speech feedback/input labels; final full release passes labels, focus/capture styles and controller-only HUD lifecycle; root inspected synthetic snapshots |
| A11 | met | Observed RED for background-neutral reader and 330 px layout; final full release passes all 10 gamepad tests, decoded 20-action MP4 and pause/resume |
| A12 | open | Done parser and actual SAPI RED observed, focused debug GREEN: parser 1 test/9 checks; voice 11 tests/95 checks; release gate pending |
| A13 | open | Recording-only badge RED observed; focused debug lifecycle GREEN; recorder starts it only at capture and hides on stop/completion |
| A14 | open | Mic boost RED observed; focused debug 1 test/8 checks GREEN; default 0 dB preserved, range/default assertions added to Settings integration |

## Evidence folder

`D:\App\AtherScreenshot-artifacts\playtest-input-feedback` (synthetic media only, not committed).

## Step 1 — baseline and RED regressions

Main a686c12 full baseline: `164 tests, 2787 checks, 7 failed`; clipboard failures preceded this implementation. Controller RED logs reproduce idle-first-slot selection and insufficient tap visibility; subsequent RED checks caught gradual analog ownership and stale highlights across ownership changes. Voice policy and HUD assertions were observed failing before implementation. A pause regression reproduced four stale button/trigger samples before the resume history fix.

## Step 2 — implementation and focused validation

Controller: poll connected controllers at about 250 Hz, follow deliberate cumulative activity, ignore drift, clear history on ownership switch/resume, retain currently held controls, hold button/trigger peaks at least 120 ms. Production recorder input/source injection enables deterministic encoded MP4 checks without a physical driver or the owner's screen.

Voice: publish actual recognition input when available, signal/rejected/accepted phrases, persistent bug/saving state, reconnect stream failures with bounded backoff, manual retry in Settings/palette, and one confidence policy for file/live commands. HUD is non-activating, click-through and capture-excluded. No dependency, version or release changes.

Final release focused voice run: `7 tests, 66 checks, 0 failed` (`final-voice-tests.log`). The actual SAPI test recognized two complete reports from synthesized speech. Final release focused controller run: `9 tests, 90 checks, 0 failed` (`final-controller-tests.log`), matching debug normal/demo runs. Every gamepad case also passes inside the full run, including all 20 released states and pause/resume (zero stale highlights, six frames of held controls).

## Step 3 — final release/full gate

`test.bat` built the release executable successfully, with only the two pre-existing C4706 warnings in installer.cpp/videoeditor.cpp. Result: `176 tests, 2916 checks, 8 failed`, exit 27. Seven cases are the baseline clipboard failures; the additional `video_editor_loop` also fails a targeted retry on the unchanged main baseline executable (`1 tests, 15 checks, 1 failed`). A foreground-dependent quickprompt test reported a skip. Logs retain exact results; the full-suite acceptance gate is **not met** and no merge/publish was performed.

Independent final read-only review covered controller selection/latch, voice lifecycle/status and decoded release/pause deltas; no material findings remain in this feature's code. The owner must still judge real device/accent behavior and visual appearance. No physical-controller or live-microphone claim is made.

## Review build

Release copied to external `test-build\AtherScreenshot.exe` with portable marker and `Launch-Test.cmd`, which sets a separate `test-profile` via ATHER_SUPPORT_DIR. It was not launched interactively; the owner's running copy and real support folder were left unchanged. Close the existing tray instance yourself before starting this test copy. Feature branch remains local, unmerged.

## Step 4 — rev 2: remaining test failures

The owner authorized fixing all eight cases. Existing assertions were preserved. A new real sequence-player regression reproduced an early finished clock (0.489065 instead of 0.500000); after the fix, that regression and the existing loop test pass. A new native owner regression failed three assertions before the per-thread message-only owner was implemented, then passed all seven checks. Clipboard clear failures now stop image/text/file publication.

The unrelated standalone Win32 clipboard probe still fails at OpenClipboard with error 5, even with a valid owner, and the QuickPrompts integration retry still fails. No host security settings, clipboard services or running apps were modified. This is an unresolved prerequisite for the real clipboard integration checks, not a reason to skip/replace them. A text clarification about whether ordinary Notepad/Explorer copy/paste works was requested while release validation continued.

Final rev 2 `test.bat` release run: `178 tests, 2928 checks, 7 failed`, exit 24, in `rev2-full-tests.log`. The loop test, both new regressions, all voice tests and all decoded controller tests pass. The remaining failures are exactly the seven clipboard cases listed in F4; a foreground-dependent QuickPrompts test still reports a skip. Acceptance A7 and the full-suite gate remain open. No assertion was skipped, weakened or replaced with a simulated clipboard.

Root self-review of the final rev 2 diff, 2026-11-20: checked natural versus failed playback end states, resetting the end state on seek, per-thread owner lifetime, and allocation ownership when EmptyClipboard fails. Release builds retain only the two pre-existing C4706 warnings. Updated portable artifact SHA256: `16DA460608E95F3246E4D28507E171DE016D9ABE0C6742EA7BE257340CB1B399`. External `Run-SelfTests.cmd` allows the same executable/assertions to be run directly from an ordinary desktop session and records `selftest-interactive.log`.

## Step 5 — access restored; full gate PASS

After the owner confirmed ordinary desktop copy/paste worked, the same standalone clipboard probe succeeded (both NULL and valid owner; error 0), followed by the unchanged QuickPrompts integration (`1 tests, 8 checks, 0 failed`). No source, permissions, ACL, services or running owner apps were changed to restore access. The exact cause of the earlier denial remains unproven.

Ran fresh complete `test.bat` against production source commit `404b025`: release build printed `Built build\AtherScreenshot.exe`; command exit **0**; `178 tests, 2942 checks, 0 failed`, `Result: PASS`. Every original failing clipboard case and the loop test passes. The previously foreground-skipped QuickPrompts test also executes successfully; this run reports **no skips**. Log: `D:\App\AtherScreenshot-artifacts\playtest-input-feedback\rev2-access-restored-full-tests.log`. Earlier failed runs remain as historical evidence, superseded by this result for the current gate.

Copied that freshly built executable to external `test-build\AtherScreenshot.exe`; both SHA256 values match `E512174C077675707AF31F2486382159E19371BF29890A78025E47CAC82435A7`. Updated the Vietnamese guide. All A1–A7 are met; intent stays active until the owner merges, as required by the project contract. No merge, release, version change, real-support-folder edit, or replacement of the owner's running app was performed. Physical controller and live accent/microphone validation limits remain as stated in F5.

## Step 6 — owner-authorized local installation and draft PR

The owner explicitly requested installation and a draft PR into main with a simple English description. Backed up the old installed executable outside the repo, sent the existing app's trusted `quit` command, observed its exit without forced termination, copied the tested release binary into the existing per-user install location, and started that installed path again. The replacement SHA256 matches the tested build (`E512174C077675707AF31F2486382159E19371BF29890A78025E47CAC82435A7`); the new installed process was observed running. No settings/data migration or manual real-support-folder edit was performed. This supersedes the earlier no-replacement boundary only for the user's expressly authorized local installation. No version bump, public release or merge.

Fetched origin/main before preparing the PR; no new upstream commits beyond this branch's baseline were present. Source and tests remain unchanged since the complete passing release run; this step only adds installation/PR records.

Direct push to origin was denied: the local GitHub account `canhnguyenngoc` has READ access to the public upstream repo. Created its fork and pushed this exact branch to `playtest-fork/intent/playtest-input-feedback`. The GitHub connector was unavailable, so GitHub CLI created draft PR #25 into `AskTinNguyen/AtherScreenshot:main`, titled **Fix controller highlights and bug voice feedback**, with the requested simple English description. No merge was performed.

## Step 7 — rev 3 short Windows voice commands

The owner requested "bug" and "stop". Added a real parser regression that failed on the old behavior (three assertions), and changed the actual SAPI generated speech fixture to the short commands, which initially produced no recognized commands and failed. Production now registers the short grammar, parses one-word command spans without losing the following description word, and retains longer phrases plus "bug cancel" for compatibility. Updated Windows Settings/template comments, tray tooltip, toast, voice HUD, README and next release notes; macOS behavior is unchanged.

Debug focused results: parser `1 tests, 9 checks, 0 failed`; SAPI `1 tests, 13 checks, 0 failed`. SAPI heard two consecutive `bug → description → stop` reports, with normal command confidence. RED/GREEN logs are external `short-commands-red.log`, `short-speech-red.log`, `short-commands-green.log`, `short-speech-green.log`. Live microphone/accent validation limits remain; no new service or dependency.

The first full run exposed a test-only HUD lookup collision with the installed app: global FindWindow could find another process's panel. Scoped the regression to the current module's panel, asserted its owning PID, and retained focus/style/capture-exclusion/destruction assertions. Focused release results with the installed app still running: `7 tests, 69 checks, 0 failed` (`short-voice-final-focused.log`); parser `1 tests, 9 checks, 0 failed` (`short-parser-final-focused.log`).

Final fresh `test.bat` built release successfully, then exited **24**: `179 tests, 2940 checks, 7 failed`, `Result: FAIL`, with a foreground-dependent QuickPrompts skip. Log: `short-commands-final-full-tests.log`. The seven failures are the recurring clipboard integrations; all short-command/SAPI, voice HUD, controller/decoded-video, and playback-loop checks pass. No clipboard assertions were skipped or weakened. The same standalone native clipboard probe again reports OpenClipboard error 5; the cause remains unproven and no host ACL/service/security change was made. A1–A6 evidence above refers to historical rev 2 unless supplemented here; A7/A8 full-suite acceptance stays open.

Updated the installed executable under the existing authorization: backed up the prior exe as `installed-backup/AtherScreenshot-before-short-commands.exe`, gracefully quit the old app, copied the fresh release and restarted the installed path. Observed PID 29744 at `C:\Users\Admin\AppData\Local\Programs\AtherScreenshot\AtherScreenshot.exe`. Built, installed and external portable exe SHA256 values all match `CF557C0E0C8CF3DC1A912896D41638197B251041AAC7B38157C59F0058EF3B06`. No manual settings/data changes; no force termination.

## Step 8 — rev 4: live stop and input feedback

Owner reports that "bug" activates but "stop" does not save and controller buttons remain unlit; uses Xbox over USB/Bluetooth with Steam Input enabled. The installed hash matched the rev 3 build, so this is not attributed to an old installation.

Observed RED: explicit low-confidence stop while open is rejected; last-heard text hides new recognizing-speech feedback (`rev4-voice-red.log`); normal-frame pad width is not 330 px (`rev4-scale-red.log`); a connected-neutral primary hides actual alternate-reader input (`rev4-background-red.log`); live/idle/disconnected mic/controller labels are absent (`rev4-indicators-red.log`).

Implemented a narrow confidence exception for an explicit command-grammar stop while open, command-rule priority over dictation, and dictation synchronization for accepted embedded commands. Start/cancel/dictation confidence guards remain. The input HUD shows explicit MIC LIVE/quiet percentage, current speech, and controller connection/slot, controls and last-input age. Show game controller enables monitoring before recording too; its poller cannot consume recorder taps. The recorder and HUD share native reader selection logic, comparing Windows' system legacy XInput when primary data is neutral/unavailable. No driver or dependency install. Size is 330 DPI-scaled pixels rather than 220, with existing small-frame caps.

Focused debug: voice `9 tests, 82 checks, 0 failed`; gamepad `10 tests, 100 checks, 0 failed`. Logs `rev4-voice-green.log`, `rev4-gamepad-green.log`. The actual SAPI synthesized file recognizes two complete reports. All 20 simulated actions show both lit and released states in decoded MP4; pause/resume has zero stale highlights. Root visually inspected `input-hud-live.png`, `input-hud-disconnected.png`, and `rev4-controller/controller-a-decoded.png`. New synthetic controller video is in `rev4-controller/controller-simulated-input.mp4`.

Read-only native probe: Xbox/XInput slot 0 connected under both Windows readers; neither returned buttons at probe time, all other slots disconnected (`rev4-native-xinput-probe.log`). This does not reproduce the owner's actual presses, prove focus suppression on this machine, or establish that the legacy mitigation bypasses the owner's Steam Input configuration. Physical USB/Bluetooth, game focus and accent remain manual validation limits. Full release suite is running; do not claim current full PASS yet.

First complete rev 4 release run passed: `183 tests, 2984 checks, 0 failed`, exit **0**, no skips (`rev4-release-full-tests.log`). All seven prior clipboard integrations pass again, without changes to clipboard source, host permissions or services in rev 4. The previous denial's cause remains unknown. Self-review then removed a harmless new int-to-WORD warning in the control-name table and explicitly stops the controller monitor before quit-time report/save callbacks; final release/full validation is running on that exact source. It retains the same confidence and input behavior.

The final-source release build succeeds with only the two existing C4706 warnings. Its first full run returned exit **2**, `183 tests, 2982 checks, 1 failed`: only `video_editor_copies_the_frame_on_screen` failed its DIB/PNG clipboard comparison. All feature tests passed. Immediate focused retry of that unchanged binary passes `1 test, 29 checks, 0 failed` (`rev4-frame-copy-retry.log`). No assertions or source were changed; a full no-build rerun is in progress. Do not attribute this transient comparison failure to a known clipboard holder or access policy without evidence.

Final exact-binary validation supersedes the transient failed run: `test.bat --nobuild` exits **0**, `183 tests, 2984 checks, 0 failed`, `Result: PASS`, **no skips** (`rev4-final-no-build-full-tests.log`). It runs the freshly built final release, not an older binary. All clipboard assertions pass unchanged. A1–A11 are met by this final run plus the labelled synthetic visual review; physical USB/Bluetooth/Steam Input and live accent remain unverified.

Under the owner's existing install authorization, backed up the installed rev 3 exe as `installed-backup/AtherScreenshot-before-live-input-indicators.exe`, gracefully quit PID 29744 without force termination, copied the final release to the installed and portable paths, and restarted the installed app. Observed PID **40264** at `C:\Users\Admin\AppData\Local\Programs\AtherScreenshot\AtherScreenshot.exe`. All three SHA256 values match **2E308F45EB388A38E9B089CB184C1D779FDD0AE4ABADE8B7DA3ACCEFE3FB1773**. Settings/data were preserved. No version, dependency, driver, host ACL/service, release or merge changes.

## Step 9 — rev 5: Done, recording-only badge and configurable mic gain

Owner approved Bug / Done and requested that the microphone/controller badge appear only during recording. For louder recorded descriptions, owner explicitly chose unchanged defaults with a Settings control.

RED: parser fails three assertions for Done; SAPI hears Done only as dictation and does not finish reports (`done-parser-red.log`, `done-speech-red.log`). After adding grammar/parser support, actual SAPI recognizes two Bug/description/Done reports with normal confidence. Focused debug voice: **11 tests, 95 checks, 0 failed**; parser **1 test, 9 checks, 0 failed**. Old stop aliases remain; all visible Windows instructions teach Done.

RED recording-only badge fails four idle/stop assertions (`recording-hud-red.log`); the same test now passes. A UI-thread recording gate opens at BeginCapture and closes at StopRecording or the shared completion callback, covering normal/automatic stop, failure, discard and cancelled countdown. Pause remains part of the recording session. Listening can stay on while its badge is hidden. Existing capture exclusion, focus and controller-only panel tests remain.

RED mic gain fails boost/saturation expectations (`mic-gain-red.log`); GREEN is **1 test, 8 checks, 0 failed** (`mic-gain-green.log`). Recording Settings expose MicrophoneGainDb 0..20, default **0**, with persistence/clamp checks. The value is snapshotted into regular/MCP recording options and bug VoiceTape; shared WASAPI capture applies it only to microphone samples before resampling. A soft knee limits boosted peaks. System loopback, default 0 dB samples, Windows input volume and SAPI recognition are unchanged. New recordings/reports use the changed value. Release/full validation is running; do not claim the previous rev 4 pass as a rev 5 pass.

First complete rev 5 release: `185 tests, 3001 checks, 1 failed`, exit **1**, no skip. Only `video_editor_copies_the_selected_clip` fails its clipboard file-list comparison. Immediate unchanged focused retry: `1 test, 11 checks, 0 failed`. All new functionality passes. No source, clipboard assertion or host policy was changed to obtain the focused pass; exact-binary full no-build rerun is in progress. Earlier transient clipboard failure causes remain unproven.

Final exact-binary full rerun (`done-hud-gain-final-full.log`): **185 tests, 3001 checks, 1 failed**, exit **3**, no skip. The clip-copy comparison now passes; only `quickprompts_paste_into_text_boxes` fails three text/caret assertions. Its immediate unchanged focused retry passes **1 test, 8 checks, 0 failed** (`done-quickprompts-retry.log`). All Done parser/SAPI, recording HUD, gain, controller/video, and playback-loop checks pass in both full runs. No full PASS is claimed for rev 5; A7 and full-gate acceptance stay open. No test assertion was skipped or weakened and no host ACL/service/volume was changed. These transient integration failures' cause remains unproven.

Installed the exact release under existing authorization after backing up rev 4 as `installed-backup/AtherScreenshot-before-done-and-recording-gain.exe`. Gracefully quit old PID 40264, replaced installed/portable exe, restarted and observed PID **46556** at `C:\Users\Admin\AppData\Local\Programs\AtherScreenshot\AtherScreenshot.exe`. Build, installed and portable SHA256 match **52DEC8DBE9C4A43E2D9C94A532A1CB066F83C0C6CEE5805FB4BDF21CC59452A5**. Settings/data preserved; default microphone gain remains 0 dB. No force termination, merge, release, version or dependency change.

Read-only post-install checks: FindWindow for AtherBugVoicePanel returns 0 while the installed app is idle; its real settings file contains MicrophoneGainDb=0. No manual support-folder edits were performed.
