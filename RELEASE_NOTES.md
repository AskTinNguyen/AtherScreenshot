# Ather Screenshot 0.0.2 (Windows and macOS)

**Get it on Windows:** download `AtherScreenshot-Setup-0.0.2.exe` below and run it. Pick **INSTALL** (per user, no admin rights) or **Run without installing**.
- **Earlier 0.0.2:** if you have the 0.0.2 build from earlier, the app offers this update by itself, or right away with "Check for updates…" in the tray.
- **0.0.1:** copies of 0.0.1 need this one download. After that, updates come in the app.
- **SmartScreen:** the exe isn't code-signed yet, so SmartScreen asks once: **More info › Run anyway**.

**Get it on macOS:** download `AtherScreenshot-0.0.2-macOS.dmg` below (macOS 14 or later, Apple silicon and Intel), open it and drag Ather Screenshot to Applications.
- **Gatekeeper:** the app isn't notarized yet, so open it once, then allow it in System Settings › Privacy & Security › **Open Anyway**.
- **Updates:** from 0.0.2 on, the Mac app updates itself too ("Check for updates…" in the menu bar or the palette). Run it from Applications, not from the disk image.
- **Controllers:** on the Mac, Xbox, PlayStation and MFi controllers work directly.

## What's new

### Updates from inside the app
- **Checking:** Ather Screenshot looks for a new version a minute after it starts and then once a day, and tells you once when there is one. "Check for updates…" in the tray menu or the palette checks right away.
- **Installing:** click the notification, or **Update to version …** in the tray. The new version downloads in the background (about 3 MB), is checked (size, SHA-256 and the version inside it), swapped in, and the app restarts. Settings, saved captures and the gallery stay. If anything goes wrong, you keep the version you have.
- **When it waits:** it waits while you record, select a region, run a scrolling capture or have an editor open. It asks before closing pinned screenshots.
- **Turning it off:** use Settings › Updates. It only ever downloads from this repository's GitHub releases.

### Your game controller in recordings
- **Turning it on:** use **Show game controller** (Settings › Recording, or the palette) to draw a connected controller in a corner of the video, like OBS's Input Overlay. Nothing is drawn while no controller is connected.
- **What it shows:** the sticks move, triggers fill and buttons light up as you play: a white controller with orange-lit sticks and colored A/B/X/Y.
- **Settings:** choose the corner, and the **opacity** (10–100%) so the game shows through.
- **Controllers:** works with Xbox-style (XInput) controllers. PlayStation controllers work through Steam Input or DS4Windows.

### Edit pictures and videos from anywhere
- **Explorer:** right-click a picture or video › **Edit with Ather Screenshot**, or use **Open with**. You can also use "Open a picture or video to edit…" in the tray. Videos open in the video editor.
- **Your files stay as they were:** edits are saved as new files in your captures.
- **Phone videos** recorded sideways now show upright everywhere: preview, saved video and thumbnails.
- **More formats:** MP4, MOV, M4V, WMV, AVI and MKV, also in the gallery's import.

### Join several videos into one
- **Adding clips:** in the video editor, **Add › Video clip…** (Ctrl+O) or drop video files on it to add them after the current clip. Videos of a different shape get black bars.
- **The clip lane:** a clip lane appears once there are two or more clips. Drag a clip to reorder it, and drag a selected clip's edges to trim it.
- **Splitting:** **S** splits at the playhead, so you can cut out a middle part. A clip's row also has Earlier, Later and Remove.
- **Your edits follow:** captions, callouts, blur and zoom move with the footage they're on. Undo covers all of it, and auto captions work across the clips.

### Fixes and polish
- **Clip lane:** fixed a crash when pressing Delete or Undo while dragging a clip. Fixed a clip's left edge being impossible to grab, and the playhead knob being taken over by the clip lane.
- **Video editor:**
  - Very short clips no longer stretch past their file when you click an edge.
  - Undo during playback keeps your place.
  - Auto captions land on the right footage even if you rearranged clips while they ran.
- **Unreadable files:** a video the PC can't decode (for example HEVC without the Windows extension) is refused when you add it, instead of breaking the whole edit. Sound that can't be decoded no longer saves as a silent track.
- **Preview:**
  - Plays straight through a split of the same video.
  - Skips a clip that can't play.
  - Shows non-square-pixel video the same way as the saved file.
- **Gallery:** existing phone videos are measured again once, so they get upright tiles.
- **Controller overlay:** costs less per frame, and ignores presses made while a recording is paused.

## Checksums

`SHA256SUMS.txt` lists the SHA-256 of the setup exe and the portable zip.
