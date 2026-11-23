# Playtest input feedback

- Rev: 4
- Status: active
- Area: Recording
- Base: a686c12

## Goal

Make controller presses visible in recorded video and make local voice bug commands observable and recoverable. The owner is away and explicitly authorized autonomous diagnosis, simulated tests, implementation and a demo video.

Rev 2 also covers diagnosing and fixing all eight remaining native self-test failures (clipboard integrations and video playback loop), with the full test suite as the acceptance gate. Preserve actual clipboard/file/decoded-frame assertions; do not skip or weaken failures to obtain a green result.

Rev 3 makes the Windows voice commands shorter: say "bug" to start and "stop" to save. Update the grammar, command parsing, HUD, tray/settings text and Windows guide. Keep the existing hotkey and "bug cancel" command. Existing longer commands remain accepted for compatibility, while all visible Windows instructions teach the new short commands. macOS behavior is outside this Windows change.

Rev 4 addresses the owner's report that "bug" opens but "stop" does not save, and controller buttons remain unlit. Show an explicit live microphone signal indicator and controller connection/input feedback on screen, including idle/disconnected states. Enlarge the controller in recordings by 50%, subject to small-frame bounds. The owner uses an Xbox controller over USB and Bluetooth with Steam Input enabled. Preserve local recognition and record honest hardware validation limits.

## Design

Keep the native Windows implementation and existing hotkey workflow. Poll connected controllers for meaningful activity, keep brief presses visible, and verify the encoded video rather than only the drawing function. Show a persistent, non-activating voice panel with the recognition microphone, audio meter, recognized/rejected speech, listening/reconnecting state and a Bug started badge until the report closes. Keep speech processing local. Preserve low-confidence protection but explain rejected commands; recover a terminated recognition stream instead of leaving stale listening state.

## Acceptance

| ID | Requirement | Proof |
| --- | --- | --- |
| A1 | Active secondary controller and brief button presses reach the overlay | gate: test.bat |
| A2 | A real MP4 recorded with injected controller input contains pressed and released button states | gate: test.bat |
| A3 | Voice status distinguishes initialization, signal, rejection, accepted command, failure/reconnection and an open bug | gate: test.bat; snapshot |
| A4 | File and live commands use the same acceptance policy; terminated streams do not remain falsely listening | gate: test.bat |
| A5 | Synthetic demo video uses the production rendering/recording path and clearly identifies simulated input | build; review |
| A6 | Instructions, limitations and actual validation are documented | review |
| A7 | Clipboard integrations and selected-clip playback loop pass together with the entire native suite | gate: test.bat |
| A8 | Windows accepts spoken "bug" and "stop", preserves the first description word, and displays the short command instructions | gate: test.bat |
| A9 | An open report accepts a low-confidence explicit stop command, without allowing low-confidence starts or incidental dictation | gate: test.bat |
| A10 | On-screen input HUD distinguishes mic level, recognizing speech, controller disconnected/connected/receiving/idle; old heard text does not hide new speech | gate: test.bat; snapshot |
| A11 | Recorded controller is 50% larger at normal frame sizes and real alternate input is retained when another reader returns neutral | gate: test.bat; decoded MP4 |

## Boundaries

No cloud recognition, new dependencies, driver installation, user media in commits, real support-folder changes, closing the owner's app, version/release changes or merge to main. Hardware and accent robustness cannot be established by clean synthetic speech; report that limit.
