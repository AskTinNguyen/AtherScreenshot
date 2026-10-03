#include "overlay.h"

#include <windowsx.h>

#include <algorithm>

#include <cmath>

#include "capture.h"
#include "output.h"

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotOverlay";
constexpr int kMagSrc = 15;  // odd: source pixels sampled around the cursor
constexpr int kBand = 256;   // max rows painted per back-buffer pass (bounds memory)

class Overlay;
Overlay* g_active = nullptr;

class Overlay {
public:
    Overlay(OverlayMode m, BitmapPtr shot, RECT v, OverlayOptions o, std::function<void(const OverlayResult&)> cb)
        : mode_(m), src_(std::move(shot)), virt_(v), opt_(o), done_(std::move(cb)) {}

    bool Create();
    LRESULT Proc(UINT m, WPARAM w, LPARAM l);
    HWND hwnd = nullptr;

private:
    int S(int v) const { return Px(scale_, v); }
    RECT Selection() const {
        return {std::min(anchor_.x, cur_.x), std::min(anchor_.y, cur_.y), std::max(anchor_.x, cur_.x) + 1,
                std::max(anchor_.y, cur_.y) + 1};
    }
    bool SelectionIsClick() const { return RectW(Selection()) < 4 && RectH(Selection()) < 4; }
    RECT Highlight() const {
        if (mode_ != OverlayMode::Region) return {};
        return dragging_ && !SelectionIsClick() ? Selection() : hover_;
    }
    const SnapTarget* SnapAt(POINT p) const {
        for (const auto& s : snaps_)
            if (PtInRect(&s.rect, p)) return &s;
        return nullptr;
    }
    void UpdateHover() {
        const SnapTarget* s = SnapAt(cur_);
        hover_ = s ? s->rect : RECT{};
        hoverWnd_ = s ? s->hwnd : nullptr;
    }
    // Ruler: the measured segment, Shift snaps it horizontal/vertical.
    POINT RulerEnd() const {
        if (GetKeyState(VK_SHIFT) >= 0) return rulerB_;
        return std::abs(rulerB_.x - rulerA_.x) >= std::abs(rulerB_.y - rulerA_.y) ? POINT{rulerB_.x, rulerA_.y}
                                                                                   : POINT{rulerA_.x, rulerB_.y};
    }
    std::wstring RulerText() const;
    RECT RulerLabelRect() const;
    RECT RulerBounds() const;
    RECT MonitorAt(POINT p) const {
        RECT r = MonitorRectAt({p.x + virt_.left, p.y + virt_.top});
        OffsetRect(&r, -virt_.left, -virt_.top);
        return r;
    }
    int BorderW() const { return std::max(1, S(1)); }
    std::wstring LabelText(const RECT& r) const {
        return std::to_wstring(RectW(r)) + L" × " + std::to_wstring(RectH(r));
    }
    RECT LabelRect(const RECT& hi) const;
    RECT MagRect() const;

    void BuildDecor(const RECT& hi, std::vector<RECT>& out) const;
    void Update();
    void Paint();
    void PaintRect(HDC hdc, const RECT& r);
    void DrawDecor(HDC dc, const RECT& hi);
    void DrawMagnifier(HDC dc);
    void MoveCursorBy(int dx, int dy);

    void FinishRect(RECT r, HWND window = nullptr);
    void FinishColor();
    void Cancel() { Finish(OverlayResult{}); }
    void Finish(const OverlayResult& r);

    OverlayMode mode_;
    BitmapPtr src_, dim_;
    RECT virt_;
    OverlayOptions opt_;
    std::function<void(const OverlayResult&)> done_;

    int W_ = 0, H_ = 0;
    HDC srcDC_ = nullptr, dimDC_ = nullptr, backDC_ = nullptr, measureDC_ = nullptr;
    HGDIOBJ srcOld_ = nullptr, dimOld_ = nullptr, backOld_ = nullptr, measureOld_ = nullptr;
    HBITMAP back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    HFONT font_ = nullptr, fontSmall_ = nullptr;
    HBRUSH accent_ = nullptr, cross_ = nullptr;
    float scale_ = 1;

