# Capture Tray: Captures Collect in a Thumbnail Strip in the Editor; Save the Ones You Pick Together

- Rev: 1
- Status: active
- Area: Editor
- Owner: AskTinNguyen
- Issue: #9
- Skill: `none`
- Branch: `intent/capture-tray`
- Started: 2026-10-21

## Goal

From the team's wishlist (L-1, the Art team's wish), with a Snagit editor screenshot as the reference: take many screenshots and manage them in a thumbnail strip.
- **Joins the strip:** every capture is added to a thumbnail strip along the bottom of the editor.
- **Kept for the session:** captures stay with the session, so people can keep capturing without saving each one right away.
- **Open and edit:** the strip shows the captures; pick one to open and annotate it, switch to another, or keep capturing.
- **Save together:** when done, pick the pictures to keep and save them all at once.

What the app does today (from the code, 2026-10-21):
- **Capture and annotate:** Capture and annotate (Ctrl+Alt+E), and After capture = "Open in editor" (`[Capture] AfterCapture=edit`), open a **new** editor window per capture (`Deliver` → `OpenEditor` in `src/main.cpp`). "Several can be open at once" (`src/editor.h`). The picture isn't saved until Save, Done or Enter.
- **Other captures:** these are saved to the captures folder straight away (`saveToFile`).
- **Per-picture state:** one `Editor` (`src/editor.cpp`) holds one picture's document: `base_`, `source_`, `st_` (`DocState`: annotations, step counter, crop and canvas, fill), `undo_`/`redo_`, `dirty_`, `version_`, and the derived `pixel_`/`committed_`. Collage is a special document.
- **Mac:** `Editor` in `macos/Sources/AtherScreenshot/Editor.swift`, one window per picture.

## Non-Goals

- **Keeping unsaved captures across a quit or a crash.** Closing asks first (D5); restoring after a crash is a follow-up.
- **Videos or GIFs in the tray.** Recordings keep opening the video editor.
- **Sending already-saved captures to the tray**, for any capture that isn't headed for the editor. The gallery already manages saved captures.
- **Reordering, renaming or tagging inside the tray.** Saved pictures get those in the gallery.
- **Changing collages.** A collage stays its own editor window.

## Decisions

These are the orchestrator's proposed defaults (rev 1). Looks, wording and keys are owner calls by the contract. The owner asked to build everything (L-1), so the worker builds them as written; the owner may change any of them by saying so.

- **D1 (rev 1): which captures join the tray.**
  - Every capture headed for the annotation editor joins the tray of the most recently used editor window (not a collage), instead of opening a new window. That's Capture and annotate, or any capture with After capture = "Open in editor".
  - The new picture becomes the current one, and the window comes to the front.
  - With no editor open, the first capture opens one as today.
  - **Setting:** Settings › Capture › **Collect captures in one editor** (`[Editor] Tray=1`, on by default). Off brings back one window per capture.
  - Opening a file from the gallery or Explorer still opens its own window, so a saved file isn't mixed with session captures.
  - For the Art team's flow (every capture into the tray), set After capture to "Open in editor". The docs say so.
- **D2 (rev 1): the strip.**
  - **Placement:** a strip of thumbnails across the editor's bottom edge, above the status bar, shown once the editor holds **two or more** pictures. A single capture looks exactly as today.
  - **Look:** after the reference, a dark strip.
    - Each thumbnail is about 120×68 px (DPI-scaled), keeps its picture's shape, and has the current picture outlined in the accent color.
    - Each one has a **tick** for "save this", a dot for unsaved changes, a "saved" badge once saved, and **×** on hover to remove it.
  - **Ticks:** new captures come ticked.
  - **Switching:** a click on a thumbnail switches to it. **Ctrl+Tab** / **Ctrl+Shift+Tab** (Ctrl+PageDown / Ctrl+PageUp too) go to the next or previous one.
  - **Scrolling:** the strip scrolls sideways with the wheel when it's full.
  - **Count:** the strip's right end shows the count ("4 pictures · 3 selected") and the **Save selected (3)** button.
- **D3 (rev 1): each picture keeps its own document.**
  - That's annotations, step numbers, crop and canvas, fill, undo and redo, the source (app and window it came from), and dirty and saved state.
  - Switching away and back shows exactly the same picture, and undo continues where it was.
  - Tool, color and size stay with the window (as today they stay across edits).
  - An unfinished text box is committed before switching.
- **D4 (rev 1): saving.**
  - **Save selected (n)** saves each ticked picture with its annotations flattened, as Save would, as a new PNG in the captures folder. Each one is noted in the Library with its app and window, and then shows the saved badge.
  - Unticked pictures are untouched.
  - Ctrl+S, Copy (Ctrl+C), Pin and Styled export still act on the current picture.
  - **Done (Enter) with a tray:**
    - It saves the ticked pictures, copies the current one, and closes.
    - It asks first if an unticked picture has unsaved changes or was never saved.
  - A picture saved and then edited again is dirty again; saving it again writes a new file, as Save does today.
