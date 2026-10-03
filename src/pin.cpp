#include "pin.h"

#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "editor.h"
#include "output.h"

namespace ather {
namespace {

constexpr wchar_t kClass[] = L"AtherScreenshotPin";
enum { kMenuCopy = 1, kMenuSave, kMenuEdit, kMenuReset, kMenuClose, kMenuCloseAll };

struct Pin {
    BitmapPtr img;
    double zoom = 1;
    int alpha = 255;
};

std::vector<HWND> g_pins;

SIZE ZoomedSize(const Pin& p) {
    return {std::max(1L, std::lround(p.img->Width() * p.zoom)), std::max(1L, std::lround(p.img->Height() * p.zoom))};
}

void SetZoom(HWND h, Pin& p, double z, POINT anchor) {
    z = std::clamp(z, 0.1, 8.0);
    if (std::abs(z - 1.0) < 0.04) z = 1.0;
    if (z == p.zoom) return;
    RECT wr;
    GetWindowRect(h, &wr);
    double fx = (anchor.x - wr.left) / (double)RectW(wr), fy = (anchor.y - wr.top) / (double)RectH(wr);
    p.zoom = z;
    SIZE sz = ZoomedSize(p);
    SetWindowPos(h, nullptr, anchor.x - std::lround(fx * sz.cx), anchor.y - std::lround(fy * sz.cy), sz.cx, sz.cy,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(h, nullptr, FALSE);
}

void SaveAs(HWND h, const Pin& p) {
    wchar_t file[MAX_PATH] = L"pinned.png";
    OPENFILENAMEW ofn{sizeof(ofn)};
    ofn.hwndOwner = h;
    ofn.lpstrFilter = L"PNG image (*.png)\0*.png\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"png";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn)) SavePng(*p.img, file);
}

void Command(HWND h, Pin& p, int id) {
    switch (id) {
        case kMenuCopy: CopyImageToClipboard(h, *p.img); break;
        case kMenuSave: SaveAs(h, p); break;
        case kMenuEdit: OpenEditor(p.img); break;
        case kMenuReset: {
            RECT wr;
            GetWindowRect(h, &wr);
            SetZoom(h, p, 1.0, {wr.left, wr.top});
            p.alpha = 255;
            SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
            break;
        }
        case kMenuClose: DestroyWindow(h); break;
        case kMenuCloseAll: CloseAllPins(); break;
    }
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCCREATE)
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
    auto* p = reinterpret_cast<Pin*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!p) return DefWindowProcW(h, m, w, l);
    switch (m) {
        case WM_NCHITTEST: return HTCAPTION;  // drag anywhere
        case WM_NCLBUTTONDBLCLK:
        case WM_NCMBUTTONUP: DestroyWindow(h); return 0;
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            AppendMenuW(menu, MF_STRING, kMenuCopy, L"Copy\tCtrl+C");
            AppendMenuW(menu, MF_STRING, kMenuSave, L"Save as…\tCtrl+S");
            AppendMenuW(menu, MF_STRING, kMenuEdit, L"Annotate…\tE");
            AppendMenuW(menu, MF_STRING, kMenuReset, L"Reset zoom && opacity\t0");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, kMenuClose, L"Close\tEsc");
            AppendMenuW(menu, MF_STRING, kMenuCloseAll, L"Close all pins");
            POINT pt;
            GetCursorPos(&pt);
            int id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(menu);
            if (id) Command(h, *p, id);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(w);
            if (GET_KEYSTATE_WPARAM(w) & MK_CONTROL) {
                p->alpha = std::clamp(p->alpha + (d > 0 ? 20 : -20), 40, 255);
                SetLayeredWindowAttributes(h, 0, (BYTE)p->alpha, LWA_ALPHA);
            } else {
                SetZoom(h, *p, p->zoom * (d > 0 ? 1.1 : 1 / 1.1), {GET_X_LPARAM(l), GET_Y_LPARAM(l)});
            }
            return 0;
        }
        case WM_KEYDOWN: {
            bool ctrl = GetKeyState(VK_CONTROL) < 0;
            if (w == VK_ESCAPE) Command(h, *p, kMenuClose);
            else if (ctrl && w == 'C') Command(h, *p, kMenuCopy);
            else if (ctrl && w == 'S') Command(h, *p, kMenuSave);
            else if (!ctrl && w == 'E') Command(h, *p, kMenuEdit);
            else if (w == '0' || w == VK_NUMPAD0) Command(h, *p, kMenuReset);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            {
                MemDC s(p->img->Handle(), dc);
                if (rc.right == p->img->Width() && rc.bottom == p->img->Height()) {
                    BitBlt(dc, 0, 0, rc.right, rc.bottom, s, 0, 0, SRCCOPY);
                } else {
                    SetStretchBltMode(dc, p->zoom < 1 ? HALFTONE : COLORONCOLOR);
                    SetBrushOrgEx(dc, 0, 0, nullptr);
                    StretchBlt(dc, 0, 0, rc.right, rc.bottom, s, 0, 0, p->img->Width(), p->img->Height(), SRCCOPY);
                }
            }
            FrameSolid(dc, rc, theme::kAccent);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_DPICHANGED: return 0;
        case WM_NCDESTROY:
            g_pins.erase(std::remove(g_pins.begin(), g_pins.end(), h), g_pins.end());
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            delete p;
            return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void PinImage(BitmapPtr img, const RECT* at) {
    if (!img) return;
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_SIZEALL);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    auto* pin = new Pin{img};
    int x, y;
    if (at) {
        x = at->left;
        y = at->top;
    } else {
        POINT pt;
        GetCursorPos(&pt);
        RECT work = MonitorRectAt(pt, true);
        double fit = std::min((RectW(work) * 0.8) / img->Width(), (RectH(work) * 0.8) / img->Height());
        if (fit < 1) pin->zoom = fit;
        SIZE sz = ZoomedSize(*pin);
        x = std::clamp<int>(pt.x - sz.cx / 2, work.left, std::max<int>(work.left, work.right - sz.cx));
        y = std::clamp<int>(pt.y - sz.cy / 2, work.top, std::max<int>(work.top, work.bottom - sz.cy));
    }
    SIZE sz = ZoomedSize(*pin);
    HWND h = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kClass, L"Pinned capture", WS_POPUP, x,
                             y, sz.cx, sz.cy, nullptr, nullptr, GetModuleHandleW(nullptr), pin);
    if (!h) {
        delete pin;
        return;
    }
    SetLayeredWindowAttributes(h, 0, 255, LWA_ALPHA);
    PrepareChromeless(h, false);
    g_pins.push_back(h);
    ShowWithoutFlash(h, true);
}

void CloseAllPins() {
    auto pins = g_pins;
    for (HWND h : pins) DestroyWindow(h);
}

}  // namespace ather
