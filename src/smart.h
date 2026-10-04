#pragma once
#include "library.h"

namespace ather {

// On-device smarts for the gallery: suggested tags, and search that understands related words and dates.
// Port of macos/Sources/AtherScreenshot/Smart.swift. The Mac version also asks a built-in word model for
// neighbors; Windows has none built in, so related words come from the curated groups and a simple stem.

namespace autotag {
// Suggested tags, computed (never stored) and cached per path.
std::vector<std::wstring> Suggest(const std::wstring& path, const ItemMeta& m);
// Suggestions not already on the capture and not turned down.
std::vector<std::wstring> Pending(const std::wstring& path, const ItemMeta& m);
std::vector<std::wstring> Compute(const std::wstring& path, const ItemMeta& m);
// Whole-word match in a lower-case haystack padded with spaces, so "pay" doesn't match "display".
bool Has(const std::wstring& hay, const std::wstring& word);
}  // namespace autotag

// A parsed search: words with related words, plus a date range from phrases like "last week".
struct SearchQuery {
    struct Term {
        std::wstring word;
        std::vector<std::wstring> related;
    };
    std::vector<Term> terms;
    std::optional<double> since, until;  // epoch seconds

    static SearchQuery Parse(const std::wstring& text, double now = 0);
    static std::vector<std::wstring> Related(const std::wstring& word);
    bool IsEmpty() const { return terms.empty() && !since; }
    // `hay` is lower-case searchable text. Exact when every word appears as typed; related when some only
    // match a related word.
    MatchKind Match(const std::wstring& hay, double mtime) const;
};

double NowEpoch();

}  // namespace ather