    std::vector<SnapTarget> snaps_;
    RECT hover_{};
    HWND hoverWnd_ = nullptr;
    POINT anchor_{}, cur_{};
    bool dragging_ = false, wasActive_ = false;
    bool hasRuler_ = false;
    POINT rulerA_{}, rulerB_{};
    RECT prevHi_{};
    std::vector<RECT> prevDecor_;
};

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE) {
        auto* o = static_cast<Overlay*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        o->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)o);
    }
    auto* o = reinterpret_cast<Overlay*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    return o ? o->Proc(m, w, l) : DefWindowProcW(h, m, w, l);
}

bool Overlay::Create() {
    W_ = RectW(virt_);
    H_ = RectH(virt_);
    dim_ = Bitmap::Create(W_, H_);
    if (!dim_) return false;
    {
        // 50% darken with a faint cool tint; trivially auto-vectorized.
        const uint32_t* a = src_->Bits();
        uint32_t* d = dim_->Bits();
        const size_t n = (size_t)W_ * H_;
        for (size_t i = 0; i < n; ++i) d[i] = (((a[i] >> 1) & 0x007F7F7Fu) + 0x00040404u) | 0xFF000000u;
    }
    srcDC_ = CreateCompatibleDC(nullptr);
    srcOld_ = SelectObject(srcDC_, src_->Handle());
    dimDC_ = CreateCompatibleDC(nullptr);
    dimOld_ = SelectObject(dimDC_, dim_->Handle());

    if (mode_ == OverlayMode::Region) {
        snaps_ = EnumSnapRects();
        for (auto& s : snaps_) OffsetRect(&s.rect, -virt_.left, -virt_.top);
    }
    POINT pt;
    GetCursorPos(&pt);
    scale_ = DpiScaleAt(pt);
    cur_ = {pt.x - virt_.left, pt.y - virt_.top};
    UpdateHover();

    font_ = MakeFont(S(13), FW_SEMIBOLD);
    fontSmall_ = MakeFont(S(12), FW_NORMAL, L"Consolas");
    accent_ = CreateSolidBrush(theme::kAccent);
    cross_ = CreateSolidBrush(theme::kAccentSoft);
    measureDC_ = CreateCompatibleDC(nullptr);
    measureOld_ = SelectObject(measureDC_, font_);

    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = OverlayProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    prevHi_ = Highlight();
    BuildDecor(prevHi_, prevDecor_);
    if (!CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass, L"AtherScreenshot", WS_POPUP, virt_.left, virt_.top,
                         W_, H_, nullptr, nullptr, GetModuleHandleW(nullptr), this))
        return false;
    PrepareChromeless(hwnd, false);
    ShowWithoutFlash(hwnd, true);
    return true;
}

RECT Overlay::LabelRect(const RECT& hi) const {
    std::wstring t = LabelText(hi);
    SIZE sz{};
    GetTextExtentPoint32W(measureDC_, t.c_str(), (int)t.size(), &sz);
    int w = sz.cx + S(16), h = sz.cy + S(8);
    int x = hi.left, y = hi.top - h - S(6);
    RECT mon = MonitorAt({hi.left, hi.top});
    if (y < mon.top) {
        x += S(6);
        y = hi.top + S(6);
    }
    x = std::clamp<int>(x, 0, std::max(0, W_ - w));
    return {x, y, x + w, y + h};
}

RECT Overlay::MagRect() const {
    int cell = std::max(4, S(8)), mag = kMagSrc * cell;
    int w = mag + 2, h = mag + 2 + S(42), off = S(22);
    RECT mon = MonitorAt(cur_);
    int x = cur_.x + off, y = cur_.y + off;
    if (x + w > mon.right) x = cur_.x - off - w;
    if (y + h > mon.bottom) y = cur_.y - off - h;
    return {x, y, x + w, y + h};
}

std::wstring Overlay::RulerText() const {
    const POINT b = RulerEnd();
    const int dx = std::abs(b.x - rulerA_.x), dy = std::abs(b.y - rulerA_.y);
    const double len = std::hypot((double)dx, (double)dy);
    const double ang = std::atan2((double)(rulerA_.y - b.y), (double)(b.x - rulerA_.x)) * 180.0 / 3.14159265358979;
    wchar_t t[128];
    swprintf_s(t, L"%d × %d   ·   %.1f px   ·   %.1f°", dx + 1, dy + 1, len, ang);
    return t;
}

