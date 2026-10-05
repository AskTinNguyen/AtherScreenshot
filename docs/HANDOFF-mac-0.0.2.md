# Handoff: bring the Mac app to Windows 0.0.2

Windows 0.0.2 (build 0.0.2.1) is released: [v0.0.2](https://github.com/AskTinNguyen/AtherScreenshot/releases/tag/v0.0.2). It adds four features to 0.0.1, and a max-level code review then fixed 15 bugs in them. This note brings all of it to the Mac (`macos/`, Swift), written the Mac way: AVFoundation, GameController, AppKit. Don't port the C++ line by line.

- **Reference:** the Windows code on `main` (`src/`). When this note and the C++ disagree, the C++ is right. Read the C++ for constants and edge cases.
- **Version (the owner's rule):**
  - The Mac release is **0.0.2** (`CFBundleShortVersionString`), whatever changed.
  - The build number goes in `CFBundleVersion`, starting at `0.0.2.1`. Re-releases raise only the build, and the updater compares the build.
- **Ground rules:**
  - No third-party dependencies (no Sparkle).
  - Same settings key names, and `library.json` stays compatible both ways.
  - Keep the product quiet: controls show only when needed.
  - Tests never touch the real support folder.
- **Work in steps:** one feature at a time, each with tests and a commit, building and testing as you go (`cd macos && ./build.sh`, `xcrun swift test`). Report after each step.
- **Don't publish** a release, tag or `latest.json` change until the owner says so.

| # | Feature | Windows reference |
|---|---|---|
| 1 | Game controller overlay in recordings | `src/gamepad.h/.cpp`, `recorder.cpp` (frame loop), `settings.cpp`, `settingsui.cpp` |
| 2 | Edit pictures and videos from anywhere | `main.cpp` (`OpenFileToEdit`, `RunCli`), `installer.cpp` (`RegisterFileTypes`), `videoio.cpp` / `media.cpp` (rotation) |
| 3 | Join several videos in the video editor | `videoedit.h/.cpp` (`Clip`, `ApplyClips`), `videoio.h/.cpp` (`SequenceReader`, `SequenceAudio`, `SequencePlayer`), `videoeditor.cpp` (clip lane) |
| 4 | In-app updates | `src/updater.h/.cpp`, `src/net.h/.cpp`, `main.cpp` (`CheckForUpdates`, `UpdateBlocked`, `InstallUpdate`), `package.bat publish` |
| 5 | The review's lessons (apply them as you build 1–4) | see "What the review taught us" below |

## 1. Game controller overlay

- **Settings:** `[Recording] ShowGamepad=0` (off by default), `GamepadCorner=bottomright` (`topleft | topright | bottomleft | bottomright`) and `GamepadOpacity=100` (percent, 10–100). Settings › Recording has a toggle, a corner picker and an opacity field, and there's a palette command "Show game controller in recordings" (`ToggleShowGamepad`).
- **Input:** the GameController framework (`GCController`, `extendedGamepad`). It covers Xbox, PlayStation and MFi pads natively, so the Mac does better than the Windows XInput version here. Use the first connected controller. Draw nothing while none is connected.
- **Sampling:** poll at about 250 Hz, or use the `valueChangedHandler` callbacks, and **latch presses between frames**. Every button seen down since the last frame shows as down in the next frame, and the triggers show their highest value since then. A tap shorter than a frame must still appear (`PadLatch` in `gamepad.cpp`; its test is `gamepad_latch_keeps_a_tap_between_frames`).
- **Pauses:** while a recording is paused, keep emptying the latch (Windows calls `Take()` every paused tick). Otherwise every press made during the pause shows at once in the first frame after it.
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
- **One list of types:** keep one list of picture and video types, and have the open panels, the drop targets, the gallery's import and `CFBundleDocumentTypes` agree with it. On Windows, `MediaExtensions`/`MediaFilter` live in `library.cpp`. The gallery imports everything the editor opens, including mkv, avi and wmv where AVFoundation can read them.
- **Saving:** edits are saved as new files in the captures folder (`NoteEdit` links them to the source). The original file is never changed.
- **Rotation:** phone videos stored sideways with a rotation must read upright everywhere: preview, export, thumbnails and size. On Windows only the preview applied it, so export came out sideways and the markup landed in the wrong place. On the Mac, check that the export and frame-grab paths apply the track's `preferredTransform` the way `AVPlayer` does. The Windows test is `video_rotated_phone_clip_reads_upright_like_the_preview`.
- **Gallery sizes:** once sizes come out upright, videos the library indexed before keep their sideways width and height. Windows raised `kIndexVersion` to 2 and re-indexes only videos; pictures are left alone. If the Mac indexer stores w/h, do the same, and keep `library.json` readable by both apps.
- **Undecodable files:** only accept a video whose frames really decode. Ask for a frame, not just the metadata: an HEVC file on a machine without the codec opens fine but can't be shown. Count its sound only if the audio decodes too. Otherwise you export silent tracks, and auto captions say "No speech found" instead of "no sound".

## 3. Join several videos (clip sequence)

The scope is deliberately small: clips play back to back. There are no transitions and no extra tracks.

- **Model:**
  - `VideoEdit.clips` is a list of `Clip { id, source, path, in, out, length, w, h, fps, hasAudio }`.
  - `source` is shared by the pieces split from one added video. A second copy of the same file added separately gets its own `source`.
  - `frameW`/`frameH` are the sequence frame size. That is the first video's size, and it stays the same even if that video is moved or removed.
  - Timeline time is the clips laid end to end. Every existing time (trim, marks, captions, word times) is timeline time.
  - Clips are part of the edit, so undo covers them.
- **Retiming (`ApplyClips`), the same rules on both platforms:**
  - An item stays on the same footage: the same file at the same source time.
  - It looks for a clip with the same `source` that still shows that moment, preferring the same clip id. This is what keeps everything in place across a split. Matching on the path alone was a bug: removing one copy of a video moved its captions onto the other copy.
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
- **Behaviour the review fixed (match it):**
  - **Lane hit test:** the selected clip's edges win over the neighbouring clip's body. Otherwise you can't trim the left edge of any clip but the first.
  - **Playhead knob:** it scrubs even where it overlaps the clip lane.
  - **Mid-drag keys:** while the mouse holds a clip drag, keys don't edit (only Esc, which cancels the drag), and losing the mouse capture cancels it. Otherwise Delete or ⌘Z mid-drag pulled the clip out from under the drag and crashed.
  - **Short clips:** a clip shorter than 0.2 s keeps its own length as the trim minimum, so a click on its edge never pushes it past the end of its file.
  - **Undo while playing:** keep the playhead where playback is; don't jump back to where Play was pressed.
  - **Auto captions:** if the clips changed while transcription ran, retime the new captions from the old clip layout to the new one with the same `ApplyClips`.
  - **Clips that can't play:** skip to the next playable clip instead of stopping playback there.
  - **Splits play straight through:** consecutive pieces of the same file play without a seek. A composition does this naturally.
  - **Thumbnails:** don't letterbox or decode work that's thrown away, and cancel jobs that a newer edit made stale.
- **UI (hide until needed):**
  - Add ▾ › "Video clip…" (⌘O) and dropping video files on the editor add the clips after the selected clip.
  - The **clip lane** above the thumbnail strip appears only with two or more clips. Each bar shows the file name and length. Click selects the clip, dragging reorders it (with an insertion marker), and dragging a selected clip's edge trims it, applied on mouse-up. Cut lines appear across the thumbnails.
  - The selected clip's inspector row reads "Clip 2 of 3: name · 0:01.0" and offers Split at playhead (S), Earlier, Later and Remove (Delete). The last clip can't be removed.
  - The hint row mentions "S split · drop videos to join them".
- **Tests to mirror:**
  - `video_sequence_joins_clips_into_one_video`: export of mixed sizes and sound.
  - `video_editor_joins_splits_and_reorders_clips`.
  - `video_editor_clip_lane_mouse` and `video_editor_clip_lane_edges_knob_and_keys`.
  - The two-copies case in `video_clip_changes_move_items_with_their_footage`.

## 4. In-app updates

- **Release channel:**
  - Binaries live on GitHub Releases, not in git. Both platforms share one release per version (`v0.0.2`) and one `latest.json` asset on it.
  - The Mac reads `https://github.com/AskTinNguyen/AtherScreenshot/releases/latest/download/latest.json` and uses its `macos` entry, with the same fields as `windows`: `version` (the build, e.g. `0.0.2.1`), `url` (the release asset, a zip of the .app), `sha256`, `size` and `notes`.
  - Publishing the Mac side means three steps:
    - Upload the DMG and the update zip to the release with `gh release upload --clobber`.
    - Fetch the release's `latest.json`, set only `macos`, keep `windows`, and upload it with `--clobber`.
    - Update `downloads/latest.json` on main the same way.
  - `package.bat publish` keeps any `macos` entry it finds.
- **Behaviour (match Windows, including its fixes):**
  - **Checking:**
    - Check a minute after launch and then daily, unless `[Updates] CheckAutomatically` is off.
    - "Check for updates…" checks now and says "up to date" or why it couldn't check.
    - Compare versions as numbers (`CompareVersions`: "0.0.10" > "0.0.9", and "0.0.2" = "0.0.2.0"). Compare the manifest against the build (`CFBundleVersion`), never the shown version.
  - **Offering:**
    - Announce each new build once with a notification.
    - The menu-bar menu then shows "Update to version …", and that item must really start the update. On Windows it first didn't, because the command was missing from the command table.
  - **When to install:**
    - Install only on a click, and never during a recording, a region selection, a scrolling capture or with an editor open.
    - Ask first if pinned screenshots would close.
    - Check these again when the download finishes, before restarting.
    - If the app is quitting when a download finishes, don't install or relaunch.
  - **Downloading and checking:**
    - Download only over HTTPS from this repository (`github.com/AskTinNguyen/AtherScreenshot/…`); HTTPS redirects to GitHub's file hosts are fine.
    - Verify the size and SHA-256.
    - Unzip with `ditto`, check that the bundle's `CFBundleVersion` equals the manifest version (as a number), and, once builds are signed, check `codesign --verify` and the team identifier.
  - **Swapping in:**
    - Replace the bundle through a short helper that waits for the app to quit, then relaunches it and shows "Updated to Ather Screenshot 0.0.2".
    - Only the running instance may clean up leftovers of an earlier update. On Windows, every launch did it, which raced with an update another instance was downloading.
    - On any failure, keep the current version and offer the download page.
- **No new dependencies:** no Sparkle; do it by hand.
- **Tests:** port `updater_compares_versions`, `updater_manifest_only_from_this_repo_over_https` and `updater_verifies_and_swaps_the_exe`. Also port the opt-in end-to-end test that runs against a local server set with `ATHER_UPDATE_URL`.

## 5. Checklist before you report done

- [ ] All four features are in, with the lessons above.
- [ ] `xcrun swift test` passes, including the ported tests.
- [ ] `./build.sh package` builds the DMG and the update zip at `CFBundleShortVersionString` 0.0.2 and `CFBundleVersion` 0.0.2.1.
- [ ] The macOS README and the main README (Mac rows) are updated; the main README's download table points the Mac at the v0.0.2 release.
- [ ] Nothing is published or tagged until the owner says so.
