#include "ocr.h"

#include "media.h"

#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cwctype>
#include <regex>
#include <thread>

#pragma comment(lib, "windowsapp")

namespace ather {

namespace wgi = winrt::Windows::Graphics::Imaging;
namespace wmo = winrt::Windows::Media::Ocr;
namespace wss = winrt::Windows::Storage::Streams;

namespace {

// Screen text is small; the OCR engine is far more accurate on upscaled input.
int UpscaleFactor(int w, int h) {
    if (h < 100 || w < 200) return 3;
    if (std::max(w, h) < 1600) return 2;
    return 1;
}

struct OcrData {
    std::vector<OcrWord> words;
    std::vector<std::wstring> lines;  // engine-joined line text (correct spacing for CJK, etc.)
};

// Runs on a worker thread with an initialized apartment. Throws on WinRT errors.
OcrData RunOcr(const Bitmap& img, std::wstring& err) {
    OcrData data;
    auto& words = data.words;
    auto engine = wmo::OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) {
        err = L"No OCR language installed. Add one in Settings › Time & language › Language.";
        return data;
    }
    const uint32_t maxDim = wmo::OcrEngine::MaxImageDimension();
    int f = UpscaleFactor(img.Width(), img.Height());
    while (f > 1 && (uint32_t)(std::max(img.Width(), img.Height()) * f) > maxDim) --f;
    const int sw = img.Width(), sh = img.Height(), w = sw * f, h = sh * f;
    if ((uint32_t)w > maxDim || (uint32_t)h > maxDim) {
        err = L"Region is too large for OCR.";
        return data;
    }
    const uint32_t bytes = (uint32_t)w * h * 4;
    wss::Buffer buf(bytes);
    buf.Length(bytes);
    auto* dst = reinterpret_cast<uint32_t*>(buf.data());
    const uint32_t* src = img.Bits();
    for (int y = 0; y < h; ++y) {
        const uint32_t* row = src + (size_t)(y / f) * sw;
        uint32_t* out = dst + (size_t)y * w;
        for (int x = 0; x < w; ++x) out[x] = row[x / f];
    }
    wgi::SoftwareBitmap sb(wgi::BitmapPixelFormat::Bgra8, w, h, wgi::BitmapAlphaMode::Premultiplied);
    sb.CopyFromBuffer(buf);
    auto result = engine.RecognizeAsync(sb).get();
    int line = 0;
    for (const auto& l : result.Lines()) {
        data.lines.push_back(l.Text().c_str());
        for (const auto& word : l.Words()) {
            auto r = word.BoundingRect();
            words.push_back({word.Text().c_str(),
                             {(LONG)(r.X / f), (LONG)(r.Y / f), (LONG)std::ceil((r.X + r.Width) / f),
                              (LONG)std::ceil((r.Y + r.Height) / f)},
                             line});
        }
        ++line;
    }
    return data;
}

template <typename T>
void RunOnWorker(BitmapPtr img, std::function<void(T, std::wstring)> done, std::function<T(OcrData&&)> shape) {
    std::thread([img = std::move(img), done = std::move(done), shape = std::move(shape)]() mutable {
        std::wstring err;
        OcrData words;
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            words = RunOcr(*img, err);
        } catch (const winrt::hresult_error& e) {
            err = e.message().c_str();
        } catch (...) {
            err = L"OCR failed.";
        }
        img.reset();
        winrt::uninit_apartment();
        T value = shape(std::move(words));
        RunOnUi([done = std::move(done), value = std::move(value), err = std::move(err)] { done(value, err); });
    }).detach();
}

}  // namespace

void RecognizeTextAsync(BitmapPtr img, std::function<void(std::wstring, std::wstring)> done) {
    RunOnWorker<std::wstring>(std::move(img), std::move(done), [](OcrData&& d) {
        std::wstring text;
        for (const auto& l : d.lines) {
            if (!text.empty()) text += L"\r\n";
            text += l;
        }
        return text;
    });
}

std::wstring RecognizeTextBlocking(const Bitmap& img) {
    std::wstring text, err;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {  // already initialized on this thread
    }
    try {
        // Long scrolling captures are taller (or wider) than the OCR engine accepts: read them in overlapping
        // strips, narrowing anything wider than the limit first.
        const int maxDim = (int)wmo::OcrEngine::MaxImageDimension();
        BitmapPtr narrowed;
        const Bitmap* src = &img;
        if (maxDim > 0 && img.Width() > maxDim) {
            const double k = (double)maxDim / img.Width();
            narrowed = Resample(img, maxDim, std::max(1, (int)(img.Height() * k)));
            if (narrowed) src = narrowed.get();
        }
        auto add = [&](const Bitmap& part) {
            for (const auto& l : RunOcr(part, err).lines) {
                if (!text.empty()) text += L' ';
                text += l;
            }
        };
        if (maxDim <= 0 || src->Height() <= maxDim) {
            add(*src);
        } else {
            const int strip = maxDim - 64, overlap = 64;  // a line cut by one strip is whole in the next
            for (int y = 0; y < src->Height(); y += strip) {
                const int h = std::min(src->Height() - y, strip + overlap);
                if (auto part = src->Crop({0, y, src->Width(), y + h})) add(*part);
                if (y + h >= src->Height()) break;
            }
        }
    } catch (...) {
    }
    return text;
}

