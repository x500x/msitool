#include "ui.h"

#include <cwchar>
#include <cwctype>

namespace ui {

void setClipboardText(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) return;
    EmptyClipboard();
    size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (h) {
        void* p = GlobalLock(h);
        if (p) {
            memcpy(p, text.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        } else {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

namespace {

int textWidth(HDC dc, HFONT font, const std::wstring& s) {
    if (s.empty()) return 0;
    HGDIOBJ old = font ? SelectObject(dc, font) : nullptr;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    if (old) SelectObject(dc, old);
    return sz.cx;
}

RECT inset(const RECT& r, int dx, int dy) {
    return RECT{r.left + dx, r.top + dy, r.right - dx, r.bottom - dy};
}

// ---- key names ------------------------------------------------------------
// Canonical, layout-neutral names used both for display and for the value that
// is persisted in the registry. Only keys that can be typed or reached with a
// modifier are listed; unknown VK codes map to an empty string and are refused
// by HotkeyField.
std::wstring keyName(int vk) {
    struct Named { int vk; const wchar_t* name; };
    static const Named named[] = {
        {VK_SPACE, L"Space"}, {VK_RETURN, L"Enter"}, {VK_TAB, L"Tab"},
        {VK_ESCAPE, L"Esc"}, {VK_BACK, L"Backspace"}, {VK_DELETE, L"Delete"},
        {VK_INSERT, L"Insert"}, {VK_HOME, L"Home"}, {VK_END, L"End"},
        {VK_PRIOR, L"PageUp"}, {VK_NEXT, L"PageDown"},
        {VK_LEFT, L"Left"}, {VK_RIGHT, L"Right"},
        {VK_UP, L"Up"}, {VK_DOWN, L"Down"},
        {VK_OEM_COMMA, L","}, {VK_OEM_PERIOD, L"."}, {VK_OEM_1, L";"},
        {VK_OEM_2, L"/"}, {VK_OEM_3, L"`"}, {VK_OEM_4, L"["},
        {VK_OEM_5, L"\\"}, {VK_OEM_6, L"]"}, {VK_OEM_7, L"'"},
        {VK_OEM_MINUS, L"-"}, {VK_OEM_PLUS, L"="}, {VK_OEM_102, L"\\"},
    };
    for (const Named& n : named)
        if (n.vk == vk) return n.name;
    if (vk >= 'A' && vk <= 'Z')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= '0' && vk <= '9')
        return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9)
        return L"Num" + std::wstring(1, static_cast<wchar_t>(L'0' + (vk - VK_NUMPAD0)));
    if (vk >= VK_F1 && vk <= VK_F24)
        return L"F" + std::to_wstring(vk - VK_F1 + 1);
    return L"";
}

int keyFromName(const std::wstring& raw) {
    std::wstring s = util::trim(raw);
    if (s.empty()) return 0;
    if (s.size() == 1) {
        wchar_t c = static_cast<wchar_t>(towupper(s[0]));
        if ((c >= L'A' && c <= L'Z') || (c >= L'0' && c <= L'9')) return c;
        switch (c) {
        case L',': return VK_OEM_COMMA;
        case L'.': return VK_OEM_PERIOD;
        case L';': return VK_OEM_1;
        case L'/': return VK_OEM_2;
        case L'`': return VK_OEM_3;
        case L'[': return VK_OEM_4;
        case L'\\': return VK_OEM_5;
        case L']': return VK_OEM_6;
        case L'\'': return VK_OEM_7;
        case L'-': return VK_OEM_MINUS;
        case L'=': return VK_OEM_PLUS;
        default: return 0;
        }
    }
    for (int vk = 1; vk < 256; ++vk) {
        if (util::iequals(keyName(vk), s)) return vk;
    }
    return 0;
}

}  // namespace

// ---------------------------------------------------------------------------
// Hotkey
// ---------------------------------------------------------------------------
bool Hotkey::usable() const noexcept {
    if (!valid()) return false;
    // The system owns these two combinations; stealing them would break
    // closing the window / switching tasks while any control has focus.
    if (alt && (vk == VK_F4 || vk == VK_TAB || vk == VK_SPACE)) return false;
    if (vk == VK_ESCAPE || vk == VK_TAB || vk == VK_RETURN) return false;
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    return ctrl || alt;
}

std::wstring Hotkey::text() const {
    std::wstring out;
    auto add = [&out](const wchar_t* part) {
        if (!out.empty()) out += L" + ";
        out += part;
    };
    if (ctrl) add(L"Ctrl");
    if (alt) add(L"Alt");
    if (shift) add(L"Shift");
    std::wstring key = keyName(vk);
    if (!key.empty()) add(key.c_str());
    return out;
}

std::wstring Hotkey::storage() const {
    std::wstring out;
    if (ctrl) out += L"Ctrl+";
    if (alt) out += L"Alt+";
    if (shift) out += L"Shift+";
    std::wstring key = keyName(vk);
    if (!key.empty()) out += key;
    else out.clear();
    return out;
}

Hotkey Hotkey::parse(const std::wstring& raw) {
    Hotkey h;
    std::wstring part;
    auto take = [&h](const std::wstring& p) {
        std::wstring t = util::trim(p);
        if (t.empty()) return;
        if (util::iequals(t, L"Ctrl") || util::iequals(t, L"Control")) h.ctrl = true;
        else if (util::iequals(t, L"Alt")) h.alt = true;
        else if (util::iequals(t, L"Shift")) h.shift = true;
        else {
            int vk = keyFromName(t);
            if (vk) h.vk = vk;
        }
    };
    for (wchar_t c : raw) {
        if (c == L'+') { take(part); part.clear(); }
        else part += c;
    }
    take(part);
    return h;
}

// ---------------------------------------------------------------------------
// Widget
// ---------------------------------------------------------------------------
Widget::~Widget() = default;

void Widget::create(HWND parent, int id, const RECT& rc, bool popup) {
    parent_ = parent;
    id_ = id;
    rc_ = rc;
    popup_ = popup;
    DWORD ex = popup ? (WS_EX_TOOLWINDOW | WS_EX_TOPMOST) : 0;
    // WS_CLIPSIBLINGS is essential for overlay widgets (toast, popup menu):
    // without it, a repainting sibling covers a higher-Z child window even
    // though the overlay was created later / raised with HWND_TOP.
    DWORD style = popup ? (WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS)
                        : (WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS);
    hwnd_ = CreateWindowExW(ex, widgetClassName(), L"", style,
                            rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                            parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                            GetModuleHandleW(nullptr), this);
    if (hwnd_) {
        afterCreate();
        if (!visible_) ShowWindow(hwnd_, SW_HIDE);
        if (!enabled_) EnableWindow(hwnd_, FALSE);
    }
}

void Widget::setRect(const RECT& r) {
    rc_ = r;
    if (hwnd_) {
        SetWindowPos(hwnd_, nullptr, r.left, r.top,
                     r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Widget::setVisible(bool v) {
    visible_ = v;
    if (hwnd_) ShowWindow(hwnd_, v ? SW_SHOW : SW_HIDE);
}

void Widget::setEnabled(bool v) {
    enabled_ = v;
    if (hwnd_) EnableWindow(hwnd_, v ? TRUE : FALSE);
}

void Widget::invalidate() {
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void Widget::takeFocus() {
    if (hwnd_) SetFocus(hwnd_);
}

LRESULT CALLBACK Widget::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Widget* self = reinterpret_cast<Widget*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<Widget*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        return TRUE;
    }
    if (!self) return DefWindowProcW(h, msg, wp, lp);

    bool handled = false;
    LRESULT custom = self->message(msg, wp, lp, handled);
    if (handled) return custom;

    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(h, &ps);
        RECT rc{};
        GetClientRect(h, &rc);
        Gfx g(dc, rc);
        g.fill(rc, self->backdrop == Backdrop::Nav ? theme().navBg : theme().card);
        self->paint(g);
        g.flush();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        return 0;
    case WM_MOUSEMOVE: {
        self->mouse_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!self->hover_ && self->enabled_) {
            self->hover_ = true;
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            self->invalidate();
        }
        self->onMouseMove(self->mouse_.x, self->mouse_.y);
        return 0;
    }
    case WM_MOUSELEAVE:
        self->hover_ = false;
        self->pressed_ = false;
        self->invalidate();
        return 0;
    case WM_LBUTTONDOWN:
        if (!self->enabled_) return 0;
        self->mouse_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        self->pressed_ = true;
        self->takeFocus();
        self->invalidate();
        return 0;
    case WM_LBUTTONUP: {
        if (!self->enabled_) return 0;
        self->mouse_ = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        bool wasPressed = self->pressed_;
        self->pressed_ = false;
        self->invalidate();
        RECT rc{};
        GetClientRect(h, &rc);
        if (wasPressed && self->hover_ && PtInRect(&rc, self->mouse_))
            self->onClick();
        return 0;
    }
    case WM_LBUTTONDBLCLK:
        if (self->enabled_) self->onDoubleClick();
        return 0;
    case WM_SETFOCUS:
        self->focus_ = true;
        self->onFocus(true);
        self->invalidate();
        return 0;
    case WM_KILLFOCUS:
        self->focus_ = false;
        self->pressed_ = false;
        self->onFocus(false);
        self->invalidate();
        return 0;
    case WM_KEYDOWN:
        if (static_cast<int>(wp) == VK_TAB) {
            PostMessageW(GetAncestor(h, GA_ROOT), WM_APP_FOCUS_NEXT,
                         reinterpret_cast<WPARAM>(h),
                         (GetKeyState(VK_SHIFT) < 0) ? 1 : 0);
            return 0;
        }
        self->onKey(static_cast<int>(wp), true);
        return 0;
    case WM_KEYUP:
        self->onKey(static_cast<int>(wp), false);
        return 0;
    case WM_MOUSEWHEEL:
        self->onWheel(GET_WHEEL_DELTA_WPARAM(wp));
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

// ---------------------------------------------------------------------------
// Button
// ---------------------------------------------------------------------------
void Button::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    COLORREF fill = t.control, stroke = t.border, fg = t.text;
    if (caption) {
        bool hot = hovered();
        g.fill(r, hot ? (captionClose ? RGB(196, 43, 28) : t.controlHover) : t.navBg);
        fg = (captionClose && hot) ? RGB(255, 255, 255) : t.text;
        int is = S(glyphSize);
        RECT ir{(w - is) / 2, (h - is) / 2, (w + is) / 2, (h + is) / 2};
        g.icon(icon, ir, fg, 2);
        return;
    }
    if (accent) {
        fill = pressed() ? t.accentPressed : hovered() ? t.accentHover : t.accent;
        stroke = kNone;
        fg = t.accentText;
    } else if (danger) {
        fill = pressed() ? mix(t.danger, t.text, 18)
                         : hovered() ? mix(t.card, t.danger, 8) : t.card;
        stroke = mix(t.card, t.danger, 55);
        fg = t.danger;
    } else if (subtle) {
        fill = pressed() ? t.controlPressed : hovered() ? t.controlHover : kNone;
        stroke = kNone;
        fg = t.text;
    } else {
        fill = pressed() ? t.controlPressed : hovered() ? t.controlHover : t.control;
        stroke = hovered() ? t.borderStrong : t.border;
    }
    if (!enabled()) {
        fill = subtle ? kNone : t.cardAlt;
        stroke = t.border;
        fg = t.disabled;
    }
    int radius = S(6);
    g.round(r, radius, fill, stroke, 1);
    // The ring has to live inside the client area: a widget paints into its
    // own bitmap, so an outward ring would only leave four clipped corners.
    if (focused()) {
        COLORREF ring = accent ? t.accentText : t.accent;
        g.round(inset(r, S(2), S(2)), std::max(S(2), radius - S(2)), kNone,
                ring, S(2));
    }

    int iconW = icon == Ic::None ? 0 : S(glyphSize);
    int gap = iconW ? S(8) : 0;
    SIZE sz = g.measure(text, t.fStrong);
    int total = iconW + gap + sz.cx;
    int start = (w - total) / 2;
    if (start < S(8)) start = S(8);
    if (iconW) {
        RECT ir{start, (h - iconW) / 2, start + iconW, (h + iconW) / 2};
        g.icon(icon, ir, fg, 2);
    }
    RECT tr{start + iconW + gap, 0, w - S(8), h};
    g.text(tr, text, fg, t.fStrong, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void Button::onKey(int vk, bool down) {
    if (down && (vk == VK_RETURN || vk == VK_SPACE)) onClick();
}

// ---------------------------------------------------------------------------
// Link
// ---------------------------------------------------------------------------
void Link::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    COLORREF c = !enabled() ? t.muted
               : pressed() ? t.accentPressed
               : (hovered() || focused()) ? t.accentHover : t.accent;
    RECT tr{0, 0, w, h};
    HFONT font = mono ? t.fMono : t.fBody;
    g.text(tr, text, c, font,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    // DrawTextW has no underline flag, so draw the rule under the measured
    // text ourselves (clamped when the URL is ellipsized).
    SIZE sz = g.measure(text, font);
    int uw = std::min(static_cast<int>(sz.cx), w);
    int uy = h / 2 + sz.cy / 2 - S(2);
    if (uw > 0) g.line(0, uy, uw, uy, c, 1);
    if (focused()) {
        int top = h / 2 - sz.cy / 2 - S(1);
        g.round(RECT{-S(3), top, uw + S(3), h / 2 + sz.cy / 2}, S(3),
                kNone, t.accent, 1);
    }
}

void Link::onClick() {
    Widget::onClick();
    if (url.empty()) return;
    HWND root = GetAncestor(hwnd_, GA_ROOT);
    ShellExecuteW(root ? root : hwnd_, L"open", url.c_str(), nullptr,
                  nullptr, SW_SHOWNORMAL);
}

void Link::onKey(int vk, bool down) {
    if (down && (vk == VK_RETURN || vk == VK_SPACE)) onClick();
}

LRESULT Link::message(UINT msg, WPARAM /*wp*/, LPARAM lp, bool& handled) {
    handled = false;
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        handled = true;
        return TRUE;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Check / Switch
// ---------------------------------------------------------------------------
void Check::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    int pad = S(2);
    if (style == Style::Box) {
        int box = S(18);
        RECT br{pad, (h - box) / 2, pad + box, (h + box) / 2};
        COLORREF face = checked ? t.accent : hovered() ? t.controlHover : t.control;
        COLORREF edge = checked ? t.accent : hovered() ? t.borderStrong : t.borderStrong;
        g.round(br, S(4), face, edge, 1);
        if (checked) {
            RECT ir = inset(br, S(2), S(2));
            g.icon(Ic::Check, ir, t.accentText, 2);
        }
        if (focused()) g.round(inset(br, -S(2), -S(2)), S(6), kNone, t.accent, S(2));
        RECT tr{box + S(10), 0, w, h};
        g.text(tr, text, enabled() ? t.text : t.disabled, t.fBody,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        int tw = S(40), th = S(20);
        RECT tr{pad, (h - th) / 2, pad + tw, (h + th) / 2};
        COLORREF face = checked ? t.accent : t.control;
        COLORREF edge = checked ? t.accent : t.borderStrong;
        g.round(tr, th / 2, face, edge, 1);
        int knob = th - S(8);
        int cx = checked ? tr.right - th / 2 : tr.left + th / 2;
        g.circle(cx, (tr.top + tr.bottom) / 2, knob / 2,
                 checked ? t.accentText : t.subText);
        if (focused()) g.round(inset(tr, -S(2), -S(2)), th / 2 + S(2), kNone, t.accent, S(2));
        RECT lr{tw + S(12), 0, w, h};
        g.text(lr, text, enabled() ? t.text : t.disabled, t.fBody,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
}

void Check::onClick() {
    checked = !checked;
    invalidate();
    if (changed) changed(checked);
    Widget::onClick();
}

void Check::onKey(int vk, bool down) {
    if (down && vk == VK_SPACE) onClick();
}

// ---------------------------------------------------------------------------
// HotkeyField (click to record)
// ---------------------------------------------------------------------------
void HotkeyField::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    COLORREF face = enabled() ? t.control : t.cardAlt;
    COLORREF edge = capturing_ ? t.accent
                  : hovered() ? t.borderStrong : t.border;
    g.round(r, S(6), face, edge, capturing_ ? S(2) : 1);

    std::wstring shown;
    COLORREF fg = t.text;
    if (capturing_) {
        shown = hint_.empty() ? L"请按下新的组合键…" : hint_;
        fg = hint_.empty() ? t.accent : t.danger;
    } else if (value.empty()) {
        shown = placeholder;
        fg = t.muted;
    } else {
        shown = Hotkey::parse(value).text();
        if (!enabled()) fg = t.disabled;
    }
    g.text(inset(r, S(10), 0), shown, fg, t.fMono,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    // Same focus cue as TextBox: a 2px accent underline, never a full ring.
    if (focused() && !capturing_)
        g.fill(RECT{r.left + S(8), r.bottom - S(3), r.right - S(8), r.bottom - S(1)},
               t.accent);
}

void HotkeyField::onClick() {
    if (!enabled_) return;
    capturing_ = true;
    hint_.clear();
    invalidate();
}

void HotkeyField::onFocus(bool focused) {
    if (!focused && capturing_) {
        capturing_ = false;
        hint_.clear();
        invalidate();
    }
}

void HotkeyField::onKey(int vk, bool down) {
    if (!down || !enabled()) return;
    if (!capturing_) {
        if (vk == VK_RETURN || vk == VK_SPACE) onClick();
        return;
    }
    if (vk == VK_ESCAPE) {
        capturing_ = false;
        hint_.clear();
        invalidate();
        return;
    }
    if (vk == VK_BACK || vk == VK_DELETE) {
        value.clear();
        capturing_ = false;
        hint_.clear();
        invalidate();
        if (changed) changed(value);
        return;
    }
    // A modifier alone is not a combo: wait for the actual key.
    if (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU ||
        vk == VK_LWIN || vk == VK_RWIN)
        return;

    Hotkey hk;
    hk.ctrl = GetKeyState(VK_CONTROL) < 0;
    hk.alt = GetKeyState(VK_MENU) < 0;
    hk.shift = GetKeyState(VK_SHIFT) < 0;
    hk.vk = vk;
    std::wstring combo = hk.storage();
    if (combo.empty() || !hk.usable()) {
        hint_ = L"需要 Ctrl / Alt 或功能键";
        invalidate();
        return;
    }
    if (accept && !accept(combo)) {
        hint_ = L"该组合键已被占用";
        invalidate();
        return;
    }
    value = combo;
    capturing_ = false;
    hint_.clear();
    invalidate();
    if (changed) changed(value);
}

LRESULT HotkeyField::message(UINT msg, WPARAM wp, LPARAM /*lp*/, bool& handled) {
    // Alt combinations arrive as WM_SYSKEYDOWN, not WM_KEYDOWN. Only steal
    // them while recording, so Alt+F4 / Alt+Tab keep working everywhere else.
    if (capturing_ && (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP)) {
        onKey(static_cast<int>(wp), msg == WM_SYSKEYDOWN);
        handled = true;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// TextBox (owner-drawn frame around the built-in EDIT control)
// ---------------------------------------------------------------------------
void TextBox::afterCreate() {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (multiline) style |= ES_MULTILINE | ES_WANTRETURN | ES_AUTOVSCROLL;
    if (readOnly)  style |= ES_READONLY;
    if (password)  style |= ES_PASSWORD;
    edit_ = CreateWindowExW(0, L"Edit", L"", style, 0, 0, 1, 1, hwnd_,
                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1)),
                            GetModuleHandleW(nullptr), nullptr);
    if (!edit_) return;
    SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    oldEdit_ = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(edit_, GWLP_WNDPROC,
                          reinterpret_cast<LONG_PTR>(TextBox::editProc)));
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme().fBody), TRUE);
    if (!placeholder.empty()) {
        // EM_SETCUEBANNER (0x1501) - the OS draws the prompt text, which keeps
        // IME and caret behaviour intact without any extra painting. FALSE =
        // only show it while the field is unfocused (standard Fluent cue).
        SendMessageW(edit_, 0x1501, FALSE, reinterpret_cast<LPARAM>(placeholder.c_str()));
    }
    layoutEdit();
}

void TextBox::invalidate() {
    Widget::invalidate();
    // The native EDIT is painted by Windows, not by Gfx: keep it in sync so a
    // resize/theme/repaint can never leave stale pixels inside the field.
    if (edit_) InvalidateRect(edit_, nullptr, FALSE);
}

void TextBox::layoutEdit() {
    if (!edit_) return;
    int m = S(9);
    int w = width() - m * 2;
    int h = height() - (multiline ? S(14) : S(12));
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    SetWindowPos(edit_, nullptr, m, (height() - h) / 2, w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(edit_, nullptr, TRUE);
}

void TextBox::setRect(const RECT& r) {
    Widget::setRect(r);
    layoutEdit();
}

void TextBox::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    COLORREF face = readOnly ? t.cardAlt : t.control;
    // Fluent text fields keep the same subtle outline in every state; focus is
    // expressed by a 2px accent underline only. Recolouring the whole border
    // would leave stale coloured segments behind on partial repaints.
    COLORREF edge = hovered() ? t.borderStrong : t.border;
    g.round(r, S(6), face, edge, 1);
    if (dropGlow_) {
        // A file drag is currently over the window: advertise this field as a
        // drop target. Only the ring the native EDIT does not cover can show
        // paint here, so the cue is a thicker outline plus two accent ticks
        // in the left / right margin (never a fill of the text area, which
        // would sit under a child window that is not repainted with us).
        COLORREF hot = dropGlow_ == 2 ? t.accent : mix(t.accent, t.card, 50);
        g.round(r, S(6), kNone, hot, dropGlow_ == 2 ? S(2) : 1);
        if (dropGlow_ == 2 && h > S(16) && w > S(28)) {
            g.fill(RECT{r.left + S(5), r.top + S(6), r.left + S(8),
                        r.bottom - S(6)}, t.accent);
            g.fill(RECT{r.right - S(8), r.top + S(6), r.right - S(5),
                        r.bottom - S(6)}, t.accent);
        }
    }
    if (focused()) {
        g.fill(RECT{r.left + S(8), r.bottom - S(3), r.right - S(8), r.bottom - S(1)},
               t.accent);
    }
    if (!enabled()) g.fill(inset(r, 1, 1), mix(face, t.cardAlt, 60));
    // Paint the whole field (including the EDIT rectangle). Windows paints
    // child windows after their parent, so the EDIT text/caret still ends up
    // on top, while a stale partial repaint can never survive here.
}

LRESULT TextBox::message(UINT msg, WPARAM wp, LPARAM /*lp*/, bool& handled) {
    if (msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORSTATIC) {
        HDC dc = reinterpret_cast<HDC>(wp);
        SetTextColor(dc, enabled() ? theme().text : theme().disabled);
        SetBkColor(dc, readOnly ? theme().cardAlt : theme().control);
        handled = true;
        return reinterpret_cast<LRESULT>(readOnly ? theme().brCard : theme().brControl);
    }
    if (msg == WM_COMMAND && HIWORD(wp) == EN_CHANGE) {
        if (changed) changed();
        handled = true;
        return 0;
    }
    return 0;
}

LRESULT CALLBACK TextBox::editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<TextBox*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self) {
        switch (msg) {
        case WM_SETFOCUS:
            self->focus_ = true;
            self->invalidate();
            break;
        case WM_KILLFOCUS:
            self->focus_ = false;
            self->invalidate();
            break;
        case WM_KEYDOWN:
            if (wp == VK_RETURN && !self->multiline) {
                if (self->enter) self->enter();
                return 0;
            }
            if (wp == VK_ESCAPE) {
                PostMessageW(GetAncestor(h, GA_ROOT), WM_APP_ESC, 0, 0);
                return 0;
            }
            if (wp == VK_TAB) {
                PostMessageW(GetAncestor(h, GA_ROOT), WM_APP_FOCUS_NEXT,
                             reinterpret_cast<WPARAM>(h),
                             (GetKeyState(VK_SHIFT) < 0) ? 1 : 0);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wp == VK_RETURN && !self->multiline) return 0;  // no beep
            break;
        case WM_PAINT: {
            // EM_SETCUEBANNER needs a comctl32 v6 activation context, which a
            // pure-Win32 build does not necessarily have. Draw the cue text
            // ourselves on top of the native EDIT painting instead.
            LRESULT r = self->oldEdit_
                ? CallWindowProcW(self->oldEdit_, h, msg, wp, lp)
                : DefWindowProcW(h, msg, wp, lp);
            if (!self->focus_ && !self->placeholder.empty() &&
                GetWindowTextLengthW(h) == 0) {
                HDC dc = GetDC(h);
                if (dc) {
                    RECT rc{};
                    GetClientRect(h, &rc);
                    RECT tr = rc;
                    tr.left += S(2);
                    if (self->multiline) tr.top += S(2);
                    SetBkMode(dc, TRANSPARENT);
                    SetTextColor(dc, theme().muted);
                    HGDIOBJ oldFont = SelectObject(dc, theme().fBody);
                    DrawTextW(dc, self->placeholder.c_str(), -1, &tr,
                              DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS |
                              (self->multiline ? DT_TOP : DT_VCENTER));
                    SelectObject(dc, oldFont);
                    ReleaseDC(h, dc);
                }
            }
            return r;
        }
        default:
            break;
        }
    }
    return self && self->oldEdit_
        ? CallWindowProcW(self->oldEdit_, h, msg, wp, lp)
        : DefWindowProcW(h, msg, wp, lp);
}

std::wstring TextBox::value() const {
    if (!edit_) return L"";
    int n = GetWindowTextLengthW(edit_);
    if (n <= 0) return L"";
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit_, s.data(), n + 1);
    s.resize(wcslen(s.c_str()));
    return s;
}

void TextBox::setValue(const std::wstring& s) {
    if (edit_) SetWindowTextW(edit_, s.c_str());
}

void TextBox::setDropGlow(int level) {
    if (dropGlow_ == level) return;
    dropGlow_ = level;
    invalidate();
}

void TextBox::selectAll() {
    if (edit_) SendMessageW(edit_, EM_SETSEL, 0, -1);
}

void TextBox::focusEdit() {
    if (edit_) SetFocus(edit_);
}

void TextBox::takeFocus() {
    if (edit_) SetFocus(edit_);
    else Widget::takeFocus();
}

void TextBox::onThemeChanged() {
    if (edit_) {
        SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(theme().fBody), TRUE);
        if (!placeholder.empty())
            SendMessageW(edit_, 0x1501, FALSE, reinterpret_cast<LPARAM>(placeholder.c_str()));
        RedrawWindow(edit_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW);
    }
    invalidate();
}

// ---------------------------------------------------------------------------
// Popup menu (used by DropDown)
// ---------------------------------------------------------------------------
void Popup::show(HWND owner, int x, int y, int w) {
    int rowH = S(34);
    int h = static_cast<int>(items.size()) * rowH + S(10);
    int maxH = S(360);
    if (h > maxH) h = maxH;
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    if (x + w > sw - S(8)) x = sw - w - S(8);
    if (y + h > sh - S(8)) y = sh - h - S(8);
    if (x < S(8)) x = S(8);
    if (y < S(8)) y = S(8);
    create(owner, 0, RECT{x, y, x + w, y + h}, true);
    if (hwnd_) {
        SetFocus(hwnd_);
        SetCapture(hwnd_);
    }
}

void Popup::close() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    ReleaseCapture();
    DestroyWindow(h);
}

