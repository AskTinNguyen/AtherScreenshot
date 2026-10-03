#pragma once
#include "common.h"

namespace ather {

// The A⁵ mark: grey "A", lime light-bar "I", lime superscript "5".
// `tile` = draw it on the dark rounded app tile (icons); otherwise transparent background.
BitmapPtr RenderLogo(int size, bool tile);          // straight (non-premultiplied) alpha
void DrawLogo(HDC dc, const RECT& box);             // into a window, on the current background
HICON CreateLogoIcon(int size);
bool WriteLogoIco(const std::wstring& path);        // multi-size .ico (16…256, PNG frames)

}  // namespace ather
