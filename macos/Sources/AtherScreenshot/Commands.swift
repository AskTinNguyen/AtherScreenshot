import Foundation

enum Cmd: String, CaseIterable {
    case palette = "CommandPalette"
    case region = "CaptureRegion"
    case regionEdit = "CaptureRegionEdit"
    case recordVideo = "RecordVideo"
    case recordGif = "RecordGif"
    case stopRecording = "StopRecording"
    case recordWindow = "RecordWindow"
    case pauseRecording = "PauseRecording"
    case ruler = "ScreenRuler"
    case toggleSystemAudio = "ToggleSystemAudio"
    case toggleMic = "ToggleMicrophone"
    case toggleClicks = "ToggleShowClicks"
    case toggleKeys = "ToggleShowKeys"
    case toggleGamepad = "ToggleShowGamepad"
    case regionRedact = "CaptureRegionRedact"
    case renameLast = "RenameLastCapture"
    case toggleAutoRedact = "ToggleAutoRedact"
    case regionUpload = "CaptureRegionUpload"
    case uploadLast = "UploadLastCapture"
    case uploadFile = "UploadFile"
    case history = "History"
    case scrolling = "CaptureScrolling"
    case editLast = "EditLastCapture"
    case openImage = "OpenImageInEditor"
    case fullscreen = "CaptureFullscreen"
    case monitor = "CaptureMonitor"
    case window = "CaptureWindow"
    case lastRegion = "CaptureLastRegion"
    case regionPin = "CaptureRegionPin"
    case ocr = "CaptureText"
    case colorPicker = "PickColor"
    case regionDelayed = "CaptureRegionDelayed"
    case fullscreenDelayed = "CaptureFullscreenDelayed"
    case pinLast = "PinLastCapture"
    case copyLast = "CopyLastCapture"
    case openLast = "OpenLastCapture"
    case openFolder = "OpenCapturesFolder"
    case closeAllPins = "CloseAllPins"
    case toggleClipboard = "ToggleCopyToClipboard"
    case toggleSave = "ToggleSaveToFile"
    case toggleCursor = "ToggleCaptureCursor"
    case toggleLogin = "ToggleLaunchAtLogin"
    case settings = "EditSettings"
    case checkUpdates = "CheckForUpdates"
    case about = "About"
    case quit = "Quit"
}

struct CmdDef {
    let cmd: Cmd
    let title: String
    let keywords: String
    let icon: String          // SF Symbol
    let hotkey: String        // default shortcut
    let capture: Bool         // needs other UI out of the way before it runs
}

