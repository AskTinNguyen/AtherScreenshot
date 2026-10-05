# Handoff: three Windows features for the macOS app

The Windows build gained these features after 0.0.1. This brings them to the Mac, written the Mac way. The Windows code is the reference this time: when this note and the C++ disagree, the C++ is right. Keep the version at 0.0.1, add no third-party dependencies, keep the settings key names, and keep the product quiet: controls show only when needed.

| # | Feature | Windows reference |
|---|---|---|
| 1 | Game controller overlay in recordings | `src/gamepad.h/.cpp`, `recorder.cpp` (frame loop), `settings.cpp`, `settingsui.cpp` |
| 2 | Edit pictures and videos from anywhere | `main.cpp` (`OpenFileToEdit`, `RunCli`), `installer.cpp` (`RegisterFileTypes`), `videoio.cpp` / `media.cpp` (rotation) |
| 3 | Join several videos in the video editor | `videoedit.h/.cpp` (`Clip`, `ApplyClips`), `videoio.h/.cpp` (`SequenceReader`, `SequenceAudio`, `SequencePlayer`), `videoeditor.cpp` (clip lane) |
| 4 | In-app updates | `src/updater.h/.cpp`, `main.cpp` (`CheckForUpdates`, `InstallUpdate`), `package.bat` (`latest.json`) |

## 1. Game controller overlay

- **Settings:** `[Recording] ShowGamepad=0` (off by default), `GamepadCorner=bottomright` (`topleft | topright | bottomleft | bottomright`) and `GamepadOpacity=100` (percent, 10–100). Settings › Recording has a toggle, a corner picker and an opacity field, and there's a palette command "Show game controller in recordings" (`ToggleShowGamepad`).
- **Input:** the GameController framework (`GCController`, `extendedGamepad`). It covers Xbox, PlayStation and MFi pads natively, so the Mac does better than the Windows XInput version here. Use the first connected controller. Draw nothing while none is connected.
- **Sampling:** poll at about 250 Hz, or use the `valueChangedHandler` callbacks, and **latch presses between frames**. Every button seen down since the last frame shows as down in the next frame, and the triggers show their highest value since then. A tap shorter than a frame must still appear (`PadLatch` in `gamepad.cpp`; its test is `gamepad_latch_keeps_a_tap_between_frames`).
- **Drawing:** burned into the recorded frames only, not shown on screen. Draw it where the click and key overlays are drawn.
  - The design box is 240 × 172 units: the outline reaches y = 170 at the grips. It is 220 pt wide at most, never more than a third of the frame's width or half its height, with a 16 pt margin (`GamepadLayout`).
  - The look matches the white modern controller in the reference photo. Every position and size is in `PaintPad` in `gamepad.cpp`, measured from the photo at about 0.495 units per photo pixel; copy them from there:
    - **Shell:** white, with soft shading just inside the edge, a light shadow and a fine dark rim, so it reads on light and dark video.
    - **Sticks:** the left stick is high on the left, the right stick low right of center. Each has an orange LED ring (`#FFA02E`) fixed in the shell with a soft glow, a dark gap, and a black domed cap that tilts up to 4.5 units with the stick. Clicking the stick turns the ring nearly white and makes the glow stronger.
    - **D-pad:** low on the left, on a light round plate. It's glossy black with arrow notches and a raised middle; the pressed arm lights orange.
    - **A, B, X and Y:** high on the right, glossy black, with letters in their colors (A `#2EB84A`, B `#E82C38`, X `#2A84F2`, Y `#F4CC14`). When pressed, a button fills with its color and glows.
    - **Middle:** View and Menu are small white buttons on either side of a home button. Below them are two more small buttons with a dot between them, a pill and three status lights. View and Menu light orange when pressed.
    - **Bumpers and triggers:** the bumpers are thin white bands along the shoulders and light orange when pressed. Triggers aren't visible at rest; pulling one makes an orange tab rise behind its shoulder, as far as it's pulled.
  - **Opacity:** draw onto a separate layer, then composite it at the chosen opacity, so overlapping parts fade together. Keep the drawn layer and reuse it while the pad state doesn't change, because drawing it costs milliseconds per frame.
  - For PlayStation pads you may relabel the face buttons as ✕○□△, keeping the same positions.

## 2. Edit pictures and videos from anywhere

- **Routing:** any file opened from outside goes to the video editor if it is a video and to the image editor otherwise.
  - Affected paths: `application(_:open:)`, the "Open…" panel and the `edit <file>` command.
  - On Windows the extensions are mp4, mov, m4v, wmv, avi and mkv. On the Mac, use whatever AVFoundation can open.
  - Pinning a video says "Videos can't be pinned" instead of failing.
  - Today `App.swift` sends every opened URL to `Editor.open(url:)`, so videos need the same check that the recents menu already does.
