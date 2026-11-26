# Meeting Recorder: Progress

- Working under rev: 2
- Worker: the orchestrator session itself (the owner was away; one agent did the work and a separate reviewer agent checked it)
- Current step: release (Mac build 0.0.2.15)
- Next step: none
- PR: see the merge commit on main (opened and merged by this session, per L-1)

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | `MeetingTests.testMixTwoTracksIntoOneFile`: two tracks become one mixed AAC track, length 3.0 s. Real app (S6): `meeting.m4a` 24.16 s, 2 ch 48 kHz AAC, `tracks/others.m4a`, `meeting.json`, marker at 0:10 |
| A2 | met | `MeetingTests` lines, echo, exports, store round trip: 8 tests, 0 failures |
| A3 | met | `MeetingSpeechLiveTests`: "good morning, everyone. let's review the budget for the summer release, though the deadline is friday." (SpeechTranscriber, en_US, 1.3 s). Real app transcript in `proof/e2e-transcript.md` |
| A4 | met | `proof/ui-meetings-transcript.png`, `ui-meetings-summary.png`, `ui-meetings-notes.png`, `ui-meetings-empty.png`, `ui-meeting-bar.png` (MeetingSnapshotTests, 5 passed) |
| A5 | met | S6: the built app, started from the CLI (`AtherScreenshot meeting`), recorded `say` speech from system audio; transcribed on the Mac; `proof/e2e-real-meeting.png` is that recording drawn by the Meetings window (real waveform, transcript, cuts) |
| A6 | met | `proof/ui-settings-meetings.png`; S7: real app over MCP `--mcp`: tools/list has the 9 meeting tools; list_meetings, get_meeting (word times), edit_meeting, export_meeting all answered |
| A7 | met | `./build.sh` → "Built build/Ather Screenshot.app (0.0.2)"; `xcrun swift test` → see S9 |
| A8 | met | README (feature row, shortcuts, Guide › Meetings (Mac), MCP tools, Platform differences), macos/README (Meetings, permissions, shortcuts), RELEASE_NOTES (build 0.0.2.15), DECISIONS.md |
| A9 | met | `MeetingEditTests.testCutArithmetic`, `testEditedTranscriptDropsCutWordsAndRetimes` |
| A10 | met | `MeetingEditTests.testFillersAndRepeats`, `testPauseDetectionOnLevels`, `testExportEditedAudioIsShorter` (7.0 s → 3.9 s, with and without Enhance). Real app: edited.m4a 14.04 s for an expected 14.1 s |
| A11 | met | `MeetingEditTests.testAgentToolsEditAndExport`; S7 over real MCP; `transcript.json` written with every save |
| A12 | met | `proof/ui-meetings-transcript.png` (struck-through cuts, Clean up, Cut, undo/redo), `proof/e2e-real-meeting.png` |

## Steps

- S1 (rev 1, 2026-11-26): pulled main (a686c12, up to date), read the Mac app (Recorder, BugReport speech, Agent/MCP, Settings, Gallery), baseline `./build.sh debug` built.
- S2 (rev 1→2): spec in prompt.md; L-2 added text-based editing (rev 2).
- S3: model and store (Meeting.swift), editing engine (MeetingEdit.swift), recorder (MeetingRecorder.swift), speech and AI (MeetingTranscribe.swift), HUD, Meetings window, detector, MCP tools, settings, menu, quit and updater guards. `xcrun swift build`: Build complete, no new warnings. FoundationModels is `LC_LOAD_WEAK_DYLIB` (otool).
- S4: probe of the real transcriber: mid-sentence fillers ("so, um, I think, uh") come back as words with times; a filler opening a sentence and stammers can be dropped or changed by the recognizer (DECISIONS D15).
- S5: tests and snapshots. Found and fixed: a slower, older waveform load could overwrite a newer one (load generation); the recording bar's controls weren't centered.
- S6: real app end to end (SaveFolder and MeetingMicrophone pointed at test values, restored after). **Found a crash:** the async save resumed off the main thread and showed its notification there (SIGABRT in Toast). The audio was already saved. Fixed with @MainActor on the recorder's async steps, and unfinished transcripts now resume at launch. After the fix the same meeting was transcribed on relaunch.
- S7: real MCP session (`AtherScreenshot --mcp`, AllowAgents on for the test, restored after): list → get (word times) → edit (4 pauses shortened, 1 line cut, 24.2 s → 14.1 s) → export (edited.m4a 14.04 s, md, srt, json).
- S8: the screen locked during S6, so window screenshots of the live app were impossible (entering the owner's password is not allowed). The real recording was rendered by the Meetings window code offscreen instead (`MeetingSnapshotTests.testRealRecording` with ATHER_E2E_MEETING).
- S9: an independent correctness review (a reviewer agent reading the diff, 2026-11-26) found 10 issues, mostly data loss on rare paths. All were fixed:
  - Quitting while a meeting was still saving now waits for it, then for a screen recording if one is also running.
  - The tracks get their real folder and meeting.json before mixing. The mix goes to `.mixing.m4a` and is redone at launch, and a failed mix never deletes tracks.
  - Notes are saved to the meeting they were typed in, and flushed when switching meetings or quitting.
  - Clean up ignores levels measured for another meeting.
  - The pipeline looks the meeting up again for each track, so a rename in between doesn't break it.
  - The agent's edit applies to the latest stored copy.
  - A disconnected microphone switches to the default one.
  - A failed system-audio start no longer counts as a source.
  - Cuts closer than 30 ms are merged, so fades can't overlap.
  - Only this player's end of playback counts, and stop while starting is refused for agents.
  - New tests: `testSliversBetweenCutsAreMerged`, `testNotesGoToTheMeetingTheyWereTypedIn`, `testUnmixedMeetingPlaysItsTrackAndIsMixedAtLaunch`.
- S10: final gate. `./build.sh` → "Built build/Ather Screenshot.app (0.0.2)"; `xcrun swift test` → "Executed 182 tests, with 23 tests skipped and 0 failures (0 unexpected)". The skips are the snapshot and live tests that need ATHER_TEST_OUT; they ran in S5–S8.

## Reconciliations

- rev 1 → 2: everything built for rev 1 stays; transcript lines now keep word times (needed for editing), and the exports take an "edited" switch.

## Residual risk

- Apple Intelligence reported "still getting ready" (`modelNotReady`) on this Mac during testing, so a live summary couldn't be produced here. The parsing and the map-reduce prompts are tested; the button explains why when the model isn't available.
- The microphone path (AVCaptureSession) wasn't exercised live, because that would have meant answering a macOS permission prompt for the owner. The capture code mirrors the system-audio path, which was exercised live.
- The legacy speech path (macOS 14–15, SFSpeechRecognizer in 55 s pieces) wasn't run on this macOS 27 Mac.
