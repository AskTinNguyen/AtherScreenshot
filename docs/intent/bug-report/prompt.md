# Bug Report: "Bug Start" / "Bug Stop" Make One Bug Bundle (Picture with What Was Said, Clip, Game Logs)

- Rev: 3
- Status: completed
- Area: Recording
- Owner: AskTinNguyen
- Issue: #8
- Skill: `none`
- Branch: `intent/bug-report`
- Started: 2026-10-21

## Goal

From the team's playtest wishlist (L-1, wish 3). While playtesting, with the whole screen being recorded, a tester wants to report a bug without leaving the game:
- **Text overlay from voice:** what the tester says is written onto the picture.
- **A clip by voice:** "bug start" → "bug stop" cuts a small video of that moment.
- **Logs:** the game's logs are collected from a configured path.
- **One bundle:** all of it becomes one set of bug metadata: the picture with the text overlay, the video (if there is one) and the game's logs.

The team drafted a fuller requirement for this but hasn't shared it yet (F-1). This rev is built from the wish; the draft is reconciled when it arrives.

What exists to build on (2026-10-21):
- **Windows speech:**
  - Windows speech recognition (SAPI) already turns speech into captions (`TranscribeWav` in `src/videoio.cpp`, in-process recognizer, dictation grammar).
  - A self-test already synthesizes speech with SAPI text-to-speech into a WAV and recognizes it.
  - Microphone capture exists in `src/audio.*` (WASAPI).
- **Recording state:** `RecordingId()` and `RecordingSeconds()` identify the running recording and its time, without pauses.
- **Markers:** AI-agent markers are already written as review notes next to a finished recording (`WriteMarkers` → `<video>.notes.json`, `src/agent.cpp`). The Windows video editor shows them on their frames.
- **Export:** `ExportMp4` cuts and encodes a range of a video with captions (`VideoEdit` with a trim and captions).
- **Mac speech:** the Mac uses `SFSpeechRecognizer` (`VideoEditor.swift`) and records with ScreenCaptureKit (`Recorder.swift`).

## Non-Goals

- **Sending bugs to a tracker** (Jira, Linear, GitHub) or to Slack by API. The bundle is files; "Copy last bug report" puts them on the clipboard.
- **Starting a recording by voice.** Without a running recording, a bug has no clip ("video if any").
- **Cloud speech recognition.** Everything is recognized on the computer.
- **Cutting the clip before the recording ends.** The recording file isn't finished until then (D4).
- **Changing what Record or Stop do** for anyone who doesn't use bug reports.

## Decisions

These are the orchestrator's proposed defaults (rev 1). Looks, wording, saved files and privacy are owner calls by the contract. The owner asked to build everything (L-1), so the worker builds them as written; the owner may change any of them by saying so.

- **D1 (rev 1): triggers.**
  - **Voice:** "bug start", then the tester describes the bug, then "bug stop". "Bug cancel" throws the bug away. Recognized by a command grammar (Windows: SAPI in-process recognizer on the default microphone; Mac: `SFSpeechRecognizer` with on-device recognition only).
  - **Hotkey:** "Bug report: start / stop" (`BugToggle`, default Ctrl+Alt+B on Windows and ⌃⌥B on the Mac, if free; otherwise the worker picks a free one) presses once to start and again to stop. Also palette commands "Bug report: start / stop", "Copy last bug report" and "Open bug reports folder".
  - A bug left open stops itself after 2 minutes.
- **D2 (rev 1): listening is opt-in.**
  - Settings › Bug reports › **Listen for "bug start" / "bug stop"** (`[Bugs] VoiceCommands=0` by default), because it keeps the microphone open.
  - While it listens, the tray icon tooltip (Mac: the menu-bar menu) says so.
  - The hotkey works with listening off.
  - If no recognizer is available (none installed, or no on-device recognition on the Mac), turning it on says so plainly, and the hotkey still works.
- **D3 (rev 1): what a bug records.**
  - At start:
    - The time.
    - A screenshot: the recorded area while a recording runs, otherwise the monitor with the foreground window.
    - The foreground app and window title.
  - Between start and stop:
    - The dictated words with their times (the description).
    - The microphone audio (D6).
  - At stop: the logs (D5).
  - A short "Bug started" / "Bug saved" toast confirms each step without stealing focus from the game.
