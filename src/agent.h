#pragma once
#include "common.h"
#include "mcp.h"
#include "upload.h"

namespace ather {

class Settings;

// What the AI-agent tools (MCP) need from the rest of the app. Everything runs on the UI thread.
struct AgentHost {
    Settings* settings = nullptr;
    struct Command {
        std::wstring key, title;  // a [Hotkeys] key and its palette title
    };
    std::vector<Command> commands;  // run_command's choices
    std::function<void(const std::wstring& key)> runCommand;
    std::function<void(BitmapPtr img, const std::wstring& path)> setLast;  // "last capture" commands follow agents' captures
    std::function<void(const std::wstring& path)> openToEdit;
    std::function<void()> settingsChanged;  // settings.ini was written: reload it
    std::function<UploadConfig()> uploadConfig;
};

// The MCP server: Ather Screenshot's tools for agents (screenshots, recording, the gallery, OCR…).
std::shared_ptr<mcp::Server> MakeAgentServer(AgentHost host);

}  // namespace ather