RECT Overlay::RulerLabelRect() const {
    std::wstring t = RulerText();
    SIZE sz{};
    GetTextExtentPoint32W(measureDC_, t.c_str(), (int)t.size(), &sz);
    const POINT b = RulerEnd();
    int w = sz.cx + S(16), h = sz.cy + S(8);
    int x = std::clamp<int>(b.x + S(14), 0, std::max(0, W_ - w));
    int y = std::clamp<int>(b.y - h - S(10), 0, std::max(0, H_ - h));
    return {x, y, x + w, y + h};
}

RECT Overlay::RulerBounds() const {
    const POINT b = RulerEnd();
    RECT r{std::min(rulerA_.x, b.x), std::min(rulerA_.y, b.y), std::max(rulerA_.x, b.x) + 1,
           std::max(rulerA_.y, b.y) + 1};
    InflateRect(&r, S(6), S(6));
    return r;
}

void Overlay::BuildDecor(const RECT& hi, std::vector<RECT>& out) const {
    out.clear();
    if (mode_ == OverlayMode::Ruler && hasRuler_) {
        out.push_back(RulerBounds());
        out.push_back(RulerLabelRect());
    }
    if (!IsRectEmpty(&hi)) {
        int b = BorderW();
        RECT o = hi;
        InflateRect(&o, b, b);
        out.push_back({o.left, o.top, o.right, hi.top});
        out.push_back({o.left, hi.bottom, o.right, o.bottom});
        out.push_back({o.left, hi.top, hi.left, hi.bottom});
        out.push_back({hi.right, hi.top, o.right, hi.bottom});
        out.push_back(LabelRect(hi));
    }
    if (opt_.crosshair) {
        out.push_back({0, cur_.y, W_, cur_.y + 1});
        out.push_back({cur_.x, 0, cur_.x + 1, H_});
    }
    if (opt_.magnifier || mode_ != OverlayMode::Region) out.push_back(MagRect());
}

// Invalidate only what changed: the XOR of old/new highlight plus old/new decorations.
void Overlay::Update() {
    RECT hi = Highlight();
    std::vector<RECT> decor;
    BuildDecor(hi, decor);
    HRGN rgn = CreateRectRgnIndirect(&prevHi_);
    HRGN tmp = CreateRectRgnIndirect(&hi);
    CombineRgn(rgn, rgn, tmp, RGN_XOR);
    for (const auto* list : {&prevDecor_, &decor})
        for (const auto& r : *list) {
            SetRectRgn(tmp, r.left, r.top, r.right, r.bottom);
            CombineRgn(rgn, rgn, tmp, RGN_OR);
        }
    InvalidateRgn(hwnd, rgn, FALSE);
    DeleteObject(tmp);
    DeleteObject(rgn);
    prevHi_ = hi;
    prevDecor_ = std::move(decor);
    UpdateWindow(hwnd);
}

void Overlay::Paint() {
    HRGN upd = CreateRectRgn(0, 0, 0, 0);
    GetUpdateRgn(hwnd, upd, FALSE);
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    DWORD size = GetRegionData(upd, 0, nullptr);
    std::vector<BYTE> buf(size);
    auto* rd = reinterpret_cast<RGNDATA*>(buf.data());
    if (size && GetRegionData(upd, size, rd) && rd->rdh.nCount <= 96) {
        const RECT* rects = reinterpret_cast<const RECT*>(rd->Buffer);
        for (DWORD i = 0; i < rd->rdh.nCount; ++i) PaintRect(hdc, rects[i]);
    } else {
        PaintRect(hdc, ps.rcPaint);
    }
    EndPaint(hwnd, &ps);
    DeleteObject(upd);
}

