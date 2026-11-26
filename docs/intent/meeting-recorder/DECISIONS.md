# Meeting Recorder: Decisions Made on Your Behalf

You asked (2026-11-26) for a meeting audio recorder, then for Descript-style audio editing for clean-up, agents and
outputs, and to build, validate, merge and release it without asking. These are the calls I made for you. Each says what I
chose, why, and how to change it. The spec is [prompt.md](prompt.md); your two prompts are in [log.md](log.md).

## Scope

**D1. Mac only for now. Windows is a follow-up.**
- **Why:** this Mac can't build or run the Windows app (MSVC), and the last few releases have shipped one platform at a
  time (0.0.2.14 was Mac only).
- **What a Windows version needs:** WASAPI loopback and microphone, Windows speech recognition (already used for
  captions) and a Win32 window. That's a separate intent.
- **Change it:** start `docs/intent/meeting-recorder-windows`.

**D2. Its own Meetings window, not the capture gallery.**
- **Why:** a meeting is a folder (audio, two tracks, transcript, notes), not one picture or video. The gallery's grid,
  tags and OCR don't fit it, and changing `library.json` is a contract-level change.
- **Effect:** meetings stay out of the gallery. `Output.listCaptures` ignores `.m4a`, so nothing there changes.

**D3. Two speakers: "You" and "Others".**
- **How:** the microphone is you; system audio is everyone else.
- **Why:** this split is exact and costs nothing. Telling apart the people *inside* the call would need speaker
  diarization, which macOS doesn't offer and which would mean a third-party model (not allowed by the contract).
- **Renaming:** you can rename either speaker per meeting (… › Speaker names).

## Recording

**D4. How each source is captured.**
- **System audio:** ScreenCaptureKit, sound only. A 2×2 picture at 1 fps is requested because the API requires video;
  those frames are thrown away.
- **Microphone:** AVCaptureSession rather than ScreenCaptureKit's microphone. That works on macOS 14 (SCK's needs 15+),
  lets you pick any input, and gives a separate file per source.
- **Permissions:** it reuses the Screen & System Audio Recording permission the app already has.

**D5. Files and where they go.**
- **Folder:** `<captures>/Meetings/<yyyy-MM-dd HH-mm> <title>/`, holding:
  - `meeting.m4a`: both sources mixed, which is what you share.
  - `tracks/others.m4a` and `tracks/you.m4a`.
  - `meeting.json`: the source of truth.
  - `transcript.md`: readable notes.
  - `transcript.json`: for AI agents.
  - `edited.m4a` / `edited.md`: after you export an edit.
- **Recording in progress:** it goes to a hidden `.recording-<id>` folder and gets its real name only once it's saved,
  so a crash never shows a half-written meeting.
- **Renaming:** renaming a meeting renames its folder.
- **Deleting:** moves the folder to the Trash.

**D6. Shortcuts.**
- **⌃⌥M:** start or stop recording.
- **⌃⌥⇧M:** mark a moment.
- **Why these keys:** both were free in the app's ⌃⌥ scheme.
- **Also in:** the palette, the menu bar (which shows the running time) and the CLI (`AtherScreenshot meeting`, `mark`,
  `meetings`).

**D7. The floating recording bar.**
- **Shows:** a level meter per source (so you can see both are being heard), Mark (with a count), Pause, Stop and
  Discard. Discard asks first.
- **Position:** top-center of the screen by default; it remembers where you drag it.
- **Privacy:** it's left out of screenshots and screen recordings.

**D8. A one-time reminder about consent.**
- The first recording shows "Let everyone know you're recording". Recording calls needs consent in many places, so a
  meeting recorder should say so once, without nagging.

**D9. Offering to record when a call starts (on by default).**
- **How:** every 3 s the app checks Core Audio's list of processes using audio input. When a known meeting app
  (Zoom, Teams, Slack, FaceTime, Webex, Discord, or a browser for Meet) starts using the microphone, it offers once:
  "Record this meeting?".
- **When the call ends:** if that app stops using the microphone for 20 s while you're recording, it offers to stop.
- **Privacy:** it only reads *which* apps use input, never any sound.
- **Turn it off:** Settings › Meetings › Offer to record when a call starts.

**D10. Quitting and updating mid-recording.**
- Quitting saves the meeting first (up to 25 s), the way screen recordings already do.
- The updater won't install while a meeting is being recorded.

## Transcript and AI

**D11. Everything stays on this Mac.**
- **Transcription:** Apple's `SpeechTranscriber` (macOS 26+). The first use of a language downloads its model through
  macOS.
- **macOS 14–15:** on-device `SFSpeechRecognizer`, in 55-second pieces. If a Mac can't recognize a language on the
  device, the app says so instead of sending audio to Apple. That's deliberately stricter than Auto captions.
- **Language:** the system's by default; there are 21 choices in Settings.

**D12. Each track is transcribed on its own, then merged.**
- **Lines:** a new line starts after a 1.2 s pause, after a sentence end followed by a 0.5 s pause, or once a line
  reaches 30 s.
- **Echo:** on speakers, your microphone also hears the others. A "You" line whose words are ≥ 60 % the same as an
  Others line at the same time (±1.5 s) is dropped as echo. Real cross-talk with different words is kept.