- **Finder integration:** declare `CFBundleDocumentTypes` for public.image and public.movie (or the specific UTIs) with `LSHandlerRank = Alternate`. The app then appears in "Open With" without taking over as the default. The Windows equivalent is "Open with" plus an "Edit with Ather Screenshot" right-click item, registered per user and removed on uninstall.
- **Saving:** edits are saved as new files in the captures folder (`NoteEdit` links them to the source). The original file is never changed.
- **Rotation:** phone videos stored sideways with a rotation must read upright everywhere: preview, export, thumbnails and size. On Windows only the preview applied it, so export came out sideways and the markup landed in the wrong place. On the Mac, check that the export and frame-grab paths apply the track's `preferredTransform` the way `AVPlayer` does. The Windows test is `video_rotated_phone_clip_reads_upright_like_the_preview`.

## 3. Join several videos (clip sequence)

The scope is deliberately small: clips play back to back. There are no transitions and no extra tracks.

- **Model:**
  - `VideoEdit.clips` is a list of `Clip { id, path, in, out, length, w, h, fps, hasAudio }`.
  - `frameW`/`frameH` are the sequence frame size. That is the first video's size, and it stays the same even if that video is moved or removed.
  - Timeline time is the clips laid end to end. Every existing time (trim, marks, captions, word times) is timeline time.
  - Clips are part of the edit, so undo covers them.
- **Retiming (`ApplyClips`), the same rules on both platforms:**
  - An item stays on the same footage: the same file at the same source time.
  - It looks for a clip that still shows that moment, preferring the same clip id. This is what keeps everything in place across a split.
  - If none does, the item falls back to the same clip, clamped. It is dropped when its clip is gone, or when both its ends were cut off on the same side.
  - Items keep their length; caption word times shift with the caption.
  - An untrimmed trim end follows the new total. Otherwise both trim ends follow their footage, and if they cross, the trim resets to the whole sequence.
  - The cases are covered by the tests `video_clip_changes_move_items_with_their_footage` and `video_clips_locate_and_total`. Port them.
- **Media:**
  - Build an `AVMutableComposition` that inserts each clip's `[in, out)` range in order.
  - Each clip is fitted into the frame with black bars, using a video composition layer instruction transform per clip, rotation included.
  - Audio: clips without sound are silent, and sync must hold across them.
  - The preview `AVPlayer`, export, GIF, thumbnails, frame grabs and auto captions all read the composition. Auto captions transcribe the composition's audio for the trimmed range.
  - Windows had to build this by hand (`SequenceReader`, `SequenceAudio`, `SequencePlayer`). AVFoundation does most of it for you.
- **UI (hide until needed):**
  - Add ▾ › "Video clip…" (⌘O) and dropping video files on the editor add the clips after the selected clip.
  - The **clip lane** above the thumbnail strip appears only with two or more clips. Each bar shows the file name and length. Click selects the clip, dragging reorders it (with an insertion marker), and dragging a selected clip's edge trims it, applied on mouse-up. Cut lines appear across the thumbnails.
  - The selected clip's inspector row reads "Clip 2 of 3: name · 0:01.0" and offers Split at playhead (S), Earlier, Later and Remove (Delete). The last clip can't be removed.
  - The hint row mentions "S split · drop videos to join them".
- **Tests to mirror:** `video_sequence_joins_clips_into_one_video` (export of mixed sizes and sound), `video_editor_joins_splits_and_reorders_clips` and `video_editor_clip_lane_mouse`.

## 4. In-app updates

- **Manifest:** `downloads/latest.json` on GitHub (`raw.githubusercontent.com/AskTinNguyen/AtherScreenshot/main/downloads/latest.json`). Windows reads `windows`. Add a `macos` entry with the same fields (`version`, `url`, `sha256`, `size`, `notes`), written by `build.sh package`.
- **Behavior (match Windows):**
  - Check a minute after launch and then daily, unless `[Updates] CheckAutomatically` is off. There's also a "Check for updates…" command.
  - Announce each new version once with a notification; the menu-bar menu shows "Update to version …".
  - Install only on a click, and never during a recording or with an editor open.
  - Download only over HTTPS from this repository. Verify the size and SHA-256, and check that the bundle's `CFBundleShortVersionString` matches the manifest.
  - Replace the app bundle in place, relaunch, and show "Updated to …". On any failure, keep the current version and offer the download page.
- **No new dependencies:** Sparkle would be a third-party dependency, so do it by hand. Download the zip, unzip it with `ditto`, and check `codesign --verify` against the running app's team identifier once the builds are signed. Swap the bundle through a short helper that waits for the app to quit.

