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

## Rev 5 — Bug / Done

Observe parser and SAPI RED with "done" fixtures before changing production. Add "done" as the save/Stop action in both grammar and parser; preserve old aliases and narrow low-confidence save policy. Update visible Windows instructions and fixtures, run final release/full validation, install/restart the exact tested executable, and update draft PR #25. Keep physical mic/accent and live listener limits explicit.

Follow-up scope: the recording lifecycle gates the HUD (including automatic/error/discard completion) while voice may keep listening in the background. Add 0..20 dB microphone gain in Recording Settings, default 0 as chosen by owner, applied through the shared WASAPI capture to narration and bug voice.wav; leave system audio and SAPI input unchanged. Observed RED/GREEN for gating and gain. Verify persistence/clamping and run the full release suite before updating the installed binary/PR.

Outcome: release builds, all feature checks pass, installed exact tested binary and restarted it. Both complete rev 5 runs have 185 tests/3001 checks and one intermittent integration failure (first clip-copy, then QuickPrompts); each failing test passes immediately in isolation without source/assertion changes. Keep full-suite gate open and report the limitation in draft PR #25. Read-only installed-app checks confirm no HUD window while idle and MicrophoneGainDb=0. Live mic/accent and USB/Bluetooth/Steam Input still require owner validation.

## Rev 6 — Settings recovery and live recognition diagnosis

Reproduce hidden-window reopen and the isolated low-confidence dictation finish filter before production changes. Explicitly show hidden Settings and recover from hidden-launch first-show behavior. Accept only whole-utterance dictation save commands at low confidence while a report is open, keeping start/embedded guards. Add bounded metadata for live diagnosis with no transcript/audio. Run focused and full release checks, then gracefully replace the authorized installation and update draft PR #25. Live mic/accent proof remains open.

Rev 6 outcome: hidden-window regression and focused release voice/Settings tests pass. Full release has one recurring QuickPrompts clipboard assertion failure, passing on an unchanged focused retry; full acceptance remains open. Exact tested exe installed, and the Settings CLI now yields a visible installed Settings window by native metadata. Live owner speech/accent reliability remains unverified; use bounded metadata for the next reproduction.

## Rev 7 — isolated Sherpa trial

Owner approved trying Sherpa first, preferring a free model. Design: use official English GigaSpeech 3.3M INT8 KWS weights and CPU runtime, with only Bug/Done keywords. Evaluate saved authorized local microphone evidence plus generated positive/negative utterances, then offer a short live dry run. Never upload audio or commit personal samples. Keep the installed app listening unchanged during dry-run evaluation; do not wire an unmeasured recognizer into bug creation. Model archive README declares Apache License 2.0; retain source, version and hashes in the trial report. Runtime and Python dependencies are isolated outside the repository.

Trial implementation plan (sequential, no agents):
- [x] Check official runtime/model source and free license declaration; pin runtime 2.0.3 and archive provenance.
- [x] Write a small standalone runner with actual model positive/negative checks. No report-state/debounce logic or app trigger is needed for the isolated trial; do not add an unused production state machine.
- [x] Tokenize custom Bug/Done with the supplied BPE model and the newer model's phoneme lexicon; run real inference on generated Bug/Done and negative Mom/background-like phrases, without treating TTS as accent proof.
- [x] Run the same engine on the owner's existing local voice.wav samples; report detections without printing descriptions or changing recordings.
- [x] Run a bounded, announced live dry run on the VR P10 mic, reporting only keyword events, peak signal, CPU timing and dropped blocks. No background service or endless recording.
- [x] Record exact results and a usable local replay launcher. Decision: do not integrate these tested models into the native backend; real microphone samples did not pass. Include code/docs only in the existing draft PR update.

## Rev 8 — false-start guard

Owner reports Bug starts randomly. Add a live-only two utterance confirmation within 2.5 seconds for standalone Bug; show the first confirmation hint, clear pending state whenever a report is open or another command arrives, and preserve the immediate hotkey path. Keep Done/Stop single utterance. Validate with RED log evidence, focused Release tests, install and PR update. Do not claim full-suite green while the known QuickPrompts integration failure remains.
