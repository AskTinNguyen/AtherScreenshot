#pragma once
#include "common.h"

namespace ather {

enum class BugVoicePhase { Off, Starting, Listening, Reconnecting, Unavailable };
struct BugVoiceStatus {
    BugVoicePhase phase = BugVoicePhase::Off;
    std::wstring microphone, language, lastHeard, message, hotkey;
    int audioLevel = 0;
    double levelAt = 0, signalAt = 0, heardAt = 0, startedAt = 0;
    bool rejected = false, speech = false, bugOpen = false, saving = false;
    double bugStarted = 0;
};
struct BugVoiceLabels {
    bool visible = false;
    std::wstring title, detail, input, activity, action;
    int meter = 0;
};
BugVoiceLabels DescribeBugVoice(const BugVoiceStatus& status, double now);
// Update and Hide are UI-thread operations. Current is safe on any thread.
void UpdateBugVoiceStatus(const BugVoiceStatus& status);
BugVoiceStatus CurrentBugVoiceStatus();
void HideBugVoicePanel();
// The same painter as the live HUD, independent of its capture exclusion.
BitmapPtr BugVoicePanelSnapshot(const BugVoiceStatus& status, double now);

}  // namespace ather
