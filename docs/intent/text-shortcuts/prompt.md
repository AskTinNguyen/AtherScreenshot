# QuickPrompts: Type a Short Trigger, Get Your Saved Prompt or Phrase

- Rev: 3
- Status: active
- Area: App
- Owner: AskTinNguyen
- Issue: none yet
- Skill: `none`
- Branch: `intent/text-shortcuts`
- Started: 2026-11-14

Rev 3 (2026-11-14, from L-3 and findings F-1, F-2): built for Windows. The prompts file is `quickprompts.json`; typed
keys come through Raw Input on a thread of its own instead of a keyboard hook (F-1); the Mac part is a follow-up (F-2).
Where a decision below still says `snippets.json`, `[Snippets]` or "hook", read `quickprompts.json`, `[QuickPrompts]`
and "listener".

## Goal

From the owner (2026-11-14): let people save phrases and AI-agent prompts they use often, and insert them anywhere by typing a short trigger, like macOS Text Replacements. Typing `doitforme` (then Space) in any app replaces it with the long prompt stored for it.

- **Save:** a list of shortcuts, each a trigger (`doitforme`) and a text (any length, many lines).
- **Type to expand:** typing a trigger in any app, followed by Space, replaces it with the text.
- **Pick instead of type:** a hotkey opens a searchable list of the saved texts and inserts the chosen one, for apps where typing a trigger doesn't work, and for texts whose trigger people forget.
- **Tie-in with captures:** a text can carry the last screenshot's path, or paste the screenshot itself after the text, so "describe this bug" prompts go to an AI chat with the picture in one step.

### Why it belongs in this app

- The app already lives in the tray all day, has rebindable global hotkeys (`kCmds` + `RegisterHotKey`, `src/main.cpp`), a fuzzy command palette that can list anything (`ShowPalette`, `src/palette.h`), a keyboard hook (`src/inputviz.cpp`, only while recording), clipboard helpers, a JSON reader/writer (`src/json.cpp`) and an MCP server for agents. Most of the parts exist.
- Its users already work with AI agents (README › AI agents). A prompt library next to the screenshot tool fits that flow: capture → `fixthis` → the prompt and the picture land in Claude/ChatGPT/Codex.
- macOS Text Replacements don't work in many apps (Electron apps, terminals, most browsers' web apps, JetBrains), don't exist on Windows, can't hold placeholders, and multi-line texts are awkward. This works everywhere, the same on both platforms, from one file.

### What the app does today (from the code, 2026-11-14)

- **Hotkeys:** each `CmdDef` in `kCmds` (`src/main.cpp:134`) has an id, an ini key, a title, keywords and a default; `RegisterHotkeys` registers them with `RegisterHotKey`. The Settings window lists them under "Shortcuts" (`src/settingsui.cpp:232`). Mac: Carbon `RegisterEventHotKey` (`macos/.../Hotkeys.swift`), no Accessibility needed.
- **Keyboard hook:** `WH_KEYBOARD_LL` in `src/inputviz.cpp`, installed only while a recording shows keystrokes. Mac: `NSEvent.addGlobalMonitorForEvents(.keyDown)` with Accessibility (`Recorder.swift:778`).
- **Settings:** `%APPDATA%\AtherScreenshot\settings.ini` (UTF-16 INI). Not a good fit for multi-line texts.
- **Typing into other apps:** only `SendInput` for scroll events (`src/scroll.cpp`). Nothing types or pastes into another app today.

## Non-Goals

