# Bug Report: Findings

Discoveries that may change the intent. The worker adds entries; the orchestrator resolves them.

<!--
## F-<n> (<YYYY-MM-DD>, rev <r>) | blocking: yes | no | status: open | accepted | rejected

**Found:** <what, with evidence>
**Proposed amendment:** <the change to prompt.md>
**Resolution:** <decision and resulting rev, filled by the orchestrator>
-->

## F-1 (2026-10-21, rev 1) | blocking: no | status: open

**Found (orchestrator, from L-1):** the wish says the team drafted a fuller requirement for this feature last weekend ("cuối tuần rồi C có draft requirement nhưng chưa kịp chạy"). The draft isn't in the repository or the prompt, so rev 1 is built from the three-line wish.
**Proposed amendment:** when the owner shares the draft, reconcile it with D1–D9 and A1–A9 in a new rev. Until then, build as written.
**Resolution:** open, waiting for the draft.

## F-2 (2026-10-21, rev 1) | blocking: no | status: accepted

**Found (orchestrator):** the team speaks Vietnamese. Windows' offline dictation (SAPI desktop recognizers) covers English and a few other languages, not Vietnamese. On the Mac, `SFSpeechRecognizer` knows `vi-VN`, but on-device recognition (which D1 requires for privacy) may not cover it. A description spoken in Vietnamese would be transcribed badly or not at all.
**Proposed amendment:**
- (a) The commands stay the English phrases "bug start" / "bug stop" / "bug cancel" (short, recognizable).
- (b) The bundle always keeps the narration audio (`voice.wav`, D6), so nothing said is lost.
- (c) The overlay uses whatever was recognized; with nothing recognized, it shows only the header.
- (d) Optional later: allow cloud recognition on the Mac for `vi-VN` behind its own opt-in. That's a privacy call for the owner.

Recommendation: (a)–(c) now (as in rev 1), and ask the owner about (d).
**Resolution:** (a)–(c) are in rev 1. (d) was accepted by the owner (L-2): an opt-in Mac setting, D10 in rev 2.

## F-3 (2026-10-21, rev 2) | blocking: no | status: open

**Found (worker):** on this Mac the test runner has no speech recognition permission: `SFSpeechRecognizer.authorizationStatus()` is `notDetermined` (0), and the tests must never ask for it. So `testRecognitionFromASaidFile` (a `say`-made "bug start … the door does not open … bug stop" through on-device recognition and the bug logic) skips with that reason. Everything around it is tested without permission: the bug logic, how settled words are passed on (`BugTranscript`), that every voice-command request has `requiresOnDeviceRecognition = true`, and the Vietnamese seam. On Windows the same path (SAPI, with TTS speech) ran and passed on the runner (A2).
**Proposed amendment:** none to the intent. A7's voice recognition and a real Vietnamese transcription are `owner`, as A7 says: turn on Settings › Bug reports › Listen for "bug start" / "bug stop" in the Mac app (it asks for the Microphone and Speech Recognition once), say "bug start … bug stop" twice, and check both bundles (part of A9). Optionally, allowing Speech Recognition for the terminal that runs `xcrun swift test` makes the test run.
**Resolution:**

## F-4 (2026-10-21, rev 2) | blocking: no | status: accepted

**Found (worker):** this Mac (macOS 27.0.1, Apple silicon) reports `SFSpeechRecognizer(locale: vi-VN).supportsOnDeviceRecognition == true` (and for en-US). So Vietnamese can be recognized on the Mac itself, without Apple's servers, at least on recent macOS. D10 as written always sends `voice.wav` to the server when "Transcribe Vietnamese with Apple's servers" is on.
**Proposed amendment (owner, privacy):**
- (a) Keep D10 as built: the opt-in always uses the server (simple; the setting's name says what happens).
- (b) Recommended: with the setting on, transcribe `voice.wav` as `vi-VN` on the Mac when `supportsOnDeviceRecognition` is true, and use the server only when it isn't. The setting could then read "Transcribe Vietnamese (with Apple's servers when this Mac can't)". Narration then leaves the Mac only where it has to.
- (c) Add a separate, default-on "Transcribe Vietnamese on this Mac" when on-device vi-VN exists, and keep the server opt-in for older Macs.
**Resolution:** accepted as D11 in rev 3 (R-1): on-device first, servers only as the fallback.