- **D5 (rev 1): closing.**
  - Closing the window (or Esc on an empty selection, where that closes today) with unsaved pictures asks: "3 pictures aren't saved" with **Save selected and close**, **Close without saving** and **Cancel**.
  - The "Close without saving" choice is never the default button.
- **D6 (rev 1): memory.**
  - Up to 100 pictures per tray. The 101st asks to save or remove some first.
  - Pictures that aren't current shouldn't keep large derived bitmaps around.
  - Measured: 40 captures of 2560×1440 in one tray keep the process's private bytes under about 1.5 GB. The worker may keep inactive bases as PNG in memory or in a temp folder, which is deleted when the window closes; that's its call.
- **D7 (rev 1): Windows first, then the Mac.**
  - Windows gates run on GitHub's Windows runner (`.github/workflows/check.yml`, on every push to `intent/**`). The run's URL and the self-test result line are the evidence for `gate: test.bat` and `build`.
  - MinGW (`x86_64-w64-mingw32-g++ -std=c++20 -fsyntax-only -DUNICODE -D_UNICODE -Dsmall=char`) is a quick local type-check, never a proof.
  - The Mac part (⌘ for Ctrl, the same strip and saving) is proven with `gate: mac` on this Mac.
- **D8 (rev 1): no waiting.** Further owner-level calls (wording, look, details the spec leaves open) are the worker's. It picks what fits the app's existing style and records each, with the options it considered, in `progress.md` under "Owner-level calls". Findings are still written for anything that changes the intent.

## Acceptance

- **A1: routing (Windows).**
  - With an editor open and Tray on, a capture headed for the editor adds a picture to that editor (the editor count stays the same) and makes it current.
  - With Tray off, a new window opens as before.
  - A collage window never receives captures; a file opened from the gallery opens its own window.
  - Proof: `gate: test.bat`.
- **A2: per-picture state (Windows).**
  - Draw on picture 1, switch to 2 and draw, switch back: picture 1's composed pixels and its undo stack are what they were, and the same holds for 2.
  - Crop and canvas changes stay with their picture.
  - Undo after switching back undoes picture 1's last step.
  - Proof: `gate: test.bat`.
- **A3: Save selected and Done (Windows).**
  - Save selected writes one PNG per ticked picture, pixel-identical to what Save writes for it, and notes each in the Library. Unticked ones are untouched, and saved badges and dirty dots update.
  - Done saves the ticked ones and closes.
  - The close prompt appears when an unsaved picture would be lost; its three answers each do what D5 says.
  - Proof: `gate: test.bat`, with the prompt's answer injected.
- **A4: strip behavior (Windows).**
  - Shown only from two pictures.
  - Click, × and Ctrl+Tab/Ctrl+Shift+Tab work; the wheel scrolls.
  - The 100-picture limit holds.
  - The memory bound from D6 is measured in a test.
  - Proof: `gate: test.bat`.
- **A5: visual proof (Windows).** The editor with a tray of five captures: the current one outlined, one unticked, one saved, one with unsaved changes, × on hover, and Save selected (n).
  - Proof: `snapshot`, from `--editor-snapshots` on the runner, opened by the worker and named in `progress.md`.
- **A6: Mac.**
  - The same routing, per-picture state, Save selected, Done and the close prompt in the Mac editor (⌘S, ⌘C, ⌃Tab/⌃⇧Tab or ⌘} / ⌘{ to switch).
  - Proof: `gate: mac`, with new XCTests mirroring A1–A4, plus a `snapshot` of the Mac strip (`ATHER_TEST_OUT`).
- **A7: docs.**
  - README's Annotation editor guide gets a "Capture tray" part (including "set After capture to Open in editor to collect every capture"), the new setting, and the keys. So does `macos/README.md`.
  - `RELEASE_NOTES.md` gets a line under the next update.
  - Proof: `review`.
- **A8: the Art team's flow.** On a real PC: capture eight pictures, annotate three, switch around, save five together. All five are in the gallery with their annotations.
  - Proof: `owner`.

## Constraints

- **The contract's Never list:**
  - no owner screenshots in git; tests make their own pictures;
  - never touch the real support folder;
  - no releases, tags, `latest.json` or version changes;
  - no third-party dependencies;
  - don't stop the owner's running app.
- **One picture stays as it is.** An editor with a single picture must look and behave exactly as today: the existing editor tests and `--editor-snapshots` states stay as they are.
- **What Save writes is unchanged:** the same pixels, names and Library entries as today.
- **Shared files:**
  - `README.md`, `RELEASE_NOTES.md`, `macos/README.md`, `src/main.cpp`, `src/settings.*`, `src/settingsui.cpp` and the Mac `App.swift`, `Settings.swift` and `SettingsWindow.swift` are also touched by other intents. Keep hunks small and self-contained.
  - Most of the work is in `src/editor.cpp` / `Editor.swift`, which no other intent touches.

## Changelog

- rev 1 (2026-10-21): created from L-1 (issue #9), with the orchestrator's proposed defaults D1–D8.