- **Rich text, images or files as the stored text.** Plain text only (the screenshot tie-in pastes the capture, not a stored image). Rich text is a follow-up if asked.
- **Cloud sync or sharing between people.** The file is plain JSON; people can sync the folder or share the file themselves. Import/export is in scope.
- **Fill-in forms** (a text with blanks that asks for values before inserting, like TextExpander's fill-ins). Follow-up.
- **Scripts or shell commands as placeholders.** Never; a text can't run anything.
- **Expanding inside games** in fullscreen-exclusive mode. Excluded by default (see D7).

## Owner answers (2026-11-14)

Recorded verbatim in `log.md` when the intent starts. They override anything below that disagrees.

- **Q1: trigger style → expand after a Space.** A trigger expands only when followed by Space. Enter, Tab and punctuation do **not** expand (they reset the buffer), so typing a trigger word and pressing Enter in a chat never sends a half-expanded message. No "expand immediately" mode in this intent.
- **Q2: newlines → always paste.** Every expansion is inserted by pasting through the clipboard; nothing is typed character by character. No `PasteThreshold` setting.
- **Q3: the keyboard hook → on from first launch, changeable in Settings.** The first time the app opens (after this update, for existing users too), a one-time welcome card introduces QuickPrompts, says plainly what the listener does, and turns typed triggers on. Settings › QuickPrompts › **Expand triggers as I type** turns it off (and back on) at any time, and the hook is uninstalled while it's off.
- **Q4: UI → a QuickPrompts page, with pre-made prompts for team members.** A **QuickPrompts** page in Settings holds the list, the editor and the on/off switch. It ships with a set of pre-made prompts (D11) that every team member gets, and a team can share its own set (D12).

## Decisions

Rev 1 defaults, with rev 2 changes from the owner's answers marked "(rev 2)".

- **D1 (rev 1): storage — `snippets.json`.**
  - File: `snippets.json` in the support folder (`%APPDATA%\AtherScreenshot`, `~/Library/Application Support/AtherScreenshot`), the same shape on both platforms so one file moves between them. UTF-8, written atomically (temp + rename), reloaded when it changes on disk (as `Settings::ChangedOnDisk` does).
  - Shape:
    ```json
    {
      "version": 1,
      "snippets": [
        {
          "id": "b3f1…",
          "trigger": "doitforme",
          "title": "Do it for me",
          "text": "You are working in this repository. Read the code first…\n\n{cursor}",
          "enabled": true,
          "caseSensitive": false,
          "source": "user",
          "hotkey": "",
          "tags": ["agent"],
          "uses": 12,
          "lastUsed": "2026-11-14T09:30:00Z"
        }
      ]
    }
    ```
  - `source` (rev 2): `user` (made by this person), `builtin` (D11) or `team` (D12). Built-in and team prompts are read-only in place; "Duplicate to edit" makes a `user` copy.
  - Settings in `settings.ini` `[QuickPrompts]` (rev 2): `ExpandAsYouType` (1 once the welcome card has run, see Q3), `WelcomeShown` (0 until the card has shown once), `ExcludedApps` (`;`-separated exe names / bundle ids), `TeamFile` (D12, empty). The picker's hotkey lives with the other shortcuts in `[Hotkeys] InsertQuickPrompt` (`Ctrl+Alt+Space`, Mac `⌃⌥Space`).
  - Rules: a trigger is 2–32 characters with no whitespace; triggers are unique (case-insensitive unless `caseSensitive`); a trigger that is the end of another trigger is warned about in the editor (`fix` vs `quickfix`) — the longer one wins.
- **D2 (rev 1): the matcher is pure code, separate from the hook.**
  - `src/snippets.cpp/.h`: a `TriggerMatcher` that takes events (`Char(c)`, `Backspace`, `Space`, `Reset`) and returns either nothing or an action `{erase: n, snippet: id}`. No Win32 inside, so it's unit-tested in `selftest` with plain sequences.
  - Only Space completes a trigger (rev 2, Q1). Enter, Tab and punctuation are `Reset`.
  - Buffer: the last 64 characters typed. Word boundary: the trigger matches only at the start of the buffer or after a non-letter/digit, so `redoitforme` doesn't fire.
  - Reset on: focus change to another window (`SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`), a mouse click (caret may have moved), arrows/Home/End/PgUp/PgDn, Esc, any Ctrl/Alt/Win combination (except AltGr characters), and after an expansion.
  - Backspace removes the last buffered character.
- **D3 (rev 3): the listener (Windows).** Rev 3: Raw Input (`RIDEV_INPUTSINK`, keyboard and mouse buttons) on its own thread with a message-only window, not `WH_KEYBOARD_LL` (F-1). Keys without a device (anything `SendInput` typed) are ignored. A match is posted to the UI thread with the key count at that moment; the paste gives up if any key was pressed since. The rev 1 text below stays for the record.
- **D3 (rev 1): the hook (Windows).**
  - A second `WH_KEYBOARD_LL` on the UI thread, installed only while `[QuickPrompts] ExpandAsYouType=1`, independent of the recording one. Turning the setting off uninstalls it at once; the picker keeps working.
  - The hook callback does only this: ignore injected events (`LLKHF_INJECTED`, which also covers the app's own typing and Vietnamese/IME tools such as Unikey/EVKey that inject their output), translate the key to a character, feed the matcher, and on a match `PostMessage` the action to the main window. It never blocks or swallows the key. Windows silently removes a hook that takes longer than `LowLevelHooksTimeout`, so all real work happens in the posted message.
  - Character translation: `ToUnicodeEx` with the foreground thread's layout and flag `0x4` (don't change keyboard state, Windows 10 1607+), so dead keys and accents in the person's real typing aren't disturbed. Any dead-key or IME composition in progress resets the buffer.
  - Known limits, documented: no expansion in windows running as administrator (UIPI blocks both seeing and typing into them) or on the secure desktop; the picker works there neither.
- **D4 (rev 1): inserting the text.**
  - Erase: `SendInput` Backspace × (trigger length + 1 for the Space).
  - Insert, **always paste** (rev 2, Q2): save the clipboard (every format that can be read and re-set), put the expanded text on it, send Ctrl+V (Mac ⌘V), restore the old clipboard ~400 ms later. The temporary entry is marked so Windows clipboard history and cloud clipboard skip it (`CanIncludeInClipboardHistory` = 0, `ExcludeClipboardContentFromMonitorProcessing`). Nothing is ever typed character by character, so a newline can never act as Enter.
  - Don't re-add the Space after the text: prompts usually end where the person keeps typing or sends. (Owner may flip this; it's one line.)
  - Paste keys per app: Ctrl+V by default; Ctrl+Shift+V for terminals that need it (`WindowsTerminal.exe`, `mintty.exe`, `alacritty.exe`, `wezterm-gui.exe`). Apps in `ExcludedApps` never expand.
  - Undo: pressing Backspace right after an expansion (before any other key) puts the trigger back, as macOS does with autocorrect. Nice-to-have; drop it if it gets complicated.
