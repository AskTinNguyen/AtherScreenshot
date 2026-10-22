# Ather Screenshot 0.0.2 (Windows and macOS)

**Get it on Windows:** download `AtherScreenshot-Setup-0.0.2.exe` below and run it. Pick **INSTALL** (per user, no admin rights) or **Run without installing**.
- **Earlier 0.0.2:** if you have the 0.0.2 build from earlier, the app offers this update by itself, or right away with "Check for updates…" in the tray.
- **0.0.1:** copies of 0.0.1 need this one download. After that, updates come in the app.
- **SmartScreen:** the exe isn't code-signed yet, so SmartScreen asks once: **More info › Run anyway**.

**Get it on macOS:** download `AtherScreenshot-0.0.2-macOS.dmg` below (macOS 14 or later, Apple silicon and Intel), open it and drag Ather Screenshot to Applications.
- **Gatekeeper:** the app isn't notarized yet, so open it once, then allow it in System Settings › Privacy & Security › **Open Anyway**.
- **Updates:** from 0.0.2 on, the Mac app updates itself too ("Check for updates…" in the menu bar or the palette). Run it from Applications, not from the disk image.
- **Controllers:** on the Mac, Xbox, PlayStation and MFi controllers work directly.

## Coming in the next update (Windows and macOS)

### Copy a clip or a frame, then paste it into Slack
- **Clip:** in the video editor, Shift+drag on the timeline (or press Shift+I and Shift+O) to mark the part you want, then press Ctrl+C (⌘C on the Mac). It's saved as an MP4 in your captures and put on the clipboard, so Ctrl+V in Slack, Teams or Discord attaches it. No more Save, find the file, drag it in. With nothing marked, Ctrl+C copies the clip selected in the clip lane.
- **Frame:** Ctrl+Shift+C (⌘⇧C) copies the frame on screen as a picture, with your crop, captions and markup; so does Ctrl+C when nothing is marked or selected.
- **Plays everywhere:** copied clips are H.264 + AAC MP4s, which play right in Slack and on any PC. Save, Save GIF and Save smaller write what they did before.

### Capture tray: keep capturing, then save the pictures you pick together
- **What:** Capture and annotate (Ctrl+Alt+E, ⌃⌥E on the Mac) no longer opens a new window for every capture. Captures collect in the open editor, in a strip of thumbnails along its bottom edge. Click one to annotate it, switch with Ctrl+Tab (⌃⇥), and each picture keeps its own notes and undo.
- **Saving:** tick the pictures to keep and press **Save selected**. Each is saved as its own PNG in your captures folder and shows up in the gallery. Done saves the ticked ones and closes. Closing with pictures that aren't saved asks first.
- **Every capture:** set After capture to "Open in editor" to collect every capture this way. Settings › **Collect captures in one editor** turns it off.

### Bug reports by voice: "bug start" … "bug stop"
- **What:** while you playtest with the screen recorded, say "bug start", describe the bug, then say "bug stop" (or press `Ctrl+Alt+B` / `⌃⌥B` to start and again to stop). You get one folder per bug: the screenshot with what you said written on it, a clip from 10 seconds before the bug with captions (cut when the recording stops), your narration as `voice.wav`, the game's logs from the paths you set, and `bug.md` / `bug.json`. "Copy last bug report" puts it on the clipboard for Slack.
- **Privacy:** listening is off until you turn on Settings › Bug reports › Listen for "bug start" / "bug stop". Speech is recognized on your computer. On the Mac, "Transcribe Vietnamese with Apple's servers" is a separate opt-in: it writes the narration down on the Mac when the Mac knows Vietnamese, and sends only the narration to Apple when it doesn't.

## What's new in this update (Windows and macOS, build 0.0.2.4)

