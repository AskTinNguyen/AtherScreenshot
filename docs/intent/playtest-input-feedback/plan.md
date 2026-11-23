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

Final validation supersedes the earlier blocked result: after the owner's clarification, native clipboard access succeeded without another source/host change. Fresh complete `test.bat` exited 0 with `178 tests, 2942 checks, 0 failed`, no skips. All acceptance items are met. Branch and updated portable build are ready for owner review; leave unmerged/unpublished under the project contract. The earlier denial's precise cause remains unknown.

## Rev 3 — shorter Windows commands (2026-11-21)

1. Reproduce missing one-word commands in the parser and actual SAPI generated-speech tests.
2. Accept "bug" / "stop", preserve command spans and description words, and update visible Windows instructions. Retain longer aliases and "bug cancel".
3. Run focused release voice/parser checks and the full release suite, recording any recurring clipboard access denial without weakening assertions.
4. Under the owner's existing authorization, update the installed executable, verify its hash and running path, and update draft PR #25 into main. Preserve settings/data; leave unmerged and unpublished.

## Rev 4 — observable live input and stop reliability (2026-11-23)

Design: keep one click-through, capture-excluded input HUD. Show an explicit live/quiet mic percentage above its meter and prioritize current speech over stale heard text. Extend the HUD with controller slot, disconnected/connected status, active control names and elapsed time since input. Monitor when Show game controller is enabled, including before recording. The recorder and monitor use separate pollers so the HUD cannot consume a recorded tap. Enlarge the recorded pad from 220 to 330 DPI-scaled pixels while retaining small-frame bounds.

Voice: allow low-confidence stop only from the command grammar while a report is open; retain start/dictation confidence protections. Give the command rule priority over dictation and synchronize dictation for accepted embedded commands too. Do not treat a synthetic pass as proof of the owner's mic/accent.

Controller: compare primary XInput and the system legacy XInput reader when the primary is neutral/unavailable. Prefer actual activity, preserve primary activity, never invent button data, and retain native disconnected status. This is a mitigation for neutral background input; Steam Input can still hide devices from external apps, so keep the diagnostic status visible and document physical limits.

Validation: observe RED for low-confidence stop, stale speech feedback, size, input indicators and background-neutral fallback; run focused GREEN and decoded MP4 checks, inspect HUD snapshots, run full release suite, then update the authorized local install and draft PR with exact results.

Outcome: final release executable passes the complete suite (`183 tests, 2984 checks, 0 failed`, exit 0, no skips). Synthetic input/HUD evidence was inspected; exact tested executable installed and restarted with matching hash. Earlier transient clipboard comparison failure and its unchanged focused/full recovery remain documented. Stop controller monitoring before quit-time save callbacks. Leave draft PR #25 into main unmerged; physical controller/Steam Input and microphone/accent verification remain manual limits.