- **D5 (rev 1): the picker (no hook needed).**
  - New command `InsertQuickPrompt` ("Insert QuickPrompt…", default `Ctrl+Alt+Space`) in `kCmds`. It remembers the foreground window, opens the existing palette with the prompts (title, trigger, first line of text; searched across all three; built-in and team prompts marked), and on pick restores focus to that window and pastes as in D4.
  - Palette also gets `NewQuickPrompt` ("New QuickPrompt…"), `NewQuickPromptFromClipboard` ("Save clipboard as a QuickPrompt…") and `OpenQuickPrompts` ("QuickPrompts…", opens the page).
  - Each snippet may have its own global `hotkey` (reuses `ParseHotkey`/`RegisterHotKey`; conflicts shown like the other shortcuts' status). This covers the owner's "sequence of hotkeys" wish without a hook.
- **D6 (rev 1): placeholders.**
  - `{cursor}` — where the caret ends up (sends ← the right number of times after pasting).
  - `{clipboard}` — the clipboard's text at the moment of expanding.
  - `{date}`, `{time}`, `{datetime}` — local, ISO-ish; `{date:yyyy-MM-dd}` with the same tokens as the file-name template.
  - `{lastcapture}` — the path of the last capture (the app already tracks it for "Copy last capture" etc.).
  - `{lastcapture:image}` — after the text, paste the last capture as a picture (a second clipboard swap + paste). This is the screenshot tie-in: `bugpls` → prompt + the screenshot in the AI chat.
  - `{{` and `}}` for literal braces. Unknown placeholders are left as typed and underlined in the editor.
- **D7 (rev 1): where it never expands.**
  - The app's own windows (the editor's text tool, Settings — so editing a snippet never triggers one).
  - Apps in `ExcludedApps`. Default: none, but the settings page offers a one-click "add current app" via the tray menu.
  - Fullscreen apps (games), with "Don't expand in fullscreen apps" on by default, since a wrong expansion in a game would go unnoticed. Recording with the key overlay doesn't conflict: both hooks only listen.
  - Password fields can't be detected reliably on Windows; documented. On the Mac, `IsSecureEventInputEnabled()` is checked and the buffer is cleared while secure input is on.