void Overlay::PaintRect(HDC hdc, const RECT& r) {
    const RECT hi = Highlight();
    for (LONG top = r.top; top < r.bottom; top += kBand) {
        RECT band{r.left, top, r.right, std::min<LONG>(top + kBand, r.bottom)};
        int w = RectW(band), h = RectH(band);
        if (w <= 0 || h <= 0) continue;
        if (w > backW_ || h > backH_) {
            if (backDC_) {
                SelectObject(backDC_, backOld_);
                DeleteObject(back_);
            } else {
                backDC_ = CreateCompatibleDC(hdc);
            }
            backW_ = std::max(backW_, w);
            backH_ = std::max(backH_, h);
            back_ = CreateCompatibleBitmap(hdc, backW_, backH_);
            backOld_ = SelectObject(backDC_, back_);
        }
        SetViewportOrgEx(backDC_, -band.left, -band.top, nullptr);
        BitBlt(backDC_, band.left, band.top, w, h, dimDC_, band.left, band.top, SRCCOPY);
        RECT x;
        if (IntersectRect(&x, &hi, &band))
            BitBlt(backDC_, x.left, x.top, RectW(x), RectH(x), srcDC_, x.left, x.top, SRCCOPY);
        DrawDecor(backDC_, hi);
        SetViewportOrgEx(backDC_, 0, 0, nullptr);
        BitBlt(hdc, band.left, band.top, w, h, backDC_, 0, 0, SRCCOPY);
    }
}

