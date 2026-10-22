# Video Copy: Findings

Discoveries that may change the intent. The worker adds entries; the orchestrator resolves them.

<!--
## F-<n> (<YYYY-MM-DD>, rev <r>) | blocking: yes | no | status: open | accepted | rejected

**Found:** <what, with evidence>
**Proposed amendment:** <the change to prompt.md>
**Resolution:** <decision and resulting rev, filled by the orchestrator>
-->

## F-1 (2026-10-21, rev 1) | blocking: no | status: accepted

**Found:** A4's proof is `gate: test.bat` with the full run at `0 failed`, but the CI runner can't show 0 failed for any branch today. On main's code it reports `110 tests, 1674 checks, 11 failed` (run 39611596945, S0): 11 video tests that tie a frame's content to `index / fps`, which the runner (no GPU) reads one frame late. On this branch the same 11 fail and nothing else of the existing suite, apart from one run where `video_editor_plays_and_seeks` (real-time playback through the media engine) got only 0.6 s into 1.6 s of playing (run 39615745081); it passed in the run before with the same playback code. Whether the full suite passes on a real PC isn't known from here.
**Proposed amendment:** for this run, A4 (and the gate behind A1–A3) counts as met when a CI run's failures are exactly the runner's baseline set and every new test passes, quoting both lines. A4 becomes `met` outright once the separate runner-baseline task gets main's run to 0 failed, or the owner runs `test.bat` on a PC.
**Resolution:** the cause was found and fixed in `intent/runner-video-tests`: Windows' software encoder wrote B-frames one frame late. With that fix, the runner reports `0 failed`. A4 needs a Check run of this branch with the fix merged in (R-1).