- **D4 (rev 1): the clip.**
  - When the bug happens during a recording, its clip runs from **10 s before "bug start"** (`[Bugs] PreRollSeconds=10`, clamped to the recording's start) to "bug stop".
  - The dictated words are captions on the clip, at their times.
  - The recording isn't readable until it stops, so the clips of every bug in it are cut, with `ExportMp4`, when it stops. Until then `bug.md` says "Clip: when the recording stops".
  - Each bug is also written as an **Issue** review note with its range on the recording (`<video>.notes.json`, the way agent markers are), so the frame-review editor shows it.
  - Pauses are respected: times are recording time.
- **D5 (rev 1): game logs.**
  - **Setting:** Settings › Bug reports › **Game log paths** (`[Bugs] LogPaths`) takes one or more files, folders or wildcards, separated by `;`. Environment variables are expanded, e.g. `%LOCALAPPDATA%\MyGame\Saved\Logs\*.log`.
  - **Which files:** at "bug stop", each file named is copied. From a folder or wildcard, the files changed in the last 30 minutes are copied, or the newest one if none changed, at most 20 files.
  - **Reading:** files are read with shared read/write access, since the game keeps its log open.
  - **Size:** a file over `[Bugs] LogMaxMB=20` keeps only its last 20 MB, and the cut is noted in `bug.json`.
  - **Missing paths:** a path that doesn't exist is noted, not an error.
- **D6 (rev 1): the bundle.**
  - **Where:** a folder `<captures>\Bugs\<yyyy-MM-dd HH-mm-ss> <first words>\`, with the first words of the description, file-name safe, at most about 40 characters.
  - **Contents:**
    - `bug.png`: the screenshot with the overlay (D7).
    - `screenshot.png`: the clean screenshot.
    - `clip.mp4` (when there was a recording).
    - `voice.wav`: what the microphone heard between start and stop, so a bug described in a language the recognizer doesn't know isn't lost (F-2).
    - `logs\…`.
    - `bug.md` for people: title (first sentence), time, app and window, description, the recording and the bug's timecodes in it, the files.
    - `bug.json` for tools: a format version, an id, times, the description and words with times, app, window, the screenshot area, the recording path and range, and the files and logs with their sources, sizes and any truncation.
  - **Gallery:** `bug.png` and `clip.mp4` show in the gallery tagged `bug`.
  - **Copy last bug report:** puts `bug.png`, `clip.mp4` (once it exists), `bug.md` and the log files (at most 10) on the clipboard as files.
  - **Toast:** a click on "Bug saved" opens the folder.
- **D7 (rev 1): the overlay.**
  - **Bar:** the description is drawn onto the screenshot as a bar along the bottom, with white text on a dark translucent background, wrapped, and at most 6 lines with "…".
  - **Header:** a small header line shows `BUG · 2026-10-21 14:03:22 · <app>`.
  - **Size:** it's sized to the screenshot (like captions are sized to the video), readable when the picture is posted in Slack.
  - **No words:** with no words recognized, only the header is drawn.
- **D8 (rev 1): Windows first, then the Mac.**
  - Windows gates run on GitHub's Windows runner (`.github/workflows/check.yml`, on every push to `intent/**`). The run's URL and the self-test result line are the evidence for `gate: test.bat` and `build`.
  - MinGW (`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -DUNICODE -D_UNICODE -Dsmall=char`) is a quick local type-check, never a proof.
  - The Mac part is proven with `gate: mac` on this Mac.
- **D9 (rev 1): no waiting.** Further owner-level calls (wording, look, details the spec leaves open) are the worker's. It picks what fits the app's style and records each, with the options it considered, in `progress.md` under "Owner-level calls". Findings are still written for anything that changes the intent; none is blocking unless it's destructive, a release, or a security, privacy or credential matter.
- **D10 (rev 2): Vietnamese on the Mac, behind its own opt-in.** Source: L-2, F-2 (d).
  - Settings › Bug reports › **Transcribe Vietnamese with Apple's servers** (`[Bugs] CloudVietnamese=0`, off by default, Mac only). Its description says plainly that the narration audio is sent to Apple.
  - When it's on:
    - The commands are still recognized on the computer.
    - At "bug stop", only the narration between "bug start" and "bug stop" (`voice.wav`) goes to `SFSpeechRecognizer` with the `vi-VN` locale and `requiresOnDeviceRecognition = false`.
    - The result becomes the description and the captions, with the words' times.
  - When it's off, nothing is ever sent: recognition stays on-device only (D1).
  - If the server transcription fails or is unavailable, the bug keeps the on-device words (if any) and `voice.wav`, and `bug.json` notes the failure.
  - Windows has no equivalent: its offline dictation doesn't know Vietnamese, so Windows bugs keep `voice.wav` (F-2 (b)).
- **D11 (rev 3): on-device Vietnamese first.** Source: F-4, R-1. With "Transcribe Vietnamese with Apple's servers" on, `voice.wav` is first transcribed as `vi-VN` on the Mac when on-device recognition supports it. It goes to Apple's servers only when it doesn't. This only ever sends less than D10 allowed, so the orchestrator accepted it within the owner's opt-in (L-2). The setting's description says so.

## Acceptance

- **A1: the bug state machine (Windows).**
  - Start, stop, cancel, the 2-minute auto-stop, the hotkey toggle, and a start while one is open (ignored) all behave as D1–D3 say.
  - Words between start and stop become the description; the command words themselves don't.
  - Proof: `gate: test.bat`, with self-tests that feed timed events (commands, words, hotkey) into the bug logic without a microphone.
- **A2: voice commands (Windows).**
  - Speech synthesized with SAPI text-to-speech ("bug start … the door does not open … bug stop") goes through the same recognizer setup the app uses for the microphone (command grammar plus dictation), bound to a WAV instead.
  - It yields start, then stop, with the description words in between.
  - Proof: `gate: test.bat`. If the runner has no recognizer, the test must say so in the log, and this item then needs `owner` on a real PC (written as a finding).
- **A3: the bundle (Windows).** From a bug with a screenshot, words, a recording file and log paths in a temp folder, the bundle folder holds:
  - `bug.png` with the bar and text where D7 puts them (pixel checks), and `screenshot.png` unchanged;
  - `clip.mp4` of the right range (pre-roll, clamped, duration within one frame) with the captions;
  - the logs: changed-recently and newest-file rules, the tail cut with its note, and a file another handle holds open for writing still copied;
  - `voice.wav`;
  - `bug.md` and `bug.json` with the D6 fields.
  - The recording's notes sidecar gets an Issue note with the bug's range.
  - Proof: `gate: test.bat`.
- **A4: recording integration (Windows).**
  - Bugs marked during a recording get their clips when it stops.
  - A bug with no recording has no clip, and `bug.md` and `bug.json` say so.
  - Proof: `gate: test.bat`, driving the recording-finished path with a generated clip.
- **A5: settings, hotkey, palette (Windows).**
  - Settings › Bug reports has VoiceCommands, LogPaths, PreRollSeconds and LogMaxMB.
  - The hotkey is rebindable; the three palette commands run.
  - Proof: `gate: test.bat`, plus a `snapshot` of the Settings section.
- **A6: visual proof.** `bug.png` with an overlay of several lines, and a frame of `clip.mp4` with its caption.
  - Proof: `snapshot`, rendered on the runner, opened by the worker and named in `progress.md`.
- **A7: Mac.**
  - The same state machine, bundle, overlay, logs and clip-on-recording-stop.
  - Voice via on-device `SFSpeechRecognizer`, with the hotkey working without it.
  - (rev 2) With "Transcribe Vietnamese with Apple's servers" on, the description comes from a `vi-VN` server request on `voice.wav` only. With it off, no request ever leaves the Mac. Tested through a seam that records the request's locale and on-device flag, without a real network call.
  - Proof: `gate: mac`. Recognition from a `say`-made audio file is tested if the test runner gets speech permission; otherwise that part is `owner`, and so is a real Vietnamese transcription.
- **A8: docs.**
  - README gets a "Bug reports" guide section, Settings keys and the shortcut row, and so does `macos/README.md`.
  - `RELEASE_NOTES.md` gets a line under the next update.
  - Proof: `review`.
- **A9: a real playtest.** The owner or a tester records a game, says "bug start … bug stop" twice, and checks both bundles.
  - Proof: `owner`.

## Constraints

- **The contract's Never list:**
  - no owner recordings or screenshots in git; tests make their own media;
  - never touch the real support folder;
  - no releases, tags, `latest.json` or version changes;
  - no third-party dependencies;
  - don't stop the owner's running app.
- **Privacy:**
  - The microphone is used only while listening is on (D2).
  - Nothing leaves the computer: no cloud recognition, no uploads.
  - Logs are copied only from paths the user typed.
- **Never block the game.** Recognition, screenshots, log copying and clip cutting run off the UI thread. Toasts never take focus.
- **The recorder's output must not change** for recordings without bugs. Plain recordings stay byte-for-byte as they were; clips are separate exports.
- **Shared files:**
  - `README.md`, `RELEASE_NOTES.md`, `macos/README.md`, `src/main.cpp`, `src/settings.*`, `src/settingsui.cpp` and the Mac `App.swift`, `Settings.swift` and `SettingsWindow.swift` are also touched by other intents. Keep hunks small and self-contained.
  - Put the feature in new files: `src/bugreport.*`, and on the Mac `BugReport.swift`.

## Changelog

- completed (2026-10-22): merged into `main` and released in build 0.0.2.5 on the owner's word (L-3). The `owner` acceptance row (real-PC/Mac check) was not run before release; it stays for the owner.

- rev 3 (2026-10-21): from R-1. F-4 accepted as D11: on-device `vi-VN` first, servers only as the fallback. Runner baseline failures are fixed separately (`intent/runner-video-tests`).

- rev 2 (2026-10-21): from L-2. D10 adds an opt-in Mac setting that transcribes Vietnamese narration with Apple's servers (F-2 (d) accepted). A7 covers it. CI artifacts may be downloaded (L-2).

- rev 1 (2026-10-21): created from L-1 (issue #8), with the orchestrator's proposed defaults D1–D9. F-1 (the team's draft requirement) and F-2 (dictation language) are open, non-blocking.