// macOS reserves ⌘⇧3/4/5 for its own screenshot tool, so the defaults live on ⌃⌥.
let kCmds: [CmdDef] = [
    CmdDef(cmd: .palette, title: "Command palette", keywords: "", icon: "command", hotkey: "Ctrl+Alt+K", capture: false),
    CmdDef(cmd: .region, title: "Capture region", keywords: "screenshot snip area select crop window", icon: "crop", hotkey: "Ctrl+Alt+4", capture: true),
    CmdDef(cmd: .regionEdit, title: "Capture region and annotate", keywords: "edit editor draw arrow markup blur", icon: "pencil.tip.crop.circle", hotkey: "Ctrl+Alt+E", capture: true),
    CmdDef(cmd: .recordVideo, title: "Record screen (MP4)", keywords: "video screencast capture movie", icon: "record.circle", hotkey: "Ctrl+Alt+5", capture: true),
    CmdDef(cmd: .recordGif, title: "Record GIF", keywords: "animation animated screencast", icon: "photo.stack", hotkey: "Ctrl+Alt+G", capture: true),
    CmdDef(cmd: .stopRecording, title: "Stop recording", keywords: "finish end save video gif", icon: "stop.circle", hotkey: "", capture: false),
    CmdDef(cmd: .recordWindow, title: "Record a window (follows it)", keywords: "video mp4 app track follow", icon: "macwindow.badge.plus", hotkey: "", capture: true),
    CmdDef(cmd: .pauseRecording, title: "Pause / resume recording", keywords: "break hold continue", icon: "pause.circle", hotkey: "", capture: false),
    CmdDef(cmd: .ruler, title: "Screen ruler", keywords: "measure pixels distance size length angle", icon: "ruler", hotkey: "", capture: true),
    CmdDef(cmd: .toggleSystemAudio, title: "Record system audio", keywords: "toggle sound speaker loopback", icon: "speaker.wave.2", hotkey: "", capture: false),
    CmdDef(cmd: .toggleMic, title: "Record microphone", keywords: "toggle mic voice", icon: "mic", hotkey: "", capture: false),
    CmdDef(cmd: .toggleClicks, title: "Show clicks in recordings", keywords: "toggle mouse ripple", icon: "cursorarrow.click", hotkey: "", capture: false),
    CmdDef(cmd: .toggleKeys, title: "Show keystrokes in recordings", keywords: "toggle keyboard keys", icon: "keyboard", hotkey: "", capture: false),
    CmdDef(cmd: .toggleGamepad, title: "Show game controller in recordings", keywords: "toggle gamepad controller xbox playstation joystick input overlay", icon: "gamecontroller", hotkey: "", capture: false),
    CmdDef(cmd: .regionRedact, title: "Capture region with auto-redact", keywords: "privacy hide email key token password ocr pixelate", icon: "eye.slash", hotkey: "", capture: true),
    CmdDef(cmd: .renameLast, title: "Rename last capture…", keywords: "name file title", icon: "character.cursor.ibeam", hotkey: "", capture: false),
    CmdDef(cmd: .toggleAutoRedact, title: "Auto-redact every capture", keywords: "toggle privacy ocr pixelate", icon: "eye.slash", hotkey: "", capture: false),
    CmdDef(cmd: .regionUpload, title: "Capture region and upload", keywords: "share link imgur s3 url", icon: "icloud.and.arrow.up", hotkey: "Ctrl+Alt+U", capture: true),
    CmdDef(cmd: .uploadLast, title: "Upload last capture", keywords: "share link imgur s3 url", icon: "icloud.and.arrow.up", hotkey: "", capture: false),
    CmdDef(cmd: .uploadFile, title: "Upload a file…", keywords: "share link imgur s3 url", icon: "doc.badge.arrow.up", hotkey: "", capture: false),
    CmdDef(cmd: .history, title: "Capture gallery", keywords: "history browse recent search thumbnails library tags collections organize", icon: "square.grid.2x2", hotkey: "Ctrl+Alt+H", capture: false),
    CmdDef(cmd: .scrolling, title: "Scrolling capture (long page)", keywords: "scroll stitch full page chat long", icon: "arrow.up.and.down.text.horizontal", hotkey: "Ctrl+Alt+S", capture: true),
    CmdDef(cmd: .editLast, title: "Annotate last capture", keywords: "edit editor draw markup", icon: "pencil.and.outline", hotkey: "", capture: false),
    CmdDef(cmd: .openImage, title: "Open a picture or video to edit…", keywords: "edit file annotate load image video mp4 mov import", icon: "photo", hotkey: "", capture: false),
    CmdDef(cmd: .fullscreen, title: "Capture full screen", keywords: "all monitors desktop entire everything displays", icon: "rectangle.on.rectangle", hotkey: "Ctrl+Alt+3", capture: true),
    CmdDef(cmd: .monitor, title: "Capture current display", keywords: "monitor screen", icon: "display", hotkey: "Ctrl+Alt+Shift+3", capture: true),
    CmdDef(cmd: .window, title: "Capture active window", keywords: "app foreground focused", icon: "macwindow", hotkey: "Ctrl+Alt+W", capture: true),
    CmdDef(cmd: .lastRegion, title: "Repeat last region", keywords: "again previous same", icon: "arrow.counterclockwise", hotkey: "", capture: true),
    CmdDef(cmd: .regionPin, title: "Capture region and pin to screen", keywords: "float sticky reference overlay", icon: "pin", hotkey: "", capture: true),
    CmdDef(cmd: .ocr, title: "Capture text (OCR)", keywords: "ocr recognize extract read copy text", icon: "text.viewfinder", hotkey: "Ctrl+Alt+T", capture: true),
    CmdDef(cmd: .colorPicker, title: "Pick color from screen", keywords: "eyedropper hex rgb colour", icon: "eyedropper", hotkey: "", capture: true),
    CmdDef(cmd: .regionDelayed, title: "Capture region after delay", keywords: "timer wait delay", icon: "timer", hotkey: "", capture: true),
    CmdDef(cmd: .fullscreenDelayed, title: "Capture full screen after delay", keywords: "timer wait delay menu", icon: "timer", hotkey: "", capture: true),
    CmdDef(cmd: .pinLast, title: "Pin last capture", keywords: "float sticky", icon: "pin", hotkey: "", capture: false),
    CmdDef(cmd: .copyLast, title: "Copy last capture", keywords: "clipboard again", icon: "doc.on.doc", hotkey: "", capture: false),
    CmdDef(cmd: .openLast, title: "Open last capture", keywords: "view image file preview", icon: "eye", hotkey: "", capture: false),
    CmdDef(cmd: .openFolder, title: "Open captures folder", keywords: "finder directory history files", icon: "folder", hotkey: "", capture: false),
    CmdDef(cmd: .closeAllPins, title: "Close all pinned images", keywords: "unpin remove", icon: "pin.slash", hotkey: "", capture: false),
    CmdDef(cmd: .toggleClipboard, title: "Copy to clipboard after capture", keywords: "toggle setting", icon: "doc.on.clipboard", hotkey: "", capture: false),
    CmdDef(cmd: .toggleSave, title: "Save to file after capture", keywords: "toggle setting disk png", icon: "square.and.arrow.down", hotkey: "", capture: false),
    CmdDef(cmd: .toggleCursor, title: "Include mouse cursor", keywords: "toggle setting pointer", icon: "cursorarrow", hotkey: "", capture: false),
    CmdDef(cmd: .toggleLogin, title: "Launch at login", keywords: "toggle startup boot autostart", icon: "power", hotkey: "", capture: false),
    CmdDef(cmd: .settings, title: "Settings", keywords: "preferences options config hotkeys shortcuts keyboard", icon: "gearshape", hotkey: "", capture: false),
    CmdDef(cmd: .checkUpdates, title: "Check for updates…", keywords: "update upgrade new version download latest", icon: "arrow.down.circle", hotkey: "", capture: false),
    CmdDef(cmd: .about, title: "About Ather Screenshot", keywords: "version info help", icon: "info.circle", hotkey: "", capture: false),
    CmdDef(cmd: .quit, title: "Quit Ather Screenshot", keywords: "exit close", icon: "power.circle", hotkey: "", capture: false),
]

func cmdDef(_ c: Cmd) -> CmdDef { kCmds.first { $0.cmd == c }! }

// Short command-line names (`AtherScreenshot region --pin`). Any Cmd raw value works too.
let kCliNames: [String: Cmd] = [
    "region": .region, "fullscreen": .fullscreen, "monitor": .monitor, "window": .window, "last": .lastRegion,
    "scroll": .scrolling, "ruler": .ruler, "ocr": .ocr, "color": .colorPicker, "record": .recordVideo,
    "gif": .recordGif, "recordwindow": .recordWindow, "stop": .stopRecording, "pause": .pauseRecording,
    "history": .history, "palette": .palette, "folder": .openFolder, "settings": .settings, "quit": .quit,
]
