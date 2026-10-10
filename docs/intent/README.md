# Intents

Each feature built by agents runs as a living intent in `docs/intent/<feature>/`:
- `prompt.md`: the spec, written by the orchestrator.
- `log.md`: every prompt, verbatim.
- `progress.md`: the worker's record and evidence.
- `findings.md`: discoveries that may change the spec.

The procedure is the `intent` skill ([AskTinNguyen/intent](https://github.com/AskTinNguyen/intent), vendored in `.agents/skills/intent/`). This file is the project's contract with it, and wins where they differ.

Not intents: releases (`package.bat publish`, only on the owner's word), one-off benchmarks, and questions.

## Statuses

| Status | Meaning |
| --- | --- |
| `planned` | Written ahead; no worker yet. A `Waits for` line names what must happen first. |
| `active` | A worker runs against it. |
| `parked` | Stopped on purpose, with a reason; resumes from the files. |
| `completed` | Every Acceptance row met and the PR merged. |

## Areas

Every intent names exactly one area.

| Area | Covers |
| --- | --- |
| Capture | Screenshots, region and window picking, scrolling capture, pins, OCR and redaction (`src/capture`, `overlay`, `wgc`, `scroll`, `pin`, `output`, `ocr`) |
| Editor | The picture annotation editor and collages (`src/editor`, `annot.h`, `collage`, `textdraw`) |
| Recording | Screen recording, sound, click/key and controller overlays (`src/recorder`, `audio`, `inputviz`, `gamepad`) |
| Video | The video editor: playback, scrubbing, edits, captions, export (`src/videoeditor`, `videoedit`, `videoio`, `media`, `videobench`) |
| Gallery | The capture gallery and library: tags, search, collections, smart suggestions (`src/gallery*`, `library*`, `smart`) |
| App | Tray, palette, hotkeys, settings, toasts, upload, updater, installer (`src/main`, `palette`, `settings*`, `toast`, `upload`, `updater`, `installer`, `net`, `json`, `common`, `logo`) |
| macOS | The Mac app (`macos/`, Swift). Built and tested on a Mac only |
| Platform | Build and packaging scripts, the self-test harness, docs, the agent workflow (`build.bat`, `test.bat`, `package.bat`, `src/selftest*`, `docs/`) |

## Proofs

Every acceptance item names one of these. A proof is a result in tool output, never a prose claim.

| Proof | What counts |
| --- | --- |
| `gate: test.bat` | `test.bat` (builds, then runs every self-test) exited 0, and its last line reads `<n> tests, <m> checks, 0 failed`. Quote that line. A filtered run (`test.bat --nobuild <filter>`) counts only for steps; acceptance needs the full run. |
| `build` | `build.bat` printed `Built build\AtherScreenshot.exe` with no `error C` and no new `warning C`. Check the log for "Built" before running the exe. |
| `gate: mac` | On a Mac: `cd macos && ./build.sh` succeeded and `xcrun swift test` reported 0 failures. Only for the macOS area; a Windows worker can't run it. |
| `bench` | `AtherScreenshot.exe --bench-export <dir> [tap] <clips>` timings, plus `--bench-compare` when output must not change, quoted from its output. Best of 3 runs, old and new builds run alternately. |
| `snapshot` | A PNG the app rendered of the changed UI (a self-test snapshot, or `--gallery-snapshots`). The worker opened it and named it in `progress.md`; the owner judges looks (see `owner`). |
| `review` | A versioned review record: who or what process, timestamp, content revision. |
| `owner` | The owner checked it themselves and said so (recorded in `log.md`). |

## Read First

Workers read these besides the intent files:
- `README.md`: the Guide section for the feature's area, and "Platform differences".
- For Video: the comments at the top of `src/videoio.h` and `src/videoedit.h`, and `src/videobench.cpp` before touching export speed.
- For macOS: `macos/README.md` and the matching `docs/HANDOFF-*.md`.

## Decisions

Engineering calls inside an intent's scope are the worker's. These go to the user as findings, with options and a recommendation:
- Product and scope, UI design and wording, content, licensing, privacy.
- Anything that changes saved files people already have (`library.json`, settings keys, the export format), or makes an export look different.
- Version numbers and releases.

## Never

Besides credentials, `.env` files, full chat transcripts, pushing to `main`, force-pushing and weakening a gate:
- Never commit or share the owner's recordings, screenshots or library. Copy test media outside the repo; tests make their own clips.
- Never touch the real support folder (`%APPDATA%\AtherScreenshot`). Tests run with `ATHER_SUPPORT_DIR` pointing elsewhere (`test.bat` does this).
- Never run `package.bat publish`, create or edit a GitHub release or tag, or change `downloads/latest.json`.
- Never change the version people see (`ATHER_VERSION_STR`, 0.0.2). A re-release raises only the build number, and only when the owner asks.
- No third-party dependencies or downloaded binaries in the app.
- Don't stop or close the owner's running copy of Ather Screenshot.

## Git And Merge

- Worktrees: `../AtherScreenshot-wt/<feature>`, branch `intent/<feature>`. Each worktree builds into its own `build\`.
- `main`: a push triggers nothing (there is no CI). Releases are manual (`package.bat publish`), only on the owner's word.
- Merge authority: none. The orchestrator says the PR is ready, with the evidence, and the owner merges.
- After a merge, confirm: nothing (no CI or deploy).

## On Close

- Update `README.md` (Features, the Guide section, shortcuts) when what people can do changed.
- Add a line to `RELEASE_NOTES.md` under the next update's section, when the change is visible to people.

## Shell Notes (Windows)

- The Bash tool collapses backslashes in heredocs. Write files with the editor tool, or run a script file.
- PowerShell doesn't wait for the app (a GUI exe) unless its output is piped, e.g. `| Out-String`.