- **D8 (rev 1): MCP.** Read-only `list_snippets` (trigger, title, text) so agents can see the person's prompt library. No tool to create, change or expand snippets in rev 1, and never anything that exposes the typed-character buffer.
- **D9 (rev 1): import/export.** Import Apple's Text Replacements (the `.plist` macOS writes when you drag replacements out of System Settings) and a plain CSV (`trigger,text`). Export to `snippets.json` and CSV. Existing triggers are kept; duplicates are listed for the person to choose.
- **D10 (rev 1): the Mac.** Same file, same matcher logic (ported to Swift with the same self-test sequences), `CGEventTap` (listen-only) or the existing `NSEvent` global monitor for keys, `CGEvent` posting for Backspace and ⌘V. Needs Accessibility, as keystroke overlays already do; the settings page shows the permission state. Built and proven on a Mac only.
- **D11 (rev 2): pre-made prompts.**
  - Compiled into the app (a small table in `src/snippets.cpp`, mirrored in Swift), `source: "builtin"`, each with a stable id so updates can change their text without duplicating them.
  - Shown in their own **Team starters** group at the top of the page. Each can be turned off (kept in `snippets.json` as `{"id": …, "enabled": false}`) or duplicated to edit; their text can't be edited in place, so an app update can improve them.
  - Triggers are made-up words that never occur in normal typing, all ending in `pls` or `me`, so they're easy to remember and never fire by accident.
  - Starter set (the owner edits the wording before release; this is a draft):

    | Trigger | Title | What the text asks |
    | --- | --- | --- |
    | `doitforme` | Do it for me | Read the relevant code first, make the change end to end, run the tests, and report what changed and what wasn't verified. |
    | `planpls` | Plan first | Before writing code, propose a short plan with the files to touch and the risks, then wait for a go-ahead. |
    | `reviewpls` | Review this change | Review the current diff for bugs, edge cases and missing tests; list findings by severity with file:line. |
    | `explainpls` | Explain this | Explain what this code or screenshot shows, step by step, for someone new to the codebase. |
    | `bugpls` | Describe this bug | Turn the attached screenshot `{lastcapture:image}` into a bug report: steps, expected, actual, severity. |
    | `fixthispls` | Fix from screenshot | Look at the attached screenshot `{lastcapture:image}`, find the cause in the code, and fix it. |
    | `testpls` | Write tests | Write tests for this change covering the normal path and edge cases; run them and report results. |
    | `shorterpls` | Make it shorter | Rewrite the text above to half the length, keeping every fact. |
    | `vienpls` | Translate VI ↔ EN | Translate between Vietnamese and English, keeping technical terms and tone. |
    | `commitpls` | Commit message | Write a commit message for the staged changes: one summary line under 72 characters, then why. |
    | `playtestpls` | Playtest notes | Summarise these playtest notes into issues, each with repro steps, frequency and a suggested owner. |
    | `standupme` | Stand-up | Draft my stand-up from these notes: yesterday, today, blockers. |
- **D12 (rev 2): team prompts.**
  - Settings › QuickPrompts › **Team prompts file**: a path to a `quickprompts-team.json` (same shape as `snippets.json`) on a shared drive, a synced folder (OneDrive, Google Drive, Dropbox) or inside a repo. The app reads it on start and when it changes; its prompts show in a **Team** group, `source: "team"`, read-only, with "Duplicate to edit".
  - "Export selected as team file…" on the page writes such a file from the person's own prompts, so whoever curates the team's set builds it in the app.
  - A trigger used by both a team prompt and a person's own prompt: the person's own wins, and the page shows the clash.
  - No downloading from URLs in this intent (no network fetch, no auth); a synced folder covers it. URL feeds are a follow-up if asked.
