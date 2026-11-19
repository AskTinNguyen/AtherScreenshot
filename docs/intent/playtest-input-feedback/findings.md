# Findings

## F1 — controller ownership

The poller previously selected the first connected XInput slot and never checked whether another connected slot received input. A simulated idle slot followed by an active slot can establish the defect. It does not establish the exact device setup on the owner's other PCs.

## F2 — recognition lifecycle and observability

The SAPI listener ignores `Next()` returning false after `SPEI_END_SR_STREAM`, leaving listening state true after the stream ends. Its interests omit audio-level, false-recognition and phrase-start events. Low-confidence microphone commands are silently discarded, whereas file tests bypass that policy. These are directly inspectable code defects; quiet narration alone cannot establish what the recognizer heard, because SAPI and narration capture choose their microphones separately.

## F3 — short taps, analog activity and recording pauses

Regression tests reproduced short taps disappearing before a readable duration, slow analog pulls failing per-sample activity thresholds, and previous controller highlights leaking across ownership changes. The fixed poller follows deliberate cumulative activity and clears historical state on a switch. Buttons and trigger peaks have a documented 120 ms visibility hold.

A decoded-video regression then reproduced four stale A/trigger samples after a tap entirely inside a short recording pause. Resume now discards history while retaining currently held controls. The regression passes with zero stale samples and six frames of held input.

## F4 — full-suite environment limitation

Before implementation, baseline a686c12 produced `164 tests, 2787 checks, 7 failed`. These failures concern clipboard-dependent app/video tests. A direct native diagnostic found `OpenClipboard(NULL)` denied with Win32 error 5; it did not establish that the desktop was locked. Do not weaken those tests or alter unrelated clipboard/security code. Functional filtered passes cannot satisfy the contract's full-suite acceptance gate.

Final release full suite produced `176 tests, 2916 checks, 8 failed`: the same seven clipboard cases plus `video_editor_loop`. A targeted loop retry failed on both the new build and the unchanged main checkout baseline executable: each `1 tests, 15 checks, 1 failed`, with playback remaining at zero. This establishes that the additional failure also reproduces without this feature, but does not diagnose its environmental/player cause. Logs: `final-full-tests.log`, `final-loop-recheck.log`, `baseline-loop-recheck.log` in the external artifact folder.

## F5 — physical/acoustic validation boundary

The owner explicitly requested simulation while away. The controller demo uses injected native XInput responses and a synthetic backdrop through the real poller, renderer, recorder, encoder and decoder. The voice demo uses synthetic status transitions through the production HUD painter; a separate actual SAPI synthesized-speech test covers accepted commands. Neither establishes physical controller/Steam Input compatibility or recognition of the owner's microphone/accent. No new dependency or voice service was introduced.
