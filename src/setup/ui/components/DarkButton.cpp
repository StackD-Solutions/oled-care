// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 StackD Solutions

#include "setup/ui/components/DarkButton.h"

#include "setup/ui/Theme.h"

using namespace theme;

namespace ui {

void DrawButton(DRAWITEMSTRUCT* dis, const wchar_t* text, bool primary) {
    bool disabled = !IsWindowEnabled(dis->hwndItem);
    bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    RECT rc = dis->rcItem;
    FillRect(dis->hDC, &rc, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(dis->hDC, &rc, bg);
    DeleteObject(bg);

    COLORREF fill = primary ? (disabled ? RGB(70, 80, 70) : (pressed ? kGreenHot : kGreen))
                            : (pressed ? RGB(55, 55, 55) : kPanel);
    HBRUSH br = CreateSolidBrush(fill);
    HPEN pen = CreatePen(PS_SOLID, 1, primary ? fill : kLine);
    HGDIOBJ ob = SelectObject(dis->hDC, br);
    HGDIOBJ op = SelectObject(dis->hDC, pen);
    RoundRect(dis->hDC, rc.left, rc.top, rc.right, rc.bottom, S(4), S(4));
    SelectObject(dis->hDC, ob);
    SelectObject(dis->hDC, op);
    DeleteObject(br);
    DeleteObject(pen);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, disabled ? kTextDim : (primary ? RGB(22, 34, 0) : kText));
    HFONT of = static_cast<HFONT>(SelectObject(dis->hDC, g_fBody));
    DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dis->hDC, of);
}

}  // namespace ui