void Popup::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    g.round(r, S(8), t.card, t.border, 1);
    int rowH = S(34);
    int top = S(5);
    SaveDC(g.dc());
    IntersectClipRect(g.dc(), S(4), top, w - S(4), h - S(4));
    for (size_t i = 0; i < items.size(); ++i) {
        int y = top + static_cast<int>(i) * rowH;
        if (y + rowH < 0 || y > h) continue;
        RECT row{S(4), y, w - S(4), y + rowH};
        if (static_cast<int>(i) == hover) g.round(row, S(5), t.gridHover);
        else if (static_cast<int>(i) == index) g.round(row, S(5), t.accentSoft);
        RECT tr{row.left + S(12), y, row.right - S(34), y + rowH};
        g.text(tr, items[i],
               static_cast<int>(i) == index ? t.accent : t.text,
               static_cast<int>(i) == index ? t.fStrong : t.fBody,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (static_cast<int>(i) == index) {
            RECT ir{row.right - S(28), y + S(8), row.right - S(10), y + S(26)};
            g.icon(Ic::Check, ir, t.accent, 2);
        }
    }
    RestoreDC(g.dc(), -1);
}

void Popup::onMouseMove(int x, int y) {
    int rowH = S(34);
    int idx = (y - S(5)) / rowH;
    if (idx < 0 || idx >= static_cast<int>(items.size())) idx = -1;
    if (idx != hover) { hover = idx; invalidate(); }
    (void)x;
}