### AI agents can take screenshots and record for you
- **What:** Claude Code, Codex and other AI agents that speak MCP can now use Ather Screenshot: take screenshots of the screen, a display, a window or a region; record gameplay to MP4 or GIF and mark moments while it runs; look back through a recording frame by frame or as one contact sheet; read text on screen; and find, tag and rate captures in the gallery. They can also change capture settings and run any palette command.
- **Playtesting:** ask your agent to record the game while it plays and to mark every bug it sees. Its markers are saved next to the video as review notes, and on Windows the video editor shows each one on its frame.
- **Turning it on:** it's off until you turn on Settings › AI agents › **Allow AI agents (MCP)**. Then run **Copy AI agent setup (MCP)** from the command palette and paste it into your agent. For Claude Code that's one `claude mcp add` line.
- **Privacy:** only programs on this computer that hold a private token from your settings folder can connect, and requests from web pages are refused. A notification shows when an agent connects. Agents can see your screen, so allow only agents you trust.
- **Every tool:** see [docs/AI-AGENTS.md](docs/AI-AGENTS.md).

### Compress a video or picture to fit a size
- **Two clicks:** right-click a capture in the gallery › Compress › Under 30 MB, 8 MB (Discord), 25 MB (email), 50 MB or any size you type. Pictures offer 2, 1 and 5 MB. Or use "Compress a picture or video…" in the palette for any file. While editing a video, Save smaller (in Save ▾ on Windows, next to Save GIF on the Mac) saves your edit to fit the size.
- **What you get:** a smaller copy next to the original (`<name>_small.mp4` or `.jpg`); the original stays as it is. Videos are H.264 MP4s that play in every chat app and browser, sized from the length so they land just under the limit (re-made a bit smaller if they don't). Pictures become JPEGs.

## Earlier update (macOS, build 0.0.2.3)

### Saving videos is about twice as fast, GIFs about 9 times
- **How much:** on an M4 Max, a 20-second 3K recording saves in about 2 s instead of 3.6 s, also with captions, callouts, blur and zoom; a 1080p one in 0.7 s instead of 1.5 s; a 2-minute 3K recording in about 15 s instead of 27 s. GIFs save in under half a second instead of 2.5 to 4.5 s.
- **How:** videos are saved as HEVC, longer ones in two pieces on the Mac's video encoders at once and then joined without re-encoding. GIFs are made in one pass, straight from your edits, in a few parts at once.
- **What you get:** your edits look exactly as before, as close to the original as the old files or closer. Files are often smaller; a busy short clip can come out bigger, because it keeps more detail.
- **HEVC:** the saved MP4s play in QuickTime, Safari and current browsers. On Windows they need Microsoft's HEVC Video Extensions; some older players and sites may not take them.
- **GIFs:** the same size and length as before. During a fast zoom a frame can sit a moment earlier or later than before.

## Earlier update (Windows, build 0.0.2.3)

### Review recordings frame by frame, with notes
- **Every frame:** ←/→ now step exactly one frame of the video, also in 60 fps recordings (before, a step skipped every other frame). The readout under the video shows `0:12:37 · frame 757 · 60 fps`; Ctrl+G goes to a frame or a time; J/K/L play backward and forward at 0.25×, 0.5× or 1× without skipping frames; zoom the timeline down to single frames and the video up to 8× to see every pixel.
- **Notes:** press M to leave a note on the frame on screen (Note, Issue, Question or Looks good; resolved or open; pinned to a spot). They're kept next to the video in `<video>.notes.json`, listed beside the video, and copied as text with one click.
- **Review video:** Save ▾ › Save review video (Ctrl+Alt+S) makes an MP4 for the other team: a summary card, the timecode and frame number in the corner, and each note's frame held for 3 seconds under its note. A notes list and a contact sheet picture are saved next to it.
- **Saving is unchanged:** Save and Save GIF make exactly the same videos as before.

## Earlier update (Windows, build 0.0.2.2)

### Saving videos is much faster
- **How much:** saving from the video editor is about 36 times faster than before. A 20-second 1440p recording saves in about a second instead of half a minute; with captions, callouts, blur and zoom, in about 1.5 s instead of more than a minute. A 5-minute 1080p recording saves in about 7 s.
- **GIFs:** about 25 to 45 times faster too.
- **How:** the video is decoded on the graphics card, the frames are drawn on all your processor's cores, only the parts your edits change are redrawn, and longer videos are encoded in pieces on several hardware encoders at once, then joined without re-encoding.
- **What you get is the same:** your edits look exactly as before. Without a graphics card that encodes video, saving still works the usual way.
- **Crops:** a saved crop can sit one pixel up or left of where you dragged it, so its colors go to the encoder as they were recorded.

## What's new in 0.0.2

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
