# Meeting Recorder: Record a Meeting's Sound, Edit It Like a Document, Get a Clean Transcript, Notes and Outputs (Mac)

- Rev: 2
- Status: active
- Area: macOS
- Owner: AskTinNguyen
- Skill: `none`
- Branch: `intent/meeting-recorder`
- Started: 2026-11-26

## Goal

Make Ather Screenshot a meeting audio recorder (L-1). During a Zoom, Meet, Teams, Slack or FaceTime call, one key records
the call's sound: everyone else (system audio) and you (the microphone), with no video. Afterwards there is one
`meeting.m4a` to share, a transcript that says who spoke (You / Others) with times, the moments you marked, and, where the
Mac has Apple Intelligence, a summary with action items. Everything stays on the Mac.

What exists to build on (2026-11-26):
- `Recorder.swift` records system audio (ScreenCaptureKit) and the microphone (macOS 15+) as AAC tracks next to video.
- `VideoEditor.transcribe` and `BugSpeech` use `SFSpeechRecognizer` on the device.
- `Agent.swift` / `MCP.swift` give AI agents tools (start_recording, add_marker, stop_recording…).
- This Mac runs macOS 27 with Xcode 27: `SpeechAnalyzer` / `SpeechTranscriber` (long-form, on-device) and
  `FoundationModels` (the on-device Apple Intelligence model) are available from macOS 26.

## Non-Goals

- **Windows.** The Windows app gets this as a later intent (no Windows toolchain here; see DECISIONS.md D2).
- **Video.** Screen recording stays as it is; a meeting recording is sound only.
- **Joining calls, bots, calendar integration, cloud transcription or cloud AI.** Nothing leaves the Mac.
- **Speaker identification beyond You / Others.** Telling apart the other people in the call needs diarization the OS
  doesn't offer; the two-source split is exact and free.
- **The capture gallery.** Meetings get their own window; the gallery keeps pictures and videos (D9).

## Decisions

The orchestrator's calls on the owner's behalf (L-1). Each is explained in DECISIONS.md.

- **D1: sources.** System audio through ScreenCaptureKit (audio only, no frames kept), optionally only from one app
  ("All apps" by default). Microphone through AVCaptureSession, so it works on macOS 14 and any input can be picked.
- **D2: files.** One folder per meeting: `<captures>/Meetings/<yyyy-MM-dd HH-mm> <title>/` with `meeting.m4a` (both
  sources mixed, AAC), `tracks/others.m4a` and `tracks/you.m4a` (one source each, kept for re-transcribing),
  `meeting.json` (title, times, sources, markers, transcript, summary), and `transcript.md` (readable notes, rewritten
  whenever the meeting changes).
- **D3: control.** ⌃⌥M starts and stops (palette: "Record meeting audio: start / stop"); ⌃⌥⇧M marks a moment. A small
  floating bar shows the time, a level meter for each source, Mark, Pause, Stop and Discard. The menu-bar icon turns red.
- **D4: transcript.** After Stop, each track is transcribed on the Mac: `SpeechTranscriber` on macOS 26+, else
  `SFSpeechRecognizer` on-device in one-minute pieces. Words are grouped into lines per speaker; a line the microphone
  only heard from the speakers (echo of Others) is dropped. Language: the system's, or one picked in Settings.
- **D5: summary.** With Apple Intelligence available, "Summarize" (automatic by default) writes a summary, key points,
  decisions and action items with the on-device model, in pieces for long meetings. Without it the button says why.
- **D6: Meetings window.** A list of meetings with search over titles and transcripts; the selected meeting shows its
  waveform with markers (click to jump), play / pause, ±15 s, speed, the transcript (click a line to jump; the line being
  spoken is lit), the summary, and notes you type. Rename, Copy transcript, Export (Markdown, text, SRT), Show in Finder,
  Transcribe again, Delete (to the Trash).
- **D7: meeting detection.** When another app starts using the microphone and a known meeting app is running, a
  notification offers "Record this meeting?" (Settings › Meetings › Offer to record when a call starts, on by default).
