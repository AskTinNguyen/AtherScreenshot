#pragma once
#include "common.h"

namespace ather {

// Small non-activating notification in the bottom-right corner. Returns a token for UpdateToastBody.
uint64_t ShowToast(const std::wstring& title, const std::wstring& body, BitmapPtr thumb,
                   std::function<void()> onClick, int durationMs);
// Updates the body only if toast `token` is still the one on screen.
void UpdateToastBody(uint64_t token, const std::wstring& body);
// Returns true if a toast was visible.
bool HideToast();

}  // namespace ather
