# Ather Screenshot: install guide

## Install (about 30 seconds, no admin rights)

1. Download **`AtherScreenshot-Setup-<version>.exe`**.
2. Double-click it.
   - If Windows SmartScreen says *"Windows protected your PC"*, click **More info → Run anyway**. The app isn't code-signed yet, so Windows doesn't recognize the publisher.
3. Click **INSTALL**. Leave **Start with Windows** on if you want it running all the time.

The installer:
- installs the app to `%LOCALAPPDATA%\Programs\AtherScreenshot` (just for you)
- adds a **Start menu** shortcut
- adds an uninstall entry in **Settings › Apps**

## Use it

| Shortcut | What it does |
|---|---|
| `PrintScreen` | Capture a region (click = window, drag = area) |
| `Ctrl+Alt+K` | Command palette: every feature, searchable |
| `Ctrl+Alt+E` | Capture and annotate |
| `Shift+PrintScreen` | Record MP4 (press again to stop) |
| `Ctrl+Alt+H` | Capture gallery: search, tags, collections, versions |

Every feature is in the command palette. Change shortcuts and options in **Settings** (palette → "Settings").

Click the notification after a recording (or open an MP4 from the gallery) to trim it, crop it, change its speed, and add captions and callouts. **Auto captions** use Windows speech recognition on your PC. If it isn't installed for your language, add a speech pack in Windows Settings › Time & language › Speech.

**If PrintScreen doesn't work:** go to Windows Settings › Accessibility › Keyboard and turn off *"Use the Print screen key to open screen capture"*. Also close ShareX, Lightshot or any other tool that uses that key.

## Update

Run the newer `AtherScreenshot-Setup-<version>.exe` and click **UPDATE**. Your settings and captures are kept.

## Uninstall

Go to **Settings › Apps › Installed apps › Ather Screenshot › Uninstall**. Your captures (`Pictures\AtherScreenshot`), settings and gallery library (`%APPDATA%\AtherScreenshot`) are kept.

## Portable use

Choose **Run without installing** in the setup window to run it from wherever the exe is. It won't ask again from that folder.