**D13. Summaries use Apple Intelligence's on-device model (FoundationModels, macOS 26+).**
- **Output:** a summary, key points, decisions and action items.
- **Long meetings:** summarized in parts first (about 6,000 characters each), then the parts are combined.
- **When it runs:** automatically after transcription (setting on by default).
- **Create menu:** a follow-up email, show notes, key quotes or highlights.
- **Without Apple Intelligence:** the Summary tab says why. Transcripts, editing, exports and transcript.json all work
  without it, so an AI agent can do the summarizing instead.
- **Compatibility:** FoundationModels is weak-linked (checked: `LC_LOAD_WEAK_DYLIB`), so the app still opens on
  macOS 14 and 15.

## Audio editing (your second prompt)

**D14. Text-based editing, Descript-style, simpler.**
- **Basics:** every word keeps its time. Click a word to play from it, ⇧-click to select a stretch, ⌫ to cut it (struck
  through in red); ⌫ on cut words restores them.
- **Undo:** ⌘Z / ⇧⌘Z.
- **Trim:** trim the start or end at the playhead.
- **Non-destructive:** cuts are a list in meeting.json and the recordings are never changed. "Play the edit" (on by
  default) skips the cuts while playing, so you hear the result.
- **Left out on purpose:** overdub/AI voice, multitrack timelines, scenes and video. You asked for simpler and
  clean-up-oriented, and those would also need cloud models.

**D15. Clean up.**
- **Remove filler words:** um, uh, er, ah, hmm, mm…
- **Remove repeated words:** "the the" or "I I"; the first of the two is cut.
- **Shorten long pauses:** a silence over 1 s becomes 0.4 s. Silences are found in the audio's loudness: the quiet
  threshold is the room's noise floor × 3, kept between -50 and -30 dBFS.
- **Repeating it:** running a clean-up again replaces its earlier cuts rather than stacking them.
- **Known limit:** Apple's transcriber sometimes leaves a filler out of the transcript, mostly at the start of a
  sentence ("Um, let's…"), and may "correct" a stammer ("the the" → "that the"). Those can't be found from the text.
  Fillers in the middle of speech ("so, um, I think") are kept with their times and do get cut (checked with real
  speech).

**D16. Exporting the edit.**
- **Audio:** `edited.m4a` joins what's left with 10 ms fades at each cut, so there are no clicks.
- **Enhance (on by default):** an 80 Hz rumble filter, a small presence lift, gentle compression, and one gain for the
  whole file (speech to about -18 dBFS RMS, peaks under -1 dBFS).
- **Transcripts:** Markdown, text, SRT or JSON, either edited (re-timed to the edit) or full.

**D17. Made for AI agents.**
- **transcript.json:** rewritten on every save. It has speakers, lines, word times, cut flags, the edited text and
  times, markers, notes, the summary and outputs.
- **New MCP tools:**
  - `start_meeting_recording` / `stop_meeting_recording` / `add_meeting_marker`: record.
  - `list_meetings` / `get_meeting`: read; `get_meeting` waits for the transcript and gives word times on request.
  - `edit_meeting`: fillers, repeats, pauses, cuts and restores by time or by line.
  - `export_meeting`: write the edited audio and transcripts.
  - `transcribe_meeting` / `import_audio`: re-transcribe, or bring in any file.
- **Same as the window:** an agent can do everything the Meetings window does.

**D18. Any audio can be edited.**
- "Open audio to edit…", or a drop on the Meetings window, copies a file's sound into Meetings as a new meeting. That
  covers podcasts, voice memos, or a video's sound.
- **Speaker label:** imported files are labeled "Speaker" (rename it as you like).

## Release

**D19. Merge authority and release word, from you.**
- **The rule it lifts:** `docs/intent/README.md` says agents never merge or release.
- **Why I did it here:** your prompt explicitly asked me to "merge to main and release it", so I took that as your word
  for this intent only. The contract itself is unchanged.

**D20. Mac build 0.0.2.15. The version people see stays 0.0.2.**
- **Build:** a re-release raises only the build number, as the contract says.
- **Windows:** stays at 0.0.2.13.
- **How it was published:** `macos/build.sh publish` uploaded the DMG and zip to the existing v0.0.2 GitHub release
  and updated its `latest.json` "macos" entry, keeping "windows". `downloads/latest.json` is committed to main. Copies
  already installed are offered the update.
- **Undo:** run `./build.sh publish` from an earlier build, or edit the "macos" entry of the release's latest.json.

**D21. Testing in the real app used your real app identity.**
- **Why:** the end-to-end check ran the built app (same bundle id) to record real system audio. Your copy wasn't
  running, so nothing of yours was stopped.
- **Isolation:** for the test, `SaveFolder` pointed at a scratch folder and `MeetingMicrophone` was off, so no
  microphone permission prompt needed answering for you. Both settings went back to what they were afterwards. The
  results are recorded in [progress.md](progress.md).

## Tools

**D22. No new tools were installed.**
- **Used instead:** everything needed was already on the Mac: Xcode 27, the macOS 27 SDK (SpeechAnalyzer and
  FoundationModels), `say` to make real speech for tests, `screencapture` and `gh`.
- **Not installed:** ffmpeg and Python audio libraries. They would only have duplicated AVFoundation, and the app must
  stay dependency-free.
