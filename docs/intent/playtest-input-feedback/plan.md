# Playtest input feedback implementation plan

**Goal:** Visible recorded controller presses and observable/recoverable local bug voice commands.

**Architecture:** Native input poller and recorder stay in place. Voice events feed a small status model and a persistent no-focus panel. The existing report tracker remains the source of bug start/stop state.

**Tech stack:** C++20, Win32, XInput, SAPI, WASAPI, Media Foundation; no new dependencies.

1. Establish baseline build/full tests in the isolated worktree.
2. Write failing controller tests for idle-first/active-second slots and minimum press visibility. Implement polling/hold fixes. Record synthetic input through the real recorder, decode the MP4 and verify button pixels.
3. Write failing voice policy/status tests for rejection visibility, microphone/signal labels, persistent bug badge, stream-end status and retry delay. Implement the status model and no-activate panel.
4. Subscribe to SAPI signal/rejection/end events; use one policy for file and live recognition; restart failed streams with bounded backoff. Report actual recognition input and retain command feedback. Test synthesized speech and lifecycle events without microphone access.
5. Expose microphone selection/retry if the native device behavior supports a reliable small implementation; preserve existing config defaults. Update instructions for English commands and Vietnamese narration limits.
6. Build release, run the entire native test suite, review source changes and inspect synthetic visual/video artifacts. Document reproducible commands, exact results and hardware/voice limitations. Leave a local branch/build for review; do not publish or replace the owner's running app.

## Execution outcome (2026-11-20)

Implementation, synthetic demos, focused release checks and independent review are finished. Microphone retry is exposed; input selection stays with Windows/SAPI defaults rather than adding saved configuration. Release build succeeds. The complete suite remains red on clipboard/player failures that reproduce on baseline; exact evidence is in progress.md. Keep acceptance A1–A4 open under the repository contract and leave the local feature branch unmerged.

Rev 2 fixes the playback end-clock mismatch and NULL clipboard write ownership, with observed RED/GREEN native regressions. Release full validation is now `178 tests, 2928 checks, 7 failed`: loop and feature checks pass, native clipboard access remains denied even in standalone probes. Resolve that host prerequisite before claiming the seven integrations or the complete gate pass. The user's normal desktop copy/paste clarification remains pending.