void Overlay::DrawDecor(HDC dc, const RECT& hi) {
    if (opt_.crosshair) {
        HGDIOBJ ob = SelectObject(dc, cross_);
        PatBlt(dc, 0, cur_.y, W_, 1, PATCOPY);
        PatBlt(dc, cur_.x, 0, 1, H_, PATCOPY);
        SelectObject(dc, ob);
    }
    if (!IsRectEmpty(&hi)) {
        RECT o = hi;
        int b = BorderW();
        InflateRect(&o, b, b);
        FrameSolid(dc, o, theme::kAccent, b);

        RECT lr = LabelRect(hi);
        FillRounded(dc, lr, S(5), theme::kSurface, theme::kBorder);
        HGDIOBJ of = SelectObject(dc, font_);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, theme::kText);
        std::wstring t = LabelText(hi);
        DrawTextW(dc, t.c_str(), (int)t.size(), &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    if (mode_ == OverlayMode::Ruler && hasRuler_) {
        const POINT b = RulerEnd();
        // dotted bounding box, solid measured line, square end caps
        HPEN dot = CreatePen(PS_DOT, 1, theme::kAccentSoft);
        HGDIOBJ op = SelectObject(dc, dot);
        HGDIOBJ obr = SelectObject(dc, GetStockObject(NULL_BRUSH));
        SetBkMode(dc, TRANSPARENT);
        Rectangle(dc, std::min(rulerA_.x, b.x), std::min(rulerA_.y, b.y), std::max(rulerA_.x, b.x) + 1,
                  std::max(rulerA_.y, b.y) + 1);
        HPEN line = CreatePen(PS_SOLID, std::max(1, S(2)), theme::kAccent);
        SelectObject(dc, line);
        MoveToEx(dc, rulerA_.x, rulerA_.y, nullptr);
        LineTo(dc, b.x, b.y);
        SelectObject(dc, op);
        SelectObject(dc, obr);
        DeleteObject(dot);
        DeleteObject(line);
        const int e = S(3);
        for (POINT p : {rulerA_, b}) FillSolid(dc, {p.x - e, p.y - e, p.x + e + 1, p.y + e + 1}, theme::kAccent);
        RECT lr = RulerLabelRect();
        FillRounded(dc, lr, S(5), theme::kSurface, theme::kBorder);
        HGDIOBJ of = SelectObject(dc, font_);
        SetTextColor(dc, theme::kText);
        std::wstring t = RulerText();
        DrawTextW(dc, t.c_str(), (int)t.size(), &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, of);
    }
    if (opt_.magnifier || mode_ != OverlayMode::Region) DrawMagnifier(dc);
}

void Overlay::DrawMagnifier(HDC dc) {
    const RECT m = MagRect();
    const int cell = std::max(4, S(8)), mag = kMagSrc * cell, half = kMagSrc / 2;
    FillSolid(dc, m, theme::kSurface);
    // Clamp the sampled square to the bitmap so edges show black instead of garbage.
    const int cx = cur_.x, cy = cur_.y;
    int sx0 = std::max(0, cx - half), sy0 = std::max(0, cy - half);
    int sx1 = std::min(W_, cx + half + 1), sy1 = std::min(H_, cy + half + 1);
    RECT inner{m.left + 1, m.top + 1, m.left + 1 + mag, m.top + 1 + mag};
    FillSolid(dc, inner, RGB(0, 0, 0));
    if (sx1 > sx0 && sy1 > sy0) {
        SetStretchBltMode(dc, COLORONCOLOR);
        StretchBlt(dc, inner.left + (sx0 - (cur_.x - half)) * cell, inner.top + (sy0 - (cur_.y - half)) * cell,
                   (sx1 - sx0) * cell, (sy1 - sy0) * cell, srcDC_, sx0, sy0, sx1 - sx0, sy1 - sy0, SRCCOPY);
    }
    RECT c{inner.left + half * cell, inner.top + half * cell, 0, 0};
    c.right = c.left + cell;
    c.bottom = c.top + cell;
    FrameSolid(dc, c, RGB(0, 0, 0));
    InflateRect(&c, 1, 1);
    FrameSolid(dc, c, RGB(255, 255, 255));
    FrameSolid(dc, m, theme::kBorder);

    uint32_t p = src_->Pixel(cur_.x, cur_.y);
    int r = (p >> 16) & 255, g = (p >> 8) & 255, b = p & 255;
    // Two info lines: swatch + hex, then screen coordinates.
    const int lineH = (m.bottom - inner.bottom - S(4)) / 2, top1 = inner.bottom + S(2), top2 = top1 + lineH;
    const int sw = S(12);
    RECT swatch{m.left + S(8), top1 + (lineH - sw) / 2, 0, 0};
    swatch.right = swatch.left + sw;
    swatch.bottom = swatch.top + sw;
    FillSolid(dc, swatch, RGB(r, g, b));
    FrameSolid(dc, swatch, theme::kBorder);
    wchar_t hex[16], pos[32];
    swprintf_s(hex, L"#%02X%02X%02X", r, g, b);
    swprintf_s(pos, L"%ld, %ld", cur_.x + virt_.left, cur_.y + virt_.top);
    HGDIOBJ of = SelectObject(dc, fontSmall_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, theme::kText);
    RECT t1{swatch.right + S(6), top1, m.right - S(4), top1 + lineH};
    DrawTextW(dc, hex, -1, &t1, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(dc, theme::kMuted);
    RECT t2{m.left + S(8), top2, m.right - S(4), top2 + lineH};
    DrawTextW(dc, pos, -1, &t2, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, of);
}

void Overlay::MoveCursorBy(int dx, int dy) {
    POINT pt;
    GetCursorPos(&pt);
    SetCursorPos(pt.x + dx, pt.y + dy);  // generates WM_MOUSEMOVE
}

void Overlay::FinishRect(RECT r, HWND window) {
    RECT bounds{0, 0, W_, H_};
    if (!IntersectRect(&r, &r, &bounds)) return Cancel();
    OverlayResult res;
    res.ok = true;
    res.rect = r;
    res.window = window;
    OffsetRect(&res.rect, virt_.left, virt_.top);
    Finish(res);
}

void Overlay::FinishColor() {
    uint32_t p = src_->Pixel(cur_.x, cur_.y);
    OverlayResult res;
    res.ok = true;
    res.color = RGB((p >> 16) & 255, (p >> 8) & 255, p & 255);
    Finish(res);
}

// Tears down the window and deletes `this`; callers must return immediately afterwards.
void Overlay::Finish(const OverlayResult& r) {
    auto cb = std::move(done_);
    HWND h = hwnd;
    SetWindowLongPtrW(h, GWLP_USERDATA, 0);
    if (GetCapture() == h) ReleaseCapture();
    DestroyWindow(h);
    SelectObject(srcDC_, srcOld_);
    DeleteDC(srcDC_);
    SelectObject(dimDC_, dimOld_);
    DeleteDC(dimDC_);
    SelectObject(measureDC_, measureOld_);
    DeleteDC(measureDC_);
    if (backDC_) {
        SelectObject(backDC_, backOld_);
        DeleteObject(back_);
        DeleteDC(backDC_);
    }
    DeleteObject(font_);
    DeleteObject(fontSmall_);
    DeleteObject(accent_);
    DeleteObject(cross_);
    g_active = nullptr;
    delete this;
    if (cb) cb(r);
}

LRESULT Overlay::Proc(UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_CROSS)); return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(); return 0;
        case WM_DPICHANGED: return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) != WA_INACTIVE) wasActive_ = true;
            else if (wasActive_) Cancel();
            return 0;
        case WM_MOUSEMOVE: {
            POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            if (p.x == cur_.x && p.y == cur_.y) return 0;
            cur_ = p;
            if (mode_ == OverlayMode::Region) UpdateHover();
            if (mode_ == OverlayMode::Ruler && dragging_) rulerB_ = cur_;
            Update();
            return 0;
        }
        case WM_LBUTTONDOWN:
            if (mode_ == OverlayMode::ColorPick) {
                FinishColor();
                return 0;
            }
            dragging_ = true;
            anchor_ = cur_;
            if (mode_ == OverlayMode::Ruler) {  // each drag starts a new measurement
                hasRuler_ = true;
                rulerA_ = rulerB_ = cur_;
            }
            SetCapture(hwnd);
            Update();
            return 0;
        case WM_LBUTTONUP:
            if (dragging_ && mode_ == OverlayMode::Ruler) {
                dragging_ = false;
                ReleaseCapture();
                rulerB_ = RulerEnd();  // bake in the Shift snap
                Update();
            } else if (dragging_) {
                const bool click = SelectionIsClick();
                RECT r = click ? hover_ : Selection();
                dragging_ = false;
                FinishRect(r, click ? hoverWnd_ : nullptr);
            }
            return 0;
        case WM_RBUTTONDOWN:
            if (dragging_) {
                dragging_ = false;
                ReleaseCapture();
                Update();
            } else {
                Cancel();
            }
            return 0;
        case WM_KEYDOWN: {
            const bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
            const int step = shift ? 10 : 1;
            switch (w) {
                case VK_ESCAPE:
                    if (dragging_) {
                        dragging_ = false;
                        ReleaseCapture();
                        Update();
                    } else {
                        Cancel();
                    }
                    return 0;
                case VK_RETURN:
                    if (mode_ == OverlayMode::ColorPick) FinishColor();
                    else if (mode_ == OverlayMode::Ruler) Cancel();
                    else if (dragging_ && !SelectionIsClick()) FinishRect(Selection());
                    else FinishRect(hover_, hoverWnd_);
                    return 0;
                case VK_SPACE:
                    if (mode_ == OverlayMode::Region) FinishRect(MonitorAt(cur_));
                    return 0;
                case 'A':
                    if (ctrl && mode_ == OverlayMode::Region) FinishRect({0, 0, W_, H_});
                    return 0;
                case 'C':
                    if (mode_ == OverlayMode::ColorPick) FinishColor();
                    else if (mode_ == OverlayMode::Ruler && hasRuler_) {
                        CopyTextToClipboard(hwnd, RulerText());
                        Cancel();
                    }
                    return 0;
                case VK_SHIFT:
                    if (mode_ == OverlayMode::Ruler && dragging_) Update();  // snap preview
                    return 0;
                case VK_LEFT: MoveCursorBy(-step, 0); return 0;
                case VK_RIGHT: MoveCursorBy(step, 0); return 0;
                case VK_UP: MoveCursorBy(0, -step); return 0;
                case VK_DOWN: MoveCursorBy(0, step); return 0;
            }
            return 0;
        }
    }
    return DefWindowProcW(hwnd, m, w, l);
}

}  // namespace

bool ShowOverlay(OverlayMode mode, BitmapPtr shot, const RECT& virt, const OverlayOptions& opt,
                 std::function<void(const OverlayResult&)> done) {
    if (g_active || !shot) return false;
    auto* o = new Overlay(mode, std::move(shot), virt, opt, std::move(done));
    g_active = o;
    if (!o->Create()) {
        g_active = nullptr;
        if (o->hwnd) DestroyWindow(o->hwnd);
        delete o;  // leaks GDI objects only on a failed create; acceptable for an unrecoverable path
        return false;
    }
    return true;
}

bool OverlayActive() { return g_active != nullptr; }

}  // namespace ather
