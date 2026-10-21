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
