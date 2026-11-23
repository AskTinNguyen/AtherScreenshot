#pragma once
#include "common.h"
#include "gamepad.h"

namespace ather {

enum class BugVoicePhase { Off, Starting, Listening, Reconnecting, Unavailable };
struct BugVoiceStatus {
    BugVoicePhase phase = BugVoicePhase::Off;
    std::wstring microphone, language, lastHeard, message, hotkey;
    int audioLevel = 0;
    double levelAt = 0, signalAt = 0, heardAt = 0, startedAt = 0;
    bool rejected = false, speech = false, bugOpen = false, saving = false;
    double bugStarted = 0;
    bool controllerEnabled = false;
    PadState controller;
    double controllerInputAt = 0;
};
struct BugVoiceLabels {
    bool visible = false;
    std::wstring title, detail, input, activity, action;
    int meter = 0;
    std::wstring microphoneSignal, controllerConnection, controllerInput;
};
BugVoiceLabels DescribeBugVoice(const BugVoiceStatus& status, double now);
// Update, Hide and Configure are UI-thread operations. Current is safe on any thread.
void UpdateBugVoiceStatus(const BugVoiceStatus& status);
BugVoiceStatus CurrentBugVoiceStatus();
void HideBugVoicePanel();
void ConfigureGamepadIndicator(bool enabled);
// The same painter as the live HUD, independent of its capture exclusion.
BitmapPtr BugVoicePanelSnapshot(const BugVoiceStatus& status, double now);

}  // namespace ather
