# QuickPrompts: Progress

- Working under rev: 3
- Worker: the Claude Code session that wrote the proposal
- Current step: none
- Next step: owner's check on a real PC (A10 looks, typing with a real keyboard); Mac port (A11, F-2)
- PR: see the PR linked from the branch `intent/text-shortcuts`

## Acceptance

| Item | Verdict | Evidence |
| --- | --- | --- |
| A1 | met | `test.bat --nobuild quickprompts` → `11 tests, 213 checks, 0 failed`: `quickprompts_matcher_expands_after_space_only`, `_whole_words_only`, `_backspace_reset_case_longest` |
| A2 | met | same run: `quickprompts_store_round_trip_and_precedence` (Vietnamese, emoji, braces; hand-edited file reloaded; broken file kept as `.bad` and not overwritten; a turned-off starter stays off) |
| A3 | met | same run: `quickprompts_paste_into_text_boxes` (an EDIT holding `Hello doitforme ` ends as `Hello Line one\r\nLine two!`, caret before `!`, clipboard back to `before`); `quickprompts_clipboard_round_trip_keeps_formats` (text and CF_DIB restored). End to end with real typing into another process: `proof/e2e-typed-doitforme.txt` (S4) |
| A4 | met | `quickprompts_paste_into_text_boxes`: a 3-line prompt pasted into a single-line box sends no Enter (`g_returnsSeen == 0`); e2e: `ENTERS=0` |
| A5 | met | `quickprompts_settings_commands_and_ui`: the five commands in the palette; `QuickPromptPaletteItems` lists every enabled prompt. Picking pastes via the same `InsertQuickPrompt` as A3 |
| A6 | met | `quickprompts_settings_commands_and_ui`: defaults `ExpandAsYouType=1`, `WelcomeShown=0`; listener on with the switch on, off with it off (`QuickPromptsListening()`); `proof/quickprompts-welcome.png` |
| A7 | met (review) | The typed characters live only in `TriggerMatcher::buf_` (64 chars) on the listener thread; nothing writes, logs or returns it (`quickprompts_win.cpp`, `quickprompts.cpp`); `list_quick_prompts` returns the store only. Reviewed in this session, 2026-11-14 |
| A8 | met | `quickprompts_placeholders` (every placeholder, `{{ }}`, unknown ones left; every starter uses known placeholders only); `{lastcapture:image}` pastes through `PutImage` → `CopyImageToClipboard` (format round trip covered by `quickprompts_clipboard_round_trip_keeps_formats`) |
| A9 | met | `quickprompts_store_round_trip_and_precedence` (team file read-only, user > team > starter), `quickprompts_import_export` (team file written and read back) |
| A10 | open (owner) | `proof/quickprompts-window.png`, `proof/settings-quickprompts.png`, `proof/quickprompts-welcome.png`; the live window ran in `--quickprompts-dev` (S4). Looks are the owner's call |
| A11 | open | Mac port not started (F-2) |
| A12 | met (review) | README: Features row, Default shortcuts row, Guide › QuickPrompts (Windows), AI agents tool list, Platform differences; RELEASE_NOTES › Coming next; docs/AI-AGENTS.md tool row |

## Steps

- S0 (rev 2, 2026-11-14): baseline. `build.bat` on `main` (ad65a28) → `Built build\AtherScreenshot.exe`.
- S1 (rev 2): store, matcher, placeholders, import/export (`src/quickprompts.*`); listener and paste (`src/quickprompts_win.*`); the window, the welcome card and the Try it box (`src/quickpromptsui.*`); Settings › QuickPrompts with an Action row (`settingsui.*`); commands, hotkeys, tray, startup and the timer that picks up file changes (`main.cpp`); `list_quick_prompts` (`agent.cpp`); `[QuickPrompts]` keys (`settings.*`). `build.bat` → `Built build\AtherScreenshot.exe`, no `warning C`.
- S2 (rev 2): `test.bat --nobuild quickprompts` → `11 tests, 213 checks, 0 failed`.
- S3 (rev 2): full `test.bat --nobuild` → `159 tests, 2696 checks, 2 failed`; the two (`video_editor_loop`, `video_editor_copies_the_frame_on_screen`) fail the same way on unmodified `main` (F-3).
- S3b (rev 3): full run on the final build → `159 tests, 2694 checks, 5 failed`: the two from F-3, plus `video_editor_timeline_zoom`, `video_editor_filmstrip_shows_frames_under_each_tile` and `video_editor_pixel_magnifier`, which passed in S3 and pass alone (`1 tests, 23 checks, 0 failed`, `1 tests, 15 checks, 0 failed`, `1 tests, 21 checks, 0 failed`): UI timing with other windows on screen. After the last fix, `test.bat --nobuild quickprompts` → `11 tests, 210 checks, 0 failed` (3 fewer checks: no `ATHER_TEST_OUT`, so no snapshot saves).
- S4 (rev 3): end to end. `AtherScreenshot.exe --quickprompts-dev 40` (a developer mode that runs only QuickPrompts and, unlike the app, counts software-typed keys so a script can type) beside the owner's running copy; `proof/e2e.ps1` types into a WinForms text box in another process. `Hello doitforme ` → the full `doitforme` starter, `ENTERS=0` (`proof/e2e-typed-doitforme.txt`); `redoitforme and doitfor ` → unchanged (`proof/e2e-no-expansion.txt`).

## Owner-level calls

- **The page is its own window** ("QuickPrompts", from the palette, the tray or Settings › QuickPrompts › Open QuickPrompts), with the switch, the excluded apps and the team file also in Settings. Long prompts don't fit a settings row. Considered: everything inside the Settings window (rejected: no room for the editor).
- **The Space after the trigger is not put back** after the prompt (D4): prompts end where the person keeps typing or sends.
- **Starter wording** is the rev 2 draft, tightened; the owner edits it before a public release.
- **Backspace-to-undo** (D4, nice-to-have) isn't built.
- **Version:** not changed (0.0.2 / 0.0.2.7). The team gets it from the staging pre-release; a numbered release is the owner's call.
