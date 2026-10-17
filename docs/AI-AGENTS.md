# AI agents (MCP)

Ather Screenshot is an [MCP](https://modelcontextprotocol.io) server, so AI agents such as Claude Code, Codex, Claude Desktop or Cursor can see the screen and record it: take screenshots while playtesting, record a gameplay session and mark the moments that matter, then look back through the recording frame by frame. Windows and the Mac offer the same tools with the same arguments.

## Setup

1. **Allow agents.** Settings › AI agents › **Allow AI agents (MCP)**, or "Allow AI agents (MCP)" in the command palette. It's off by default.
2. **Connect your agent.** "Copy AI agent setup (MCP)" in the palette copies ready-to-paste configurations with this computer's paths:

   | Agent | Setup |
   |---|---|
   | Claude Code | `claude mcp add ather-screenshot -- "<path to the app>" --mcp` |
   | Codex | `~/.codex/config.toml`: `[mcp_servers.ather-screenshot]` with `command = '<path to the app>'` and `args = ["--mcp"]` |
   | Claude Desktop, Cursor… | `"ather-screenshot": { "command": "<path to the app>", "args": ["--mcp"] }` |
   | URL-based clients | `http://127.0.0.1:47821/mcp`, header `Authorization: Bearer <token>` |

   The app's path is `%LOCALAPPDATA%\Programs\AtherScreenshot\AtherScreenshot.exe` once installed on Windows, and `/Applications/Ather Screenshot.app/Contents/MacOS/AtherScreenshot` on the Mac.

`AtherScreenshot --mcp` (or `AtherScreenshot.exe --mcp`) is a small stdio server: it passes each message to the running app over 127.0.0.1 and starts the app if it isn't running. It finds the port and token by itself, so its configuration holds no secrets.

## How it works

- The running app serves MCP's Streamable HTTP transport on `127.0.0.1:<AgentPort>/mcp` (default 47821; Windows `[AiAgents] AgentPort`, Mac `AgentPort`). Responses are plain JSON; there are no sessions and no server-sent events.
- **Who can connect:** only programs on this computer that send the token in `agent-token` in the settings folder (`%APPDATA%\AtherScreenshot` or `~/Library/Application Support/AtherScreenshot`). Only you can read that file. Requests from web pages are refused (their `Origin`, or a `Host` other than 127.0.0.1/localhost, gives 403), so a site can't drive the app even through DNS rebinding.
- A notification shows when an agent connects, at most once every 10 minutes per agent.
- Agents work like you do: their screenshots and recordings go to the captures folder and the gallery, follow the file-name template, and update "last capture". They don't touch the clipboard unless asked (`copy_to_clipboard`).

## Tools

Coordinates are physical pixels on Windows and points on the Mac, with the primary display's top-left at 0,0; `get_status` lists the displays. Most capture tools take a **target**:

| Argument | Meaning |
|---|---|
| `target` | `screen` (every display, the default), `display`, `window` or `region` |
| `display` | display index from `get_status` (0 = primary) |
| `window_id` / `window_title` | a window from `list_windows`, or part of its title or app name (default: the active window) |
| `x`, `y`, `width`, `height` | the region |
| `bring_to_front` | activate the window first. Otherwise a covered window is drawn by itself (Windows; some games come out black that way) or captured directly (Mac) |
| `include_cursor` | draw the pointer (default: the app's setting) |

| Tool | What it does |
|---|---|
| `get_status` | Version, captures folder, displays, the active window, the recording state (time, markers) and the capture and recording settings |
| `list_windows` | App windows with ids, titles, app names and rectangles (`query` filters) |
| `take_screenshot` | Captures a target, saves a PNG (`save: false` to skip) and returns the path and the picture (`image`: `preview` at most 1568 px, `full` or `none`). `tags`, `comment`, `delay_seconds`, `redact`, `copy_to_clipboard` |
| `start_recording` | Records a target to MP4 or GIF (`format`) and returns at once. A window target follows the window (`follow_window`). `fps`, `countdown_seconds` (default 0), `max_seconds`, `system_audio`, `microphone`, `show_clicks`, `show_keys`, `show_gamepad`, `tags`, `comment` |
| `add_marker` | Marks the current moment of the recording: `label`, `kind` (`note`, `issue`, `question`, `good`), optional `seconds` |
| `pause_recording` | `paused: true` pauses, `false` resumes |
| `stop_recording` | Stops, waits for the file and returns its path, length, size and markers (`discard: true` throws it away). Also reports a recording that already ended |
| `get_video_frames` | Frames at `times`, or `count` frames between `start_seconds` and `end_seconds`; `layout: "sheet"` returns one contact sheet |
| `get_capture` | A picture, or one frame of a video (`time_seconds`), with its size |
| `read_text` | OCR of a file, a video frame or a fresh capture; `with_boxes` gives each word's rectangle in screen coordinates |
| `list_captures` | Searches the gallery like its search box (`query`, also "today" or "last week"), `type`, `tags`, `min_rating`, `limit`, `offset` |
| `tag_capture` | `add_tags`, `remove_tags`, `rating` (0–5), `comment` |
| `configure` | Changes capture and recording settings: clipboard, saving, cursor, auto-redact, notifications, frame rates, countdown, audio, microphone and the click, key and controller overlays |
| `open_in_editor` | Opens a picture or video in the editor for the person at the computer |
| `upload_capture` | Uploads with the uploader from Settings › Upload and returns the link |
| `run_command` | Runs any command-palette command by its shortcut name, e.g. `CaptureScrolling` or `ToggleShowGamepad` |

### Markers and review notes

When an MP4 recording with markers is saved, its markers are written next to it as `<video>.notes.json` in the format the Windows video editor uses for review notes (frame, time, kind, author, text). Open the video in the video editor and each marker sits on its frame, ready for frame review and the review video. `stop_recording` and `list_captures` return the notes file's path.

## Example: a playtest session

> Record the "Space Miner" window while you play the first level. Each time something looks wrong, add an issue marker and take a tagged screenshot. Then stop, look through the recording around each marker, and write me a bug list with timestamps and screenshot paths.

What the agent does with it:

1. `start_recording` with `target: "window"`, `window_title: "Space Miner"`, `show_gamepad: true`.
2. While playing: `add_marker` with `kind: "issue"`, `label: "player clips through the wall"`, and `take_screenshot` of the same window with `tags: ["bug", "level-1"]`.
3. `stop_recording` returns the MP4 and the markers.
4. `get_video_frames` with `times` around each marker (or `layout: "sheet"` for an overview) to check what happened.
5. `list_captures` with `tags: ["bug"]` to collect the screenshots for the report.