void RecognizeWordsAsync(BitmapPtr img, std::function<void(std::vector<OcrWord>, std::wstring)> done) {
    RunOnWorker<std::vector<OcrWord>>(std::move(img), std::move(done), [](OcrData&& d) { return std::move(d.words); });
}

// ---- sensitive-data detection ----

namespace {

bool Luhn(const std::wstring& digits) {
    int sum = 0;
    bool dbl = false;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        int d = *it - L'0';
        if (dbl && (d *= 2) > 9) d -= 9;
        sum += d;
        dbl = !dbl;
    }
    return sum % 10 == 0;
}

bool LooksLikeSecret(const std::wstring& w) {
    static const wchar_t* const kPrefixes[] = {L"sk-", L"sk_", L"pk_", L"rk_", L"ghp_", L"gho_", L"ghs_", L"github_pat_",
                                               L"xox", L"AKIA", L"ASIA", L"AIza", L"eyJ", L"glpat-", L"npm_", L"hf_"};
    for (const wchar_t* p : kPrefixes)
        if (w.size() >= 12 && w.compare(0, wcslen(p), p) == 0) return true;
    if (w.size() < 20) return false;
    int letters = 0, digits = 0, upper = 0, lower = 0;
    for (wchar_t c : w) {
        if (iswdigit(c)) ++digits;
        else if (iswalpha(c)) {
            ++letters;
            if (iswupper(c)) ++upper;
            else ++lower;
        } else if (c != L'_' && c != L'-' && c != L'.' && c != L'/' && c != L'+' && c != L'=') {
            return false;
        }
    }
    return digits >= 3 && letters >= 6 && upper && lower;  // random-looking mixed token, not a long word
}

}  // namespace

std::vector<RECT> FindSensitive(const std::vector<OcrWord>& words) {
    static const std::wregex kPatterns[] = {
        std::wregex(LR"([A-Za-z0-9._%+\-]+@[A-Za-z0-9\-]+(\.[A-Za-z0-9\-]+)+)"),                    // email
        std::wregex(LR"(\b(25[0-5]|2[0-4]\d|1?\d?\d)(\.(25[0-5]|2[0-4]\d|1?\d?\d)){3}\b)"),          // IPv4
        std::wregex(LR"(\b([0-9a-fA-F]{1,4}:){3,7}[0-9a-fA-F]{1,4}\b)"),                              // IPv6
        std::wregex(LR"((\+?\d{1,3}[\s.\-]?)?\(?\d{2,4}\)?[\s.\-]?\d{3,4}[\s.\-]?\d{3,4})"),         // phone
    };
    static const std::wregex kCard(LR"(\b(\d[ \-]?){13,19}\b)");

    std::vector<RECT> out;
    std::vector<bool> hit(words.size(), false);
    for (size_t i = 0; i < words.size(); ++i)
        if (LooksLikeSecret(words[i].text)) hit[i] = true;

    // Line-level patterns can span several OCR words ("4111 1111 1111 1111", "+1 555 123 4567").
    for (size_t start = 0; start < words.size();) {
        size_t end = start;
        std::wstring line;
        std::vector<std::pair<size_t, size_t>> spans;  // char range of each word in `line`
        while (end < words.size() && words[end].line == words[start].line) {
            if (end > start) line += L' ';
            spans.push_back({line.size(), line.size() + words[end].text.size()});
            line += words[end].text;
            ++end;
        }
        auto mark = [&](size_t a, size_t b) {
            for (size_t k = 0; k < spans.size(); ++k)
                if (spans[k].first < b && spans[k].second > a) hit[start + k] = true;
        };
        for (const auto& re : kPatterns)
            for (std::wsregex_iterator it(line.begin(), line.end(), re), e; it != e; ++it) {
                const std::wstring m = it->str();
                int digits = (int)std::count_if(m.begin(), m.end(), [](wchar_t c) { return iswdigit(c); });
                if (&re == &kPatterns[3] && digits < 9) continue;  // short numbers aren't phone numbers
                mark((size_t)it->position(), (size_t)(it->position() + it->length()));
            }
        for (std::wsregex_iterator it(line.begin(), line.end(), kCard), e; it != e; ++it) {
            std::wstring digits;
            for (wchar_t c : it->str())
                if (iswdigit(c)) digits += c;
            if (digits.size() >= 13 && Luhn(digits))
                mark((size_t)it->position(), (size_t)(it->position() + it->length()));
        }
        start = end;
    }
    // Merge neighbouring hits on the same line into one box.
    for (size_t i = 0; i < words.size(); ++i) {
        if (!hit[i]) continue;
        RECT r = words[i].rect;
        while (i + 1 < words.size() && hit[i + 1] && words[i + 1].line == words[i].line) {
            ++i;
            UnionRect(&r, &r, &words[i].rect);
        }
        InflateRect(&r, 3, 3);
        out.push_back(r);
    }
    return out;
}

}  // namespace ather
