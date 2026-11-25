# Offline Sherpa keyword trial

This is a bounded, optional diagnostic tool, not the app's voice backend. It never sends app commands. The owner's November 23, 2026 trial did not establish reliable Bug/Done recognition, so the native app was not switched to Sherpa.

Use Python 3.14 on Windows x64. Keep dependencies, models, reports and audio outside the repository. For example, install `tools/sherpa_trial_requirements.txt` with pip `--target D:\App\AtherScreenshot-artifacts\playtest-input-feedback\sherpa-trial\python-deps`. The experiment does not change the global Python environment.

Official model downloads:

- [English GigaSpeech 3.3M](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01.tar.bz2). Archive SHA256: `f170013b4716e41b62b9bfd809687c207cef798ef9bc6534d524e17af9b6561a`. Its bundled README declares Apache License 2.0.
- [Chinese/English phoneme 3M](https://github.com/k2-fsa/sherpa-onnx/releases/download/kws-models/sherpa-onnx-kws-zipformer-zh-en-3M-2025-12-20.tar.bz2). Archive SHA256: `68447f4fbc67e70eee3a93961f36e81e98f47aef73ce7e7ca00885c6cd3616a6`. Its [author's model card](https://www.modelscope.cn/models/pkufool/icefall-kws-zipformer-zh-en-3M-2025-12-20) declares Apache License 2.0. It uses an fp32 decoder with the int8 encoder/joiner.

Sherpa itself is [Apache 2.0](https://github.com/k2-fsa/sherpa-onnx/blob/master/LICENSE). These local models require no API key or usage payment. Models, libraries and personal audio are not bundled or redistributed by this PR.

Example, after extracting a model outside the repo:

```powershell
$trial = 'D:\App\AtherScreenshot-artifacts\playtest-input-feedback\sherpa-trial'
python tools/sherpa_trial.py --deps "$trial\python-deps" `
  --model "$trial\sherpa-onnx-kws-zipformer-gigaspeech-3.3M-2024-01-01" `
  --wav "$trial\sample.wav" --report "$trial\sample-result.json"
```

For a live dry run, use `--list-devices` to select the correct input, then replace `--wav ...` with `--live-seconds 60 --device <index>`. Device indices can change; never assume the owner's index 31 applies to another machine. Wait for `ready: true` before speaking. The capture ends automatically, with a hard limit of 120 seconds. No raw audio is saved unless `--save-live-wav <new-path.wav>` is explicitly supplied. That option refuses to overwrite an existing file.

Settings for comparisons: `--score`, `--threshold`, `--precision int8|fp32`, `--input-gain-db 0..24`, and `--keywords BUG DONE`. Inference gain does not alter Windows volume, recorded app narration or source WAVs. The runner reads 16-bit PCM WAV, averages stereo to mono, and lets Sherpa resample. The report contains model hashes, keyword detections, CPU time, audio duration, signal peaks and capture drop/status counts. Reported keyword time is the audio block where detection became available, not an exact word boundary or measured end-to-end latency. Private sample paths are replaced by hash IDs; descriptions are not transcribed.

Observed results (small diagnostic corpus, not a general accuracy benchmark):

| Configuration | Synthetic intended commands, 12 total | Synthetic negative files, 4 total | Owner's confirmed live sample |
| --- | --- | --- | --- |
| English INT8, score 1 / threshold .25 | 8 detected | 0 detections | Not run live with these exact settings |
| English INT8, score 1.5 / threshold .15 | 11 detected | 0 detections | 0 detections |
| Phoneme INT8, score 1.5 / threshold .15 | 12 detected | 0 detections | 0 detections on replay |

The confirmed sample is 60 seconds: the owner was instructed to speak three Bug/Done pairs and three instances of Mom, then confirmed completion. The first live attempt is excluded from recognition scoring because the owner later said they were focused elsewhere. Both captures delivered 60 seconds with no audio drop/status blocks. The confirmed English run used 4.046875 CPU seconds over 60.2141 wall seconds (about 6.7% of one core); this is not a whole-machine CPU percentage. It received signal peaks up to 94.38%, so absence of detections is not absence of delivered PCM data. Two earlier owner WAVs also yielded no English detections with tested threshold, gain and fp32 variants.

The phoneme model also missed the confirmed sample with threshold .05 and inference gain +18 dB. A more aggressive English score 3 / threshold .05 did not fix that sample either. Narrow SAPI recognition on the same WAV returned some command/negative-word hypotheses; it is not treated as ground truth or a clean SAPI pass.

The outcome is a failed recognition trial for these models/settings on this owner's sample. Do not promote this trial to the native backend or claim accent robustness from the synthetic pass. Private audio and detailed reports remain in the external artifact folder and are not committed.