- **D8: AI agents.** MCP tools `start_meeting_recording`, `stop_meeting_recording` (waits for the file),
  `list_meetings`, `get_meeting` (transcript, markers, summary) and `add_meeting_marker`.
- **D10 (rev 2): edit the audio by editing the transcript** (Descript-style, simpler). The transcript keeps every word's
  time. Select words or lines and press Delete: that audio is cut (struck through, Restore brings it back). Cuts are
  non-destructive (`cuts` in meeting.json); the original files are never changed. Play skips the cuts, so you hear the
  edit. Trim start / end from the waveform.
- **D11 (rev 2): one-click clean-up.** Remove filler words (um, uh, er, hmm…), remove repeated words ("the the"),
  shorten long pauses (silences over 1 s, found in the audio, become 0.4 s). Each shows how much it saved and is one undo.
- **D12 (rev 2): export the edit.** "Export edited audio" writes `edited.m4a` (cuts joined with 10 ms fades, optional
  Enhance: rumble filter, gentle compression, loudness to about -16 LUFS-ish peak-safe gain) and the edited transcript
  with times on the edited timeline (Markdown, text, SRT).
- **D13 (rev 2): ready for agents.** `transcript.json` (speakers, lines, word times, cuts, markers, summary, original
  and edited times) is written with every save. MCP tools let an agent clean up and cut (`edit_meeting`: remove fillers,
  repeats, pauses, cut or restore time ranges or lines), export (`export_meeting`), and read word-level transcripts.
- **D14 (rev 2): outputs.** Besides the summary, "Create" makes a follow-up email, show notes or key quotes from the
  edited transcript with the on-device model, saved with the meeting. Any audio file can be opened into the editor
  ("Open audio to edit…", or dropped on the Meetings window), so podcasts and voice memos get the same editing.
- **D9: version.** Mac build 0.0.2.15. Windows stays at 0.0.2.13.

## Acceptance

| Id | What | Proof |
| --- | --- | --- |
| A1 | Recording a meeting writes `meeting.m4a` with both sources mixed plus the two tracks and `meeting.json`; pause, discard and markers work; times are recording time | `gate: mac` (MeetingTests) |
| A2 | Transcript: words from both tracks become timed lines with the right speaker; echo lines are dropped; Markdown, text and SRT exports are right | `gate: mac` (MeetingTests) |
| A3 | Real speech is transcribed on the Mac (a synthesized-speech file through the real transcriber) | `gate: mac` (MeetingTests, live) |
| A4 | The floating bar and the Meetings window render (recording, list + transcript + summary, empty state) | `snapshot` |
| A5 | The real app records real system audio end to end from the menu / hotkey and shows it in the Meetings window | `snapshot` (screen capture of the running app) |
| A6 | Settings › Meetings, palette commands, menu-bar items and MCP tools exist and work | `gate: mac` + `snapshot` |
| A7 | Full Mac gate: `./build.sh` succeeds and `xcrun swift test` reports 0 failures | `gate: mac` |
| A9 | Text-based editing: deleting words/lines makes cuts; edited-time mapping, restore, trim are right | `gate: mac` (MeetingEditTests) |
| A10 | Clean-up finds fillers, repeats and long pauses; edited audio is exported with the cuts removed (length checked) and the edited transcript re-timed | `gate: mac` (MeetingEditTests) |
| A11 | Agent tools edit_meeting / export_meeting work; transcript.json is written | `gate: mac` |
| A12 | The editor UI renders: struck-through cuts, clean-up bar, export | `snapshot` |
| A8 | README, macos/README, RELEASE_NOTES updated; DECISIONS.md complete | `review` |

## Constraints

- No third-party dependencies or downloaded binaries in the app (contract).
- macOS 14 stays the minimum: macOS 26 APIs behind `#available`, `FoundationModels` weak-linked.
- Tests never touch the real captures folder or library (`Settings.underTests`).
- Don't change how screen recording, bug reports or the gallery behave.

## Changelog

- rev 1 (2026-11-26): started from L-1.
- rev 2 (2026-11-26): audio editing and clean-up for agents and outputs (L-2): D10–D14, A9–A12.
