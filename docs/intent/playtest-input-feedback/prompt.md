# Playtest input feedback

- Rev: 1
- Status: active
- Area: Recording
- Base: a686c12

## Goal

Make controller presses visible in recorded video and make local voice bug commands observable and recoverable. The owner is away and explicitly authorized autonomous diagnosis, simulated tests, implementation and a demo video.

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

## Boundaries

No cloud recognition, new dependencies, driver installation, user media in commits, real support-folder changes, closing the owner's app, version/release changes or merge to main. Hardware and accent robustness cannot be established by clean synthetic speech; report that limit.