void Popup::onKey(int vk, bool down) {
    if (!down || items.empty()) return;
    if (vk == VK_ESCAPE) { close(); return; }
    if (vk == VK_DOWN || vk == VK_UP) {
        int i = hover < 0 ? index : hover;
        i += (vk == VK_DOWN ? 1 : -1);
        if (i < 0) i = static_cast<int>(items.size()) - 1;
        if (i >= static_cast<int>(items.size())) i = 0;
        hover = i;
        invalidate();
        return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        int i = hover >= 0 ? hover : index;
        if (picked) picked(i);
        close();
    }
}

LRESULT Popup::message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (!PtInRect(&rc, POINT{x, y})) {
            close();
        } else {
            int idx = (y - S(5)) / S(34);
            if (idx >= 0 && idx < static_cast<int>(items.size())) {
                if (picked) picked(idx);
            }
            close();
        }
        handled = true;
        return 0;
    }
    if (msg == WM_KILLFOCUS || msg == WM_CAPTURECHANGED) {
        close();
        handled = true;
        return 0;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) {
        close();
        handled = true;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// DropDown
// ---------------------------------------------------------------------------
void DropDown::setItems(std::vector<std::wstring> values) {
    items = std::move(values);
    if (index >= static_cast<int>(items.size())) index = 0;
    invalidate();
}

void DropDown::setIndex(int i) {
    if (i < 0 || i >= static_cast<int>(items.size())) return;
    index = i;
    invalidate();
}

std::wstring DropDown::value() const {
    return (index >= 0 && index < static_cast<int>(items.size())) ? items[index] : L"";
}

void DropDown::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    COLORREF face = pressed() ? t.controlPressed : hovered() ? t.controlHover : t.control;
    g.round(r, S(6), face, hovered() ? t.borderStrong : t.border, 1);
    if (focused()) g.fill(RECT{r.left + S(8), r.bottom - S(3), r.right - S(8), r.bottom - S(1)}, t.accent);
    RECT tr{r.left + S(12), 0, r.right - S(34), h};
    g.text(tr, value(), enabled() ? t.text : t.disabled, t.fBody,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    RECT ir{r.right - S(30), (h - S(18)) / 2, r.right - S(10), (h + S(18)) / 2};
    g.icon(Ic::ChevronDown, ir, t.subText, 2);
}

void DropDown::onClick() {
    openPopup();
    Widget::onClick();
}

void DropDown::onKey(int vk, bool down) {
    if (!down) return;
    if (vk == VK_RETURN || vk == VK_SPACE || vk == VK_DOWN || vk == VK_F4)
        openPopup();
    else if (vk == VK_UP && !items.empty()) {
        setIndex((index - 1 + static_cast<int>(items.size())) % static_cast<int>(items.size()));
        if (selected) selected(index);
    }
}

void DropDown::openPopup() {
    if (items.empty()) return;
    if (popup_) popup_->close();
    POINT p{0, height()};
    ClientToScreen(hwnd_, &p);
    popup_ = std::make_unique<Popup>();
    popup_->items = items;
    popup_->index = index;
    popup_->hover = index;
    popup_->picked = [this](int i) {
        setIndex(i);
        if (selected) selected(i);
    };
    popup_->show(hwnd_, p.x, p.y, std::max(width(), S(180)));
}

// ---------------------------------------------------------------------------
// DataGrid
// ---------------------------------------------------------------------------
int DataGrid::headHeight() const {
    return headerVisible ? (headHeightPx ? headHeightPx : S(36)) : 0;
}
int DataGrid::rowHeight() const {
    return rowHeightPx ? rowHeightPx : S(32);
}

int DataGrid::totalWidth() const {
    int w = 0;
    for (int c : widths_) w += c;
    return w;
}

int DataGrid::maxScrollY() const {
    int content = height() - headHeight();
    int total = static_cast<int>(rows_.size()) * rowHeight();
    return std::max(0, total - content);
}

int DataGrid::maxScrollX() const {
    return std::max(0, totalWidth() - width() + S(10));
}

void DataGrid::fitWidths() {
    size_t n = headers_.size();
    for (const auto& r : rows_) n = std::max(n, r.size());
    if (autoWidths_ && n) {
        HDC dc = GetDC(nullptr);
        widths_.assign(n, S(120));
        for (size_t c = 0; c < n; ++c) {
            int w = c < headers_.size()
                ? textWidth(dc, theme().fStrong, headers_[c]) + S(30) : S(120);
            size_t limit = std::min<size_t>(rows_.size(), 120);
            for (size_t r = 0; r < limit; ++r)
                if (c < rows_[r].size())
                    w = std::max(w, textWidth(dc, theme().fBody, rows_[r][c]) + S(30));
            widths_[c] = std::min(S(420), std::max(S(90), w));
        }
        ReleaseDC(nullptr, dc);
    }
    if (widths_.size() < n) widths_.resize(n, S(140));
    // With only a couple of columns, fill the available width instead of
    // leaving a large blank area to the right of the last column.
    if (width() > S(100) && n >= 1 && n <= 3) {
        int total = totalWidth();
        if (total > 0 && total < width() - S(16)) {
            int extra = (width() - S(16) - total) / static_cast<int>(n);
            for (int& c : widths_) c += extra;
        }
    }
    builtDpi_ = theme().dpi;
}

// The grid stores its geometry in physical pixels (column widths, row height),
// which is what the hit-testing and painting code needs. Those numbers were
// however fitted with the *fonts and paddings of the DPI that was active when
// the data arrived*: after WM_DPICHANGED (window dragged to a monitor with a
// different scale) they would be far too narrow / too short and every cell
// would end in an ellipsis. So re-fit as soon as the DPI is seen to change:
// auto-fitted columns are measured again with the new fonts, while explicit
// widths (S(230) style constants chosen by the caller) are scaled linearly.
void DataGrid::syncDpi() {
    int d = theme().dpi;
    if (builtDpi_ == 0) { builtDpi_ = d; return; }
    if (d == builtDpi_) return;
    if (autoWidths_) {
        fitWidths();
    } else {
        for (int& w : widths_) w = MulDiv(w, d, builtDpi_);
        builtDpi_ = d;
    }
}

void DataGrid::setData(std::vector<std::wstring> headers,
                       std::vector<std::vector<std::wstring>> rows,
                       std::vector<int> widths,
                       std::vector<Ic> rowIcons) {
    headers_ = std::move(headers);
    rows_ = std::move(rows);
    widths_ = std::move(widths);
    autoWidths_ = widths_.empty();
    rowIcons_ = std::move(rowIcons);
    if (!rowIcons_.empty() && rowIcons_.size() < rows_.size())
        rowIcons_.resize(rows_.size(), Ic::None);
    fitWidths();
    scrollY_ = scrollX_ = 0;
    hoverRow_ = selected_ = -1;
    invalidate();
}

void DataGrid::clear() { setData({}, {}, {}); }

void DataGrid::setColumnWidths(std::vector<int> widths) {
    if (widths.empty()) return;
    widths_ = std::move(widths);
    autoWidths_ = false;
    builtDpi_ = theme().dpi;
    invalidate();
}

void DataGrid::setRect(const RECT& r) {
    Widget::setRect(r);
    // After the new size is in place: a re-fit at a new DPI distributes the
    // spare width over the columns and needs the *new* client width.
    syncDpi();
    // A resize (or a first layout pass with a tiny client area) can leave the
    // old scroll offset pointing past the end; rows would then be skipped and
    // hit-testing would select the wrong row.
    int my = maxScrollY();
    int mx = maxScrollX();
    if (scrollY_ > my) scrollY_ = my;
    if (scrollY_ < 0) scrollY_ = 0;
    if (scrollX_ > mx) scrollX_ = mx;
    if (scrollX_ < 0) scrollX_ = 0;
    invalidate();
}

std::wstring DataGrid::cell(int row, int col) const {
    if (row < 0 || row >= static_cast<int>(rows_.size())) return L"";
    if (col < 0 || col >= static_cast<int>(rows_[row].size())) return L"";
    return rows_[row][col];
}

std::wstring DataGrid::selectedRowText() const {
    if (selected_ < 0 || selected_ >= static_cast<int>(rows_.size())) return L"";
    std::wstring s;
    for (size_t c = 0; c < rows_[selected_].size(); ++c) {
        if (c) s += L"\t";
        s += rows_[selected_][c];
    }
    return s;
}

void DataGrid::copyAll() {
    std::wstring s;
    for (size_t c = 0; c < headers_.size(); ++c) {
        if (c) s += L"\t";
        s += headers_[c];
    }
    if (!headers_.empty()) s += L"\r\n";
    for (const auto& row : rows_) {
        for (size_t c = 0; c < row.size(); ++c) {
            if (c) s += L"\t";
            s += row[c];
        }
        s += L"\r\n";
    }
    setClipboardText(hwnd_, s);
}

void DataGrid::ensureVisible(int row) {
    int hh = headHeight(), rh = rowHeight();
    int content = std::max(0, height() - hh);
    int top = row * rh;
    if (top < scrollY_) scrollY_ = top;
    if (top + rh > scrollY_ + content) scrollY_ = top + rh - content;
    scrollY_ = std::min(scrollY_, maxScrollY());
    invalidate();
}

void DataGrid::select(int row) {
    if (rows_.empty()) return;
    row = std::max(0, std::min(row, static_cast<int>(rows_.size()) - 1));
    selected_ = row;
    ensureVisible(row);
    invalidate();
    if (selectionChanged) selectionChanged(row);
}

void DataGrid::scrollBy(int dy) {
    scrollY_ = std::max(0, std::min(maxScrollY(), scrollY_ + dy));
    invalidate();
}

void DataGrid::hitTest(int x, int y, int& row) {
    (void)x;
    int hh = headHeight(), rh = rowHeight();
    if (y < hh) { row = -1; return; }
    row = (y - hh + scrollY_) / rh;
    if (row < 0 || row >= static_cast<int>(rows_.size())) row = -1;
}

void DataGrid::paint(Gfx& g) {
    syncDpi();
    Theme& t = theme();
    int w = width(), h = height();
    if (w < 4 || h < 4) return;
    int hh = headHeight(), rh = rowHeight();

    SaveDC(g.dc());
    if (rounded) {
        HRGN rgn = CreateRoundRectRgn(0, 0, w, h, S(8), S(8));
        SelectClipRgn(g.dc(), rgn);
        DeleteObject(rgn);
    }
    g.fill(RECT{0, 0, w, h}, t.card);

    if (headerVisible) {
        g.fill(RECT{0, 0, w, hh}, t.gridHead);
        int x = -scrollX_;
        for (size_t c = 0; c < headers_.size(); ++c) {
            int cw = c < widths_.size() ? widths_[c] : S(140);
            RECT cr{x + S(12), 0, x + cw - S(8), hh};
            if (cr.right > 0 && cr.left < w)
                g.text(cr, headers_[c], t.subText, t.fStrong,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            x += cw;
            if (c + 1 < headers_.size() && x > 0 && x < w)
                g.line(x, S(8), x, hh - S(8), t.gridLine);
        }
        g.line(0, hh - 1, w, hh - 1, t.gridLine);
    }

    SaveDC(g.dc());
    IntersectClipRect(g.dc(), 0, hh, w, h);
    int first = scrollY_ / rh;
    int count = (h - hh) / rh + 2;
    for (int i = first; i < std::min<int>(static_cast<int>(rows_.size()), first + count); ++i) {
        int y = hh + i * rh - scrollY_;
        if (y + rh < hh || y > h) continue;
        COLORREF bg = t.card;
        if (i == selected_) bg = t.selection;
        else if (i == hoverRow_) bg = t.gridHover;
        else if (zebra && (i % 2)) bg = t.gridAlt;
        g.fill(RECT{0, y, w, y + rh}, bg);
        if (i == selected_) g.fill(RECT{0, y, S(3), y + rh}, t.accent);
        int x = -scrollX_;
        for (size_t c = 0; c < widths_.size(); ++c) {
            int cw = widths_[c];
            if (x + cw > 0 && x < w) {
                RECT cr{x + S(12), y, x + cw - S(8), y + rh};
                if (c == 0 && i < static_cast<int>(rowIcons_.size()) &&
                    rowIcons_[i] != Ic::None) {
                    int is = std::min(S(18), rh - S(6));
                    RECT ir{x + S(10), y + (rh - is) / 2,
                            x + S(10) + is, y + (rh + is) / 2};
                    COLORREF ic = (rowIcons_[i] == Ic::Folder) ? t.warning : t.subText;
                    g.icon(rowIcons_[i], ir, ic, 2);
                    cr.left = x + S(10) + is + S(6);
                }
                g.text(cr, cell(i, static_cast<int>(c)), t.text, t.fBody,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            }
            x += cw;
            if (c + 1 < widths_.size() && x > 0 && x < w)
                g.line(x, y + S(6), x, y + rh - S(6), t.gridLine);
        }
        g.line(0, y + rh - 1, w, y + rh - 1, t.gridLine);
    }
    RestoreDC(g.dc(), -1);

    if (rows_.empty()) {
        RECT ir{(w - S(40)) / 2, hh + (h - hh) / 2 - S(44),
                (w + S(40)) / 2, hh + (h - hh) / 2 - S(4)};
        g.icon(Ic::Database, ir, t.muted, 2);
        RECT tr{0, hh + (h - hh) / 2 + S(4), w, hh + (h - hh) / 2 + S(30)};
        g.text(tr, L"暂无数据", t.muted, t.fBody,
               DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    // Fluent-style thin scrollbar (drag + wheel + keyboard).
    if (maxScrollY() > 0) {
        int trackH = h - hh;
        int thumbH = std::max(S(28), trackH * trackH /
                             (static_cast<int>(rows_.size()) * rh));
        int pos = maxScrollY() ? (trackH - thumbH) * scrollY_ / maxScrollY() : 0;
        g.fill(RECT{w - S(6), hh, w, h}, t.scrollTrack);
        RECT thumb{w - S(5), hh + pos + S(2), w - S(1), hh + pos + thumbH - S(2)};
        g.round(thumb, S(3), t.scrollThumb);
    }
    if (rounded) g.round(RECT{0, 0, w, h}, S(8), kNone, t.border, 1);
    else g.rect(RECT{0, 0, w - 1, h - 1}, t.border, 1);
    RestoreDC(g.dc(), -1);
}

void DataGrid::onMouseMove(int x, int y) {
    if (dragging_) {
        int trackH = std::max(1, height() - headHeight());
        int total = static_cast<int>(rows_.size()) * rowHeight();
        int delta = (mouse_.y - dragStartY_) * total / trackH;
        scrollY_ = std::max(0, std::min(maxScrollY(), dragStartScroll_ + delta));
        invalidate();
        return;
    }
    int row = -1;
    if (x < width() - S(10)) hitTest(x, y, row);
    if (row != hoverRow_) { hoverRow_ = row; invalidate(); }
}

void DataGrid::onWheel(int delta) {
    scrollBy(-delta / WHEEL_DELTA * rowHeight() * 3);
}

void DataGrid::onKey(int vk, bool down) {
    if (!down || rows_.empty()) {
        if (down && (GetKeyState(VK_CONTROL) < 0) && vk == L'C') copyAll();
        return;
    }
    if ((GetKeyState(VK_CONTROL) < 0) && vk == L'C') { copyAll(); return; }
    int cur = selected_ < 0 ? 0 : selected_;
    int page = std::max(1, (height() - headHeight()) / rowHeight());
    switch (vk) {
    case VK_DOWN:   cur = std::min<int>(static_cast<int>(rows_.size()) - 1, cur + 1); break;
    case VK_UP:     cur = std::max(0, cur - 1); break;
    case VK_NEXT:   cur = std::min<int>(static_cast<int>(rows_.size()) - 1, cur + page); break;
    case VK_PRIOR:  cur = std::max(0, cur - page); break;
    case VK_HOME:   cur = 0; break;
    case VK_END:    cur = static_cast<int>(rows_.size()) - 1; break;
    case VK_RETURN:
        if (rowActivated && selected_ >= 0) rowActivated(selected_);
        return;
    case VK_BACK:
        if (goUp) goUp();
        return;
    case VK_F5:
        if (refreshRequested) refreshRequested();
        return;
    default: return;
    }
    select(cur);
}

LRESULT DataGrid::message(UINT msg, WPARAM /*wp*/, LPARAM lp, bool& handled) {
    if (msg == WM_LBUTTONDOWN && enabled()) {
        SetFocus(hwnd_);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (headerVisible && y < headHeight() && headerClicked) {
            int cx = -scrollX_;
            for (size_t c = 0; c < widths_.size(); ++c) {
                cx += widths_[c];
                if (x < cx) { headerClicked(static_cast<int>(c)); handled = true; return 0; }
            }
        }
        if (iconClicked && y >= headHeight() && x < S(26)) {
            int row = -1;
            hitTest(x, y, row);
            if (row >= 0) iconClicked(row);
            handled = true;
            return 0;
        }
        if (maxScrollY() > 0 && x > width() - S(16)) {
            dragging_ = true;
            dragStartY_ = y;
            dragStartScroll_ = scrollY_;
            SetCapture(hwnd_);
        } else {
            int row = -1;
            hitTest(x, y, row);
            if (row >= 0) select(row);
        }
        handled = true;
        return 0;
    }
    if (msg == WM_LBUTTONUP && dragging_) {
        dragging_ = false;
        ReleaseCapture();
        handled = true;
        return 0;
    }
    if (msg == WM_LBUTTONDBLCLK && enabled()) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int row = -1;
        hitTest(x, y, row);
        if (row >= 0) {
            select(row);
            if (rowActivated) rowActivated(row);
        }
        handled = true;
        return 0;
    }
    if (msg == WM_RBUTTONUP && enabled()) {
        SetFocus(hwnd_);
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        int row = -1;
        hitTest(x, y, row);
        if (row >= 0) select(row);
        if (row >= 0 && itemContextMenu) {
            POINT p{x, y};
            ClientToScreen(hwnd_, &p);
            itemContextMenu(row, p);
        }
        handled = true;
        return 0;
    }
    if (msg == WM_CONTEXTMENU && itemContextMenu) {
        int row = selected_;
        if (row >= 0) {
            POINT p{0, 0};
            if (lp != static_cast<LPARAM>(-1)) {
                p.x = GET_X_LPARAM(lp);   // already screen coords
                p.y = GET_Y_LPARAM(lp);
            } else {
                int y = headHeight() + row * rowHeight() - scrollY_ + rowHeight() / 2;
                p.x = width() / 2;
                p.y = std::max(headHeight(), y);
                ClientToScreen(hwnd_, &p);
            }
            itemContextMenu(row, p);
        }
        handled = true;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// LogView
// ---------------------------------------------------------------------------
int LogView::lineHeight() const { return S(22); }
int LogView::maxScroll() const {
    return std::max(0, static_cast<int>(lines_.size()) * lineHeight()
                       - (height() - S(12)));
}

void LogView::append(const std::wstring& line, Kind kind) {
    std::wstring s = line;
    while (!s.empty() && (s.back() == L'\r' || s.back() == L'\n')) s.pop_back();
    if (s.empty()) return;
    constexpr size_t kMaxLines = 4000;
    if (lines_.size() >= kMaxLines) lines_.erase(lines_.begin(), lines_.begin() + 500);
    lines_.emplace_back(kind, s);
    if (autoScroll_) scrollY_ = maxScroll();
    invalidate();
}

void LogView::clear() {
    lines_.clear();
    scrollY_ = 0;
    autoScroll_ = true;
    invalidate();
}

void LogView::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    if (w < 4 || h < 4) return;
    int lh = lineHeight();
    SaveDC(g.dc());
    HRGN rgn = CreateRoundRectRgn(0, 0, w, h, S(8), S(8));
    SelectClipRgn(g.dc(), rgn);
    DeleteObject(rgn);
    g.fill(RECT{0, 0, w, h}, t.cardAlt);
    SaveDC(g.dc());
    IntersectClipRect(g.dc(), 0, S(6), w, h - S(6));
    int first = scrollY_ / lh;
    int count = h / lh + 2;
    for (int i = first; i < std::min<int>(static_cast<int>(lines_.size()), first + count); ++i) {
        int y = S(6) + i * lh - scrollY_;
        if (y + lh < 0 || y > h) continue;
        COLORREF c = t.text;
        switch (lines_[i].first) {
        case Kind::Warn:  c = t.warning; break;
        case Kind::Error: c = t.danger;  break;
        case Kind::Ok:    c = t.success; break;
        default:          c = t.subText; break;
        }
        if (lines_[i].first != Kind::Info) g.fill(RECT{0, y + S(3), S(3), y + lh - S(3)}, c);
        RECT tr{S(12), y, w - S(12), y + lh};
        g.text(tr, lines_[i].second, c, t.fMono,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    RestoreDC(g.dc(), -1);
    if (lines_.empty()) {
        RECT tr{S(16), 0, w - S(16), h};
        g.text(tr, L"运行日志会显示在这里 · 支持 Ctrl+C 复制全部", t.muted, t.fSmall,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (maxScroll() > 0) {
        int trackH = h - S(12);
        int total = static_cast<int>(lines_.size()) * lh;
        int thumbH = std::max(S(28), trackH * trackH / total);
        int pos = maxScroll() ? (trackH - thumbH) * scrollY_ / maxScroll() : 0;
        g.round(RECT{w - S(6), S(6) + pos, w - S(2), S(6) + pos + thumbH}, S(3), t.scrollThumb);
    }
    g.round(RECT{0, 0, w, h}, S(8), kNone, t.border, 1);
    RestoreDC(g.dc(), -1);
}

void LogView::onWheel(int delta) {
    scrollY_ = std::max(0, std::min(maxScroll(),
                                   scrollY_ - delta / WHEEL_DELTA * lineHeight() * 3));
    autoScroll_ = scrollY_ >= maxScroll();
    invalidate();
}

void LogView::onKey(int vk, bool down) {
    if (!down) return;
    if ((GetKeyState(VK_CONTROL) < 0) && vk == L'C') {
        std::wstring all;
        for (auto& l : lines_) { all += l.second; all += L"\r\n"; }
        setClipboardText(hwnd_, all);
        return;
    }
    int lh = lineHeight();
    if (vk == VK_UP) scrollY_ -= lh;
    else if (vk == VK_DOWN) scrollY_ += lh;
    else if (vk == VK_PRIOR) scrollY_ -= height();
    else if (vk == VK_NEXT) scrollY_ += height();
    else if (vk == VK_HOME) scrollY_ = 0;
    else if (vk == VK_END) scrollY_ = maxScroll();
    else return;
    scrollY_ = std::max(0, std::min(maxScroll(), scrollY_));
    autoScroll_ = scrollY_ >= maxScroll();
    invalidate();
}

LRESULT LogView::message(UINT /*msg*/, WPARAM, LPARAM, bool&) { return 0; }

// ---------------------------------------------------------------------------
// Progress
// ---------------------------------------------------------------------------
void Progress::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    int barH = std::min(S(6), h);
    RECT track{0, (h - barH) / 2, w, (h + barH) / 2};
    g.round(track, barH / 2, t.scrollTrack);
    if (indeterminate) {
        int segW = std::max(S(24), w * 3 / 10);
        double p = (phase % 140) / 140.0;
        int x = static_cast<int>(-segW + p * (w + segW * 2));
        RECT seg{std::max(0, x), track.top,
                 std::min(w, x + segW), track.bottom};
        if (seg.right > seg.left) g.round(seg, barH / 2, t.accent);
    } else if (value > 0.0) {
        int fw = static_cast<int>(w * std::min(1.0, std::max(0.0, value)));
        if (fw > S(2)) g.round(RECT{0, track.top, fw, track.bottom}, barH / 2, t.accent);
    }
}

// ---------------------------------------------------------------------------
// ToastView: an independent top-most child HWND, so it is never clipped by
// WS_CLIPCHILDREN or by the parent's child-exclusion painting.
// ---------------------------------------------------------------------------
void ToastView::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{0, 0, w, h};
    COLORREF c = kind == 2 ? t.danger
               : kind == 1 ? t.warning
               : kind == 3 ? t.success : t.accent;
    g.round(r, S(8), t.card, t.border, 1);
    g.fill(RECT{0, S(8), S(4), h - S(8)}, c);
    RECT ir{S(14), S(14), S(42), S(42)};
    g.icon(kind == 2 ? Ic::Warning
         : kind == 3 ? Ic::Check : Ic::InfoCircle, ir, c, 2);
    g.text(RECT{S(50), 0, w - S(10), h}, text, t.text, t.fBody,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------
// Breadcrumb (Explorer-style address bar)
// ---------------------------------------------------------------------------
void Breadcrumb::setSegments(std::vector<Segment> s) {
    segments = std::move(s);
    hover_ = -1;
    invalidate();
}

int Breadcrumb::hit(int x) const {
    for (size_t i = 0; i < rects_.size(); ++i)
        if (x >= rects_[i].left && x < rects_[i].right) return static_cast<int>(i);
    return -1;
}

void Breadcrumb::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    g.round(RECT{0, 0, w, h}, S(6), t.control, t.border, 1);
    RECT iconR{S(6), (h - S(18)) / 2, S(6) + S(18), (h + S(18)) / 2};
    g.icon(Ic::Database, iconR, t.accent, 2);

    rects_.clear();
    int x = S(30);
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i) {
            RECT cr{x, (h - S(14)) / 2, x + S(16), (h + S(14)) / 2};
            g.icon(Ic::ChevronRight, cr, t.muted, 2);
            x += S(16);
        }
        std::wstring text = segments[i].text;
        SIZE sz = g.measure(text, t.fBody);
        int segW = std::max(S(40), static_cast<int>(sz.cx) + S(18));
        if (x + segW > w - S(6)) segW = w - S(6) - x;
        if (segW < S(18)) break;
        RECT seg{x, S(4), x + segW, h - S(4)};
        rects_.push_back(seg);
        if (static_cast<int>(i) == hover_)
            g.round(seg, S(4), t.gridHover);
        else if (static_cast<int>(i) + 1 == static_cast<int>(segments.size()))
            g.round(seg, S(4), t.accentSoft);
        g.text(inset(seg, S(8), 0), text,
               static_cast<int>(i) + 1 == static_cast<int>(segments.size())
                   ? t.text : t.subText,
               t.fBody, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        x += segW;
    }
}

void Breadcrumb::onMouseMove(int x, int /*y*/) {
    int i = hit(x);
    if (i != hover_) { hover_ = i; invalidate(); }
}

LRESULT Breadcrumb::message(UINT msg, WPARAM /*wp*/, LPARAM lp, bool& handled) {
    if (msg == WM_LBUTTONUP) {
        int i = hit(GET_X_LPARAM(lp));
        if (i >= 0 && i < static_cast<int>(segments.size()) && picked)
            picked(segments[i].dir);
        handled = true;
        return 0;
    }
    if (msg == WM_MOUSELEAVE) { hover_ = -1; invalidate(); }
    return 0;
}

// ---------------------------------------------------------------------------
// Menu (owner-drawn context menu)
// ---------------------------------------------------------------------------
int Menu::rowAt(int y) const {
    if (y < pad_) return -1;
    int cur = pad_;
    for (size_t i = 0; i < items_.size(); ++i) {
        int h = items_[i].separator ? sepH_ : itemH_;
        if (y >= cur && y < cur + h) return items_[i].separator ? -1 : static_cast<int>(i);
        cur += h;
    }
    return -1;
}

void Menu::show(HWND owner, POINT screen, std::vector<Item> items,
                std::function<void(int)> picked) {
    if (hwnd_) close();
    items_ = std::move(items);
    picked_ = std::move(picked);
    hover_ = -1;
    itemH_ = S(32);
    sepH_ = S(9);
    pad_ = S(6);

    HDC dc = GetDC(owner);
    int textW = 0;
    if (dc) {
        HGDIOBJ old = SelectObject(dc, theme().fBody);
        for (const auto& it : items_) {
            if (it.separator) continue;
            SIZE sz{};
            GetTextExtentPoint32W(dc, it.text.c_str(),
                                  static_cast<int>(it.text.size()), &sz);
            textW = std::max(textW, static_cast<int>(sz.cx));
        }
        SelectObject(dc, old);
        ReleaseDC(owner, dc);
    }
    int w = std::max(S(196), textW + S(96));
    int h = pad_ * 2;
    for (const auto& it : items_) h += it.separator ? sepH_ : itemH_;

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int x = screen.x, y = screen.y;
    if (x + w > sw - S(8)) x = std::max(S(8), sw - w - S(8));
    if (y + h > sh - S(8)) y = std::max(S(8), sh - h - S(8));
    if (x < S(8)) x = S(8);
    if (y < S(8)) y = S(8);

    create(owner, 0, RECT{x, y, x + w, y + h}, true);
    if (hwnd_) {
        SetFocus(hwnd_);
        SetCapture(hwnd_);
    }
}

void Menu::close() {
    if (!hwnd_) return;
    HWND h = hwnd_;
    hwnd_ = nullptr;
    ReleaseCapture();
    DestroyWindow(h);
}

void Menu::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    g.round(RECT{0, 0, w, h}, S(8), t.card, t.border, 1);
    int y = pad_;
    for (size_t i = 0; i < items_.size(); ++i) {
        const Item& it = items_[i];
        if (it.separator) {
            g.line(S(12), y + sepH_ / 2, w - S(12), y + sepH_ / 2, t.gridLine);
            y += sepH_;
            continue;
        }
        RECT row{S(4), y, w - S(4), y + itemH_};
        if (static_cast<int>(i) == hover_ && it.enabled)
            g.round(row, S(5), t.gridHover);
        if (it.icon != Ic::None) {
            RECT ir{row.left + S(10), row.top + S(6),
                    row.left + S(30), row.top + S(26)};
            g.icon(it.icon, ir, it.enabled ? t.subText : t.disabled, 2);
        }
        g.text(RECT{row.left + S(40), row.top, row.right - S(10), row.bottom},
               it.text, it.enabled ? t.text : t.disabled, t.fBody,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        y += itemH_;
    }
}

void Menu::onMouseMove(int /*x*/, int y) {
    int idx = rowAt(y);
    if (idx >= 0 && idx < static_cast<int>(items_.size()) && !items_[idx].enabled)
        idx = -1;
    if (idx != hover_) { hover_ = idx; invalidate(); }
}

void Menu::onKey(int vk, bool down) {
    if (!down) return;
    if (vk == VK_ESCAPE) { close(); return; }
    if (vk == VK_DOWN || vk == VK_UP) {
        int n = static_cast<int>(items_.size());
        if (!n) return;
        int i = hover_;
        for (int step = 0; step < n; ++step) {
            i += (vk == VK_DOWN ? 1 : -1);
            if (i < 0) i = n - 1;
            if (i >= n) i = 0;
            if (!items_[i].separator && items_[i].enabled) break;
        }
        hover_ = i;
        invalidate();
        return;
    }
    if (vk == VK_RETURN || vk == VK_SPACE) {
        int i = hover_;
        if (i >= 0 && i < static_cast<int>(items_.size()) && items_[i].enabled) {
            auto cb = picked_;
            close();
            if (cb) cb(i);
        }
    }
}

LRESULT Menu::message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) {
    if (msg == WM_LBUTTONDOWN) {
        int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        if (!PtInRect(&rc, POINT{x, y})) {
            close();
        } else {
            int idx = rowAt(y);
            if (idx >= 0 && idx < static_cast<int>(items_.size()) && items_[idx].enabled) {
                auto cb = picked_;
                int i = idx;
                close();
                if (cb) cb(i);
            }
        }
        handled = true;
        return 0;
    }
    if (msg == WM_KILLFOCUS || msg == WM_CAPTURECHANGED ||
        (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE)) {
        close();
        handled = true;
        return 0;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// NavItem
// ---------------------------------------------------------------------------
void NavItem::paint(Gfx& g) {
    Theme& t = theme();
    int w = width(), h = height();
    RECT r{S(8), 0, w - S(8), h};
    if (selected) {
        g.round(r, S(6), t.accentSoft);
        g.round(RECT{S(8), S(9), S(11), h - S(9)}, S(2), t.accent);
    } else if (hovered()) {
        g.round(r, S(6), t.gridHover);
    }
    if (focused()) g.round(inset(r, -S(1), -S(1)), S(7), kNone, t.accent, S(2));
    int is = S(18);
    RECT ir{S(20), (h - is) / 2, S(20) + is, (h + is) / 2};
    COLORREF c = selected ? t.accent : t.subText;
    g.icon(icon, ir, c, 2);
    RECT tr{S(20) + is + S(12), 0, w - S(12), h};
    g.text(tr, text, selected ? t.text : t.subText,
           selected ? t.fStrong : t.fBody,
           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

void NavItem::onClick() {
    if (activated) activated(index);
    Widget::onClick();
}

}  // namespace ui
