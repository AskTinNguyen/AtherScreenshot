#pragma once
#include "common.h"

namespace ather {

struct OcrWord {
    std::wstring text;
    RECT rect;  // image coordinates
    int line;   // index of the line the word belongs to
};

// Windows.Media.Ocr using the user's profile languages. Runs on a worker thread; `done` runs on the UI thread.
void RecognizeTextAsync(BitmapPtr img, std::function<void(std::wstring text, std::wstring error)> done);
void RecognizeWordsAsync(BitmapPtr img, std::function<void(std::vector<OcrWord> words, std::wstring error)> done);
// Blocking variant for background threads (initializes its own apartment). Never call on the UI thread.
std::wstring RecognizeTextBlocking(const Bitmap& img);

// Rectangles (image coordinates) of words that look sensitive: emails, IP addresses, API keys/tokens,
// JWTs, card numbers (Luhn-checked) and phone numbers.
std::vector<RECT> FindSensitive(const std::vector<OcrWord>& words);

}  // namespace ather