- **D13 (rev 2): the QuickPrompts page and the welcome card.**
  - **Page:** Settings › **QuickPrompts**. Top: the **Expand triggers as I type** switch, the picker's hotkey, Excluded apps, Team prompts file. Below: a searchable list in three groups (Team starters, Team, My prompts) showing trigger, title, first line and use count; **New**, **Duplicate**, **Delete**, **Import…** (Apple `.plist`, CSV, JSON), **Export…**.
  - **Editor:** selecting a prompt opens it in a resizable editor (a pane on the page if it fits the settings window's layout, otherwise its own window; the worker picks and records it): trigger, title, multi-line text with a monospace option, placeholder buttons (`{cursor}`, `{clipboard}`, `{date}`, `{lastcapture}`, `{lastcapture:image}`), tags, own hotkey, and a live **Try it** box where typing the trigger and Space expands it, so people can test without leaving the page.
  - **Welcome card (Q3):** shown once, the first time the app runs with this feature (`[QuickPrompts] WelcomeShown=0`), after the install prompt if there is one. It says what QuickPrompts does, lists three starters (`doitforme`, `reviewpls`, `bugpls`), says in one sentence that the app listens for triggers as you type and keeps only the last few characters in memory (never saved, logged or shared with AI agents), and turns typing triggers on. Buttons: **Got it** (keeps it on) and **Open QuickPrompts** (keeps it on and opens the page). It notes where to turn it off.

## Phases

| Phase | What ships | Rough size |
| --- | --- | --- |
| P1 | `snippets.json`, the QuickPrompts page and editor, the built-in starters (D11), the picker command, per-prompt hotkeys, paste insertion | ~900 lines C++ |
| P2 | Typed triggers: hook, matcher (Space only), resets, exclusions, the welcome card that turns them on | ~600 lines + tests |
| P3 | Placeholders (`{cursor}`, `{clipboard}`, dates, `{lastcapture}`, `{lastcapture:image}`), team prompts file (D12), import/export, `list_snippets` MCP tool | ~600 lines |
| P4 | macOS parity | ~1000 lines Swift |

P1 to P3 are one intent on Windows and ship together in one update, since the welcome card turns typed triggers on at first launch (Q3) and the starters `bugpls` and `fixthispls` need `{lastcapture:image}`. P4 follows on a Mac. Each phase is its own PR so the owner can review them one at a time.

## Acceptance

- **A1: matcher (Windows).** Self-tests feed key sequences and check actions: `doitforme␣` erases 10 characters and gives the prompt; `doitforme⏎`, `doitforme⇥` and `doitforme.` give nothing; `redoitforme␣` gives nothing; `doitfoX⌫rme␣` matches; a focus reset mid-trigger gives nothing; `fix`/`quickfix` gives the longer one; case rules hold. Proof: `gate: test.bat`.
- **A2: storage.** Round-trip of `snippets.json` with multi-line, Unicode (Vietnamese with tone marks, emoji) and brace texts; a hand-edited file is reloaded; an invalid file is never overwritten (kept as `snippets.json.bad`, toast). A built-in prompt turned off stays off after a restart. Proof: `gate: test.bat`.
- **A3: insertion (Windows).** In an edit window the self-test creates, typing `doitforme ` leaves exactly the full multi-line text (no trigger, no extra space), and the clipboard afterwards holds what it held before (text and an image both tested). Proof: `gate: test.bat` and a `snapshot`.
- **A4: newline safety.** A 3-line prompt expanded into a single-line control that treats Enter as "submit" (the test makes one) never triggers submit. Proof: `gate: test.bat`.
- **A5: picker.** `InsertQuickPrompt` lists the prompts in the palette (starters, team and own, marked), search finds by trigger, title and text, and picking pastes into the window that was active before. Proof: `gate: test.bat` + `snapshot` of the palette.
- **A6: first launch and the switch.** With `WelcomeShown=0` the welcome card shows once and sets `ExpandAsYouType=1` and `WelcomeShown=1`; it doesn't show again. With the switch off no keyboard hook is installed and with it on one is (checked from the self-test); toggling takes effect without a restart. Proof: `gate: test.bat` + `snapshot` of the card.
- **A7: privacy.** The typed buffer is never written to disk, logs or MCP replies (code review + a test that types a marker string and searches the support folder and the log afterwards). Proof: `gate: test.bat` + `review`.
- **A8: placeholders.** Each placeholder in D6 expands as specified; `{lastcapture:image}` pastes a picture into a rich edit control after the text. Proof: `gate: test.bat`.
- **A9: team prompts.** A `quickprompts-team.json` set in Settings shows its prompts in the Team group, read-only; editing the file updates the list without a restart; a person's own prompt with the same trigger wins. Proof: `gate: test.bat`.
- **A10: the QuickPrompts page.** The page, its three groups, the editor and the Try it box. Proof: `snapshot`; looks are `owner`.
- **A11: Mac.** P4 parity. Proof: `gate: mac`.
- **A12: docs.** README Features row ("⌨️ QuickPrompts"), a Guide section with the starter list, a Default shortcuts row for the picker, "Platform differences", a RELEASE_NOTES line. Proof: `review`.

## Risks

- **Antivirus and trust (biggest, and larger with Q3).** An unsigned exe that installs an always-on `WH_KEYBOARD_LL` at first launch and sends input looks like a keylogger to heuristics, and Defender may quarantine the update for some people. Mitigations: the welcome card says plainly what happens and where to turn it off; the hook never swallows or stores keys; ship to the team first and watch for Defender reports before a public release; code signing (already a known gap for SmartScreen) would help most. If Defender flags it, the fallback is to make the card ask ("Turn on") instead of turning it on, which is a one-line change.
- **Wrong erasure.** If the buffer believes a trigger was typed but the caret moved (click, autocomplete, IME), Backspaces delete the wrong text. Mitigations: aggressive resets (D2), word-boundary rule, Space-only completion, Backspace-to-undo.
- **Clipboard clobbering.** Saving and restoring every clipboard format is slow for big items (a copied 4K picture, many files). Mitigation: restore every format that can be read; for clipboard contents over a size limit, keep them by delaying the paste until restored, or warn once. Measured in A3.
- **Latency for every keystroke system-wide.** The hook must stay trivial (D3); measured with a typing self-test that the callback stays well under 1 ms.
- **Vietnamese/IME input.** Telex/VNI tools inject replacement keystrokes; ignoring injected events and resetting on composition keeps triggers made of plain ASCII working. Triggers with tone marks are not supported in this intent (documented).
