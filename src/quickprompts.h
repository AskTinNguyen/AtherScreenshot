#pragma once
#include "common.h"

#include <ctime>
#include <map>
#include <mutex>

namespace ather {

// QuickPrompts: saved phrases and AI-agent prompts. Typing a prompt's trigger and then Space in any app replaces the
// trigger with the prompt (quickprompts_win.cpp listens and pastes); the picker (Ctrl+Alt+Space) lists them all.
//
// Three sources, one list:
//   builtin  the team starters compiled into the app (stable ids "builtin.*"; text can't be edited, only turned off)
//   team     a shared quickprompts-team.json named in Settings ([QuickPrompts] TeamFile); read-only here
//   user     the person's own, in quickprompts.json in the support folder
// When two enabled prompts share a trigger, the person's own wins, then the team's, then the built-in one.
//
// quickprompts.json (UTF-8, the same shape on Windows and the Mac):
//   {"version": 1,
//    "prompts": [{"id", "trigger", "title", "text", "enabled", "caseSensitive", "hotkey", "tags": [...], "uses", "lastUsed"}],
//    "state": {"<builtin or team id>": {"enabled": false, "uses": 3, "lastUsed": "..."}}}
// A team file has the same "prompts" array (other fields are ignored).

struct QuickPrompt {
    std::wstring id, trigger, title, text;
    std::wstring source = L"user";  // user | team | builtin
    bool enabled = true;
    bool caseSensitive = false;
    std::wstring hotkey;  // its own global shortcut, e.g. Ctrl+Alt+1 (user prompts only)
    std::vector<std::wstring> tags;
    int64_t uses = 0;
    std::wstring lastUsed;  // ISO 8601 UTC

    bool ReadOnly() const { return source != L"user"; }
};

// "" if the trigger is fine, else why not: 2-32 characters, no spaces or control characters.
std::wstring CheckTrigger(const std::wstring& trigger);

// The team starters (D11 of docs/intent/text-shortcuts/prompt.md).
const std::vector<QuickPrompt>& BuiltinQuickPrompts();

// ---- matching typed text ----

// Watches typed characters for "<trigger><Space>". Pure logic: the listener feeds it keys, tests feed it strings.
class TriggerMatcher {
public:
    struct Entry {
        std::wstring trigger;
        bool caseSensitive = false;
        int tag = 0;  // returned on a match (an index into the caller's list)
    };
    void SetTriggers(std::vector<Entry> entries);
    void Char(wchar_t c);  // a printable character that isn't a space
    void Backspace();
    void Reset();          // the caret may have moved: focus change, click, arrows, Enter, Tab, shortcuts...
    // Space typed: the matching entry's tag (longest trigger wins), or -1. On a match *erase is the number of
    // characters to delete (the trigger and the space) and the buffer starts over.
    int Space(int* erase);
    const std::wstring& Buffer() const { return buf_; }
    static constexpr size_t kMaxBuffer = 64;

private:
    std::vector<Entry> entries_;
    std::wstring buf_;
};

// Feeds a whole string (tests, the editor's Try it box): ' ' is Space, '\b' Backspace, '\n' Reset. Returns the tags
// matched, in order, and for each the erase count.
std::vector<std::pair<int, int>> FeedMatcher(TriggerMatcher& m, const std::wstring& typed);

// ---- placeholders ----

struct ExpandContext {
    std::wstring clipboard;    // {clipboard}
    std::wstring lastCapture;  // {lastcapture}
    std::tm now{};             // {date} {time} {datetime} {date:yyyy-MM-dd HH:mm:ss}
};

struct Expansion {
    std::wstring text;       // with \n line breaks
    int cursorBack = 0;      // ← presses after pasting, for {cursor} (a line break counts as one)
    bool pasteImage = false;  // {lastcapture:image}: paste the last capture after the text
};

// {cursor} {clipboard} {date} {time} {datetime} {date:FORMAT} {lastcapture} {lastcapture:image}; {{ and }} are
// literal braces; anything else in braces is left as typed.
Expansion ExpandPlaceholders(const std::wstring& text, const ExpandContext& ctx);
// The placeholders a text uses that aren't known (the editor marks them).
std::vector<std::wstring> UnknownPlaceholders(const std::wstring& text);

// ---- the store ----

class QuickPromptStore {
public:
    static QuickPromptStore& Shared();

    // Reads quickprompts.json from `folder` (the support folder) and the team file. Missing files are fine; a file
    // that doesn't parse is kept as quickprompts.json.bad and never overwritten (`*error` says so).
    void Load(const std::wstring& folder, const std::wstring& teamFile, std::wstring* error = nullptr);
    void SetTeamFile(const std::wstring& teamFile);
    // Re-reads files changed on disk since the last load or save. True if anything changed.
    bool ReloadIfChanged();
    bool Save();  // atomically (temp + rename)

    // Every prompt: built-in, team, then the person's own, each with its enabled state and use count applied.
    std::vector<QuickPrompt> All() const;
    // The prompts typed triggers can reach: enabled, one per trigger (user > team > builtin).
    std::vector<QuickPrompt> Active() const;
    bool Find(const std::wstring& id, QuickPrompt* out) const;
    std::wstring TeamError() const;  // why the team file couldn't be read, or ""

    // Changes (each saves). Add/Update return "" or the reason it was refused (bad or duplicate trigger).
    std::wstring Add(QuickPrompt p, std::wstring* newId = nullptr);
    std::wstring Update(const QuickPrompt& p);
    void Remove(const std::wstring& id);
    void SetEnabled(const std::wstring& id, bool on);  // any source
    void NoteUsed(const std::wstring& id);
    // Another enabled prompt with this trigger (case-insensitive), other than `exceptId`; "" if none.
    std::wstring TriggerOwner(const std::wstring& trigger, const std::wstring& exceptId) const;

    // Import/export. Import returns how many were added; triggers already taken are skipped and listed in *skipped.
    int ImportFile(const std::wstring& path, std::vector<std::wstring>* skipped, std::wstring* error);
    bool ExportJson(const std::wstring& path, const std::vector<std::wstring>& ids) const;
    bool ExportCsv(const std::wstring& path, const std::vector<std::wstring>& ids) const;

    // Parsing, exposed for tests.
    static std::vector<QuickPrompt> ParsePrompts(const std::string& utf8, bool* ok);
    static std::vector<QuickPrompt> ParseCsv(const std::wstring& text);
    static std::vector<QuickPrompt> ParseApplePlist(const std::wstring& text);
    static std::wstring MakeId();

    uint64_t Version() const { return version_; }  // bumps on every change, for the listener and the UI
    std::wstring UserPath() const { return folder_.empty() ? L"" : folder_ + L"\\quickprompts.json"; }

private:
    struct State {
        bool enabled = true;
        int64_t uses = 0;
        std::wstring lastUsed;
    };
    void LoadTeam();
    QuickPrompt* FindUser(const std::wstring& id);
    std::wstring Validate(const QuickPrompt& p) const;

    mutable std::recursive_mutex mu_;
    std::wstring folder_, teamFile_, teamError_;
    std::vector<QuickPrompt> user_, team_;
    std::map<std::wstring, State> state_;
    FILETIME userStamp_{}, teamStamp_{};
    bool userBad_ = false;  // the file didn't parse: don't overwrite it
    uint64_t version_ = 1;
};

}  // namespace ather
