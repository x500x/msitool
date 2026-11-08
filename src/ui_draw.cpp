#include "ui.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>

namespace ui {
namespace {

const wchar_t* kWidgetClass = L"msitool_widget_v1";

Theme g_theme;
std::atomic<void*> g_logTarget{nullptr};

bool fontExists(const wchar_t* face) {
    struct Ctx { const wchar_t* want; bool found; } ctx{face, false};
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf,
        [](const LOGFONTW* elf, const TEXTMETRICW*, DWORD, LPARAM param) -> int {
            auto* c = reinterpret_cast<Ctx*>(param);
            if (elf && _wcsicmp(elf->lfFaceName, c->want) == 0) {
                c->found = true;
                return 0;
            }
            return 1;
        },
        reinterpret_cast<LPARAM>(&ctx), 0);
    ReleaseDC(nullptr, dc);
    return ctx.found;
}

HFONT makeFont(int px, int weight, const wchar_t* face) {
    return CreateFontW(-g_theme.s(px), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

}  // namespace

Theme& theme() { return g_theme; }
int S(int px) { return g_theme.s(px); }

COLORREF mix(COLORREF a, COLORREF b, int percentOfB) {
    if (percentOfB <= 0) return a;
    if (percentOfB >= 100) return b;
    auto channel = [&](int shift) {
        int x = (a >> shift) & 0xFF;
        int y = (b >> shift) & 0xFF;
        return x + (y - x) * percentOfB / 100;
    };
    return RGB(channel(0), channel(8), channel(16));
}

void Theme::dispose() noexcept {
    HFONT fonts[] = {fSmall, fBody, fStrong, fSubtitle, fTitle, fMono};
    for (HFONT f : fonts) if (f) DeleteObject(f);
    HBRUSH brushes[] = {brCard, brControl, brAccent, brBg, brHead};
    for (HBRUSH b : brushes) if (b) DeleteObject(b);
    fSmall = fBody = fStrong = fSubtitle = fTitle = fMono = nullptr;
    brCard = brControl = brAccent = brBg = brHead = nullptr;
}

void Theme::rebuild(HWND hwnd, bool darkMode, int dpiOverride) {
    dispose();
    dark = darkMode;
    dpi = dpiOverride > 0 ? dpiOverride : systemDpi(hwnd);

    if (!dark) {
        bgTop        = RGB(247, 249, 252);
        bgBottom     = RGB(234, 240, 248);
        navBg        = RGB(241, 244, 249);
        card         = RGB(255, 255, 255);
        cardAlt      = RGB(249, 251, 253);
        border       = RGB(226, 231, 238);
        borderStrong = RGB(203, 212, 223);
        text         = RGB(27, 29, 33);
        subText      = RGB(94, 101, 112);
        muted        = RGB(137, 144, 155);
        disabled     = RGB(172, 178, 187);
        accent       = RGB(15, 108, 189);
        accentHover  = RGB(17, 94, 163);
        accentPressed= RGB(12, 62, 105);
        accentSoft   = RGB(228, 240, 252);
        accentText   = RGB(255, 255, 255);
        control      = RGB(255, 255, 255);
        controlHover = RGB(244, 247, 251);
        controlPressed = RGB(235, 240, 247);
        gridHead     = RGB(247, 249, 252);
        gridAlt      = RGB(250, 251, 253);
        gridHover    = RGB(239, 244, 250);
        gridLine     = RGB(232, 236, 242);
        success      = RGB(15, 122, 58);
        warning      = RGB(151, 91, 0);
        danger       = RGB(193, 42, 28);
        infoBarOk    = RGB(222, 245, 227);
        infoBarBad   = RGB(253, 231, 228);
        infoBarWarn  = RGB(255, 243, 211);
        scrollThumb  = RGB(185, 194, 207);
        scrollTrack  = RGB(238, 241, 246);
        heroA        = RGB(13, 96, 175);
        heroB        = RGB(63, 157, 255);
        selection    = RGB(226, 239, 252);
    } else {
        bgTop        = RGB(36, 38, 42);
        bgBottom     = RGB(25, 26, 30);
        navBg        = RGB(31, 33, 37);
        card         = RGB(44, 46, 51);
        cardAlt      = RGB(49, 51, 57);
        border       = RGB(64, 68, 75);
        borderStrong = RGB(86, 91, 100);
        text         = RGB(245, 246, 248);
        subText      = RGB(192, 197, 205);
        muted        = RGB(148, 154, 164);
        disabled     = RGB(105, 110, 119);
        accent       = RGB(96, 205, 255);
        accentHover  = RGB(126, 216, 255);
        accentPressed= RGB(72, 173, 224);
        accentSoft   = RGB(28, 60, 82);
        accentText   = RGB(6, 27, 43);
        control      = RGB(53, 56, 62);
        controlHover = RGB(64, 68, 75);
        controlPressed = RGB(46, 49, 55);
        gridHead     = RGB(49, 52, 58);
        gridAlt      = RGB(46, 48, 54);
        gridHover    = RGB(60, 64, 71);
        gridLine     = RGB(67, 71, 79);
        success      = RGB(108, 203, 112);
        warning      = RGB(250, 198, 88);
        danger       = RGB(255, 151, 139);
        infoBarOk    = RGB(31, 58, 42);
        infoBarBad   = RGB(66, 38, 38);
        infoBarWarn  = RGB(64, 53, 30);
        scrollThumb  = RGB(100, 106, 116);
        scrollTrack  = RGB(40, 42, 47);
        heroA        = RGB(18, 78, 132);
        heroB        = RGB(39, 137, 210);
        selection    = RGB(30, 66, 92);
    }

    const wchar_t* uiFace = L"Segoe UI";
    if (PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE) {
        if (fontExists(L"Microsoft YaHei UI")) uiFace = L"Microsoft YaHei UI";
    } else if (fontExists(L"Segoe UI Variable Text")) {
        uiFace = L"Segoe UI Variable Text";
    }
    const wchar_t* monoFace = fontExists(L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";

    fSmall    = makeFont(12, FW_NORMAL, uiFace);
    fBody     = makeFont(14, FW_NORMAL, uiFace);
    fStrong   = makeFont(14, FW_SEMIBOLD, uiFace);
    fSubtitle = makeFont(16, FW_SEMIBOLD, uiFace);
    fTitle    = makeFont(26, FW_SEMIBOLD, uiFace);
    fMono     = makeFont(13, FW_NORMAL, monoFace);

    brCard    = CreateSolidBrush(card);
    brControl = CreateSolidBrush(control);
    brAccent  = CreateSolidBrush(accent);
    brBg      = CreateSolidBrush(bgTop);
    brHead    = CreateSolidBrush(gridHead);
}

int systemDpi(HWND hwnd) {
    // QA hook: "MSITOOL_DPI=144" renders the whole UI at 144 DPI (150%) even on
    // a 100% display, so layout and scaling can be checked / screenshot-tested
    // on a single-DPI machine. Out-of-range values are ignored.
    wchar_t forced[16]{};
    DWORD n = GetEnvironmentVariableW(L"MSITOOL_DPI", forced, 16);
    if (n > 0 && n < 16) {
        int v = _wtoi(forced);
        if (v >= 72 && v <= 480) return v;
    }

    using Fn = UINT(WINAPI*)(HWND);
    auto fn = reinterpret_cast<Fn>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (fn && hwnd) {
        UINT d = fn(hwnd);
        if (d >= 72 && d <= 480) return static_cast<int>(d);
    }
    HDC dc = GetDC(hwnd);
    int d = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return d > 0 ? d : 96;
}

void enableDpiAwareness() {
    HMODULE user = GetModuleHandleW(L"user32.dll");
    if (!user) return;
    using SetCtxFn = BOOL(WINAPI*)(void*);
    auto setCtx = reinterpret_cast<SetCtxFn>(
        GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
    if (setCtx && setCtx(reinterpret_cast<void*>(static_cast<INT_PTR>(-4)))) return;
    using SetAwareFn = BOOL(WINAPI*)();
    auto setAware = reinterpret_cast<SetAwareFn>(
        GetProcAddress(user, "SetProcessDPIAware"));
    if (setAware) setAware();
}

bool systemPrefersDark() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    LSTATUS st = RegGetValueW(
        HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return st == ERROR_SUCCESS && value == 0;
}

void prepareGuiProcess() {
    // The existing CLI implementation writes to stdout/stderr. In GUI mode
    // there is no console, so point those streams at NUL; the log sink below
    // still delivers every line to the UI.
    _wfreopen(L"NUL", L"w", stdout);
    _wfreopen(L"NUL", L"w", stderr);
    _wfreopen(L"NUL", L"r", stdin);
    util::setLogSink([](bool isError, const std::wstring& line) {
        void* target = g_logTarget.load(std::memory_order_relaxed);
        if (!target) return;
        auto* copy = new std::wstring(line);
        if (!PostMessageW(static_cast<HWND>(target), WM_APP_LOG,
                          isError ? 1 : 0, reinterpret_cast<LPARAM>(copy))) {
            delete copy;
        }
    });
}

void setLogTarget(HWND hwnd) {
    g_logTarget.store(hwnd, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Gfx
// ---------------------------------------------------------------------------
Gfx::Gfx(HDC target, const RECT& bounds) : target_(target), bounds_(bounds) {
    int w = bounds.right - bounds.left;
    int h = bounds.bottom - bounds.top;
    if (w <= 0 || h <= 0) return;
    mem_ = CreateCompatibleDC(target);
    if (mem_) {
        bmp_ = CreateCompatibleBitmap(target, w, h);
        if (bmp_) {
            old_ = static_cast<HBITMAP>(SelectObject(mem_, bmp_));
            SetViewportOrgEx(mem_, -bounds.left, -bounds.top, nullptr);
        } else {
            DeleteDC(mem_);
            mem_ = nullptr;
        }
    }
}

Gfx::~Gfx() {
    if (!flushed_) flush();
    if (mem_) {
        if (old_) SelectObject(mem_, old_);
        if (bmp_) DeleteObject(bmp_);
        DeleteDC(mem_);
    }
}

void Gfx::flush() {
    if (!mem_ || flushed_) return;
    flushed_ = true;
    BitBlt(target_, bounds_.left, bounds_.top,
           bounds_.right - bounds_.left, bounds_.bottom - bounds_.top,
           mem_, 0, 0, SRCCOPY);
}

void Gfx::exclude(const RECT& r) {
    ExcludeClipRect(target_, r.left, r.top, r.right, r.bottom);
}

void Gfx::clip(const RECT& r) {
    IntersectClipRect(dc(), r.left, r.top, r.right, r.bottom);
}

void Gfx::fill(const RECT& r, COLORREF c) {
    if (c == kNone || r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH b = CreateSolidBrush(c);
    RECT tmp = r;
    FillRect(dc(), &tmp, b);
    DeleteObject(b);
}

void Gfx::gradient(const RECT& r, COLORREF a, COLORREF b, bool vertical) {
    int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0) return;
    const int steps = vertical ? h : w;
    for (int i = 0; i < steps; ++i) {
        int p = steps <= 1 ? 0 : i * 100 / (steps - 1);
        COLORREF c = mix(a, b, p);
        RECT band = vertical ? RECT{r.left, r.top + i, r.right, r.top + i + 1}
                             : RECT{r.left + i, r.top, r.left + i + 1, r.bottom};
        fill(band, c);
    }
}

void Gfx::round(const RECT& r, int radius, COLORREF fillColor,
                COLORREF strokeColor, int strokeWidth) {
    if (r.right <= r.left || r.bottom <= r.top) return;
    HBRUSH brush = fillColor != kNone
        ? CreateSolidBrush(fillColor)
        : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = strokeColor != kNone
        ? CreatePen(PS_SOLID, std::max(1, strokeWidth), strokeColor)
        : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldB = SelectObject(dc(), brush);
    HGDIOBJ oldP = SelectObject(dc(), pen);
    int d = std::max(2, radius * 2);
    RoundRect(dc(), r.left, r.top, r.right, r.bottom, d, d);
    SelectObject(dc(), oldP);
    SelectObject(dc(), oldB);
    if (fillColor != kNone) DeleteObject(brush);
    if (strokeColor != kNone) DeleteObject(pen);
}

void Gfx::rect(const RECT& r, COLORREF strokeColor, int strokeWidth) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, strokeWidth), strokeColor);
    HGDIOBJ oldP = SelectObject(dc(), pen);
    HGDIOBJ oldB = SelectObject(dc(), GetStockObject(NULL_BRUSH));
    Rectangle(dc(), r.left, r.top, r.right, r.bottom);
    SelectObject(dc(), oldB);
    SelectObject(dc(), oldP);
    DeleteObject(pen);
}

void Gfx::circle(int cx, int cy, int radius, COLORREF fillColor,
                 COLORREF strokeColor, int strokeWidth) {
    HBRUSH brush = fillColor != kNone
        ? CreateSolidBrush(fillColor)
        : static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    HPEN pen = strokeColor != kNone
        ? CreatePen(PS_SOLID, std::max(1, strokeWidth), strokeColor)
        : static_cast<HPEN>(GetStockObject(NULL_PEN));
    HGDIOBJ oldB = SelectObject(dc(), brush);
    HGDIOBJ oldP = SelectObject(dc(), pen);
    Ellipse(dc(), cx - radius, cy - radius, cx + radius, cy + radius);
    SelectObject(dc(), oldP);
    SelectObject(dc(), oldB);
    if (fillColor != kNone) DeleteObject(brush);
    if (strokeColor != kNone) DeleteObject(pen);
}

void Gfx::line(int x1, int y1, int x2, int y2, COLORREF c, int width) {
    HPEN pen = CreatePen(PS_SOLID, std::max(1, width), c);
    HGDIOBJ old = SelectObject(dc(), pen);
    MoveToEx(dc(), x1, y1, nullptr);
    LineTo(dc(), x2, y2);
    SelectObject(dc(), old);
    DeleteObject(pen);
}

void Gfx::text(const RECT& r, const std::wstring& s, COLORREF c, HFONT font,
               UINT format) {
    if (s.empty()) return;
    HGDIOBJ old = font ? SelectObject(dc(), font) : nullptr;
    SetBkMode(dc(), TRANSPARENT);
    SetTextColor(dc(), c);
    RECT tmp = r;
    DrawTextW(dc(), s.c_str(), static_cast<int>(s.size()), &tmp, format);
    if (old) SelectObject(dc(), old);
}

SIZE Gfx::measure(const std::wstring& s, HFONT font) const {
    SIZE sz{0, 0};
    if (s.empty()) return sz;
    HGDIOBJ old = font ? SelectObject(dc(), font) : nullptr;
    GetTextExtentPoint32W(dc(), s.c_str(), static_cast<int>(s.size()), &sz);
    if (old) SelectObject(dc(), old);
    return sz;
}

// ---------------------------------------------------------------------------
// Vector icons. All shapes are authored on a 24x24 grid and scaled into the
// requested rectangle with a plain GDI pen - no fonts, no bitmaps, no libs.
// ---------------------------------------------------------------------------
void Gfx::icon(Ic id, const RECT& r, COLORREF color, int strokeWidth) {
    if (id == Ic::None) return;
    int rw = r.right - r.left, rh = r.bottom - r.top;
    if (rw < 4 || rh < 4) return;
    double sc = static_cast<double>(std::min(rw, rh)) / 24.0;
    int ox = r.left + static_cast<int>((rw - 24.0 * sc) / 2.0);
    int oy = r.top + static_cast<int>((rh - 24.0 * sc) / 2.0);
    HDC dc = Gfx::dc();
    HPEN pen = CreatePen(PS_SOLID, std::max(1, static_cast<int>(strokeWidth * sc)), color);
    HGDIOBJ oldP = SelectObject(dc, pen);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(NULL_BRUSH));
    SetBkMode(dc, TRANSPARENT);

    struct Draw {
        HDC dc; double sc; int ox, oy; COLORREF color;
        int X(double v) const { return ox + static_cast<int>(v * sc + 0.5); }
        int Y(double v) const { return oy + static_cast<int>(v * sc + 0.5); }
        void seg(double x1, double y1, double x2, double y2) const {
            MoveToEx(dc, X(x1), Y(y1), nullptr);
            LineTo(dc, X(x2), Y(y2));
        }
        void path(std::initializer_list<std::pair<double, double>> pts) const {
            std::vector<POINT> p;
            for (auto& q : pts) p.push_back({X(q.first), Y(q.second)});
            if (!p.empty()) Polyline(dc, p.data(), static_cast<int>(p.size()));
        }
        void shape(std::initializer_list<std::pair<double, double>> pts, bool close = true) const {
            std::vector<POINT> p;
            for (auto& q : pts) p.push_back({X(q.first), Y(q.second)});
            if (close && !p.empty()) p.push_back(p.front());
            if (!p.empty()) Polyline(dc, p.data(), static_cast<int>(p.size()));
        }
        void box(double x, double y, double w, double h, double rad = 1.5) const {
            int d = std::max(2, static_cast<int>(rad * 2 * sc));
            RoundRect(dc, X(x), Y(y), X(x + w), Y(y + h), d, d);
        }
        void ell(double x, double y, double w, double h) const {
            Ellipse(dc, X(x), Y(y), X(x + w), Y(y + h));
        }
        void arc(double x, double y, double w, double h,
                 double sx, double sy, double ex, double ey) const {
            Arc(dc, X(x), Y(y), X(x + w), Y(y + h), X(sx), Y(sy), X(ex), Y(ey));
        }
        void dot(double x, double y, double d) const {
            HBRUSH b = CreateSolidBrush(color);
            HGDIOBJ old = SelectObject(dc, b);
            Ellipse(dc, X(x), Y(y), X(x + d), Y(y + d));
            SelectObject(dc, old);
            DeleteObject(b);
        }
    } d{dc, sc, ox, oy, color};

    switch (id) {
    case Ic::App:
        d.box(2, 2, 20, 20, 5);
        d.path({{6.5, 16}, {6.5, 8}, {12, 14}, {17.5, 8}, {17.5, 16}});
        break;
    case Ic::Home:
        d.path({{3, 11}, {12, 4}, {21, 11}});
        d.box(5, 10, 14, 10, 1);
        d.box(10, 14, 4, 6, 0.5);
        break;
    case Ic::Info:
    case Ic::InfoCircle:
        d.ell(3, 3, 18, 18);
        d.seg(12, 11, 12, 17);
        d.dot(11, 6.5, 2);
        break;
    case Ic::Table:
        d.box(3, 4, 18, 16, 1.5);
        d.seg(3, 9, 21, 9);
        d.seg(3, 14, 21, 14);
        d.seg(11, 4, 11, 20);
        break;
    case Ic::Query:
    case Ic::Search:
        d.ell(3.5, 3.5, 13, 13);
        d.seg(13.5, 13.5, 20.5, 20.5);
        if (id == Ic::Query) { d.seg(7, 9, 13, 9); d.seg(7, 12, 11, 12); }
        break;
    case Ic::Extract:
        d.path({{12, 3}, {12, 14}, {8, 10}, {12, 14}, {16, 10}});
        d.path({{4, 15}, {4, 20}, {20, 20}, {20, 15}});
        break;
    case Ic::Export:
        d.box(3, 4, 14, 15, 1.5);
        d.path({{7, 11.5}, {20, 11.5}, {17, 8.5}, {20, 11.5}, {17, 14.5}});
        break;
    case Ic::Pack:
        d.box(3, 7, 18, 13, 1.2);
        d.path({{3, 7}, {6, 3}, {18, 3}, {21, 7}});
        d.box(10, 7, 4, 5, 0.5);
        break;
    case Ic::Shield:
        d.shape({{12, 2}, {20.5, 5.5}, {20.5, 12}, {12, 22}, {3.5, 12}, {3.5, 5.5}});
        d.path({{8, 12}, {11, 15}, {16.5, 9}});
        break;
    case Ic::Settings:
        d.ell(8, 8, 8, 8);
        d.ell(3.5, 3.5, 17, 17);
        for (int i = 0; i < 8; ++i) {
            double a = 3.1415926535 * 2.0 * i / 8.0;
            d.seg(12 + 5.5 * cos(a), 12 + 5.5 * sin(a),
                  12 + 9.5 * cos(a), 12 + 9.5 * sin(a));
        }
        break;
    case Ic::Sun:
        d.ell(8, 8, 8, 8);
        for (int i = 0; i < 8; ++i) {
            double a = 3.1415926535 * 2.0 * i / 8.0;
            d.seg(12 + 6.5 * cos(a), 12 + 6.5 * sin(a),
                  12 + 9.5 * cos(a), 12 + 9.5 * sin(a));
        }
        break;
    case Ic::Moon:
        d.arc(3, 3, 18, 18, 15, 3, 15, 21);
        d.arc(7, 1, 18, 18, 15, 21, 15, 3);
        break;
    case Ic::Folder:
        d.shape({{3, 7}, {9, 7}, {11, 5}, {19, 5}, {21, 8}, {21, 19}, {3, 19}});
        break;
    case Ic::Play:
        d.shape({{8, 5}, {18, 12}, {8, 19}});
        break;
    case Ic::Stop:
        d.box(6, 6, 12, 12, 1);
        break;
    case Ic::ChevronDown:
        d.path({{7, 10}, {12, 15}, {17, 10}});
        break;
    case Ic::ChevronRight:
        d.path({{10, 7}, {15, 12}, {10, 17}});
        break;
    case Ic::ArrowLeft:
        d.path({{20, 12}, {4, 12}, {10, 6}, {4, 12}, {10, 18}});
        break;
    case Ic::ArrowUp:
        d.path({{12, 20}, {12, 4}, {6, 10}, {12, 4}, {18, 10}});
        break;
    case Ic::Close:
    case Ic::Cross:
        d.seg(6, 6, 18, 18);
        d.seg(18, 6, 6, 18);
        break;
    case Ic::Min:
        d.seg(5, 12, 19, 12);
        break;
    case Ic::Max:
        d.box(6, 6, 12, 12, 0.5);
        break;
    case Ic::Restore:
        d.box(8, 4, 11, 11, 0.5);
        d.path({{8, 8}, {5, 8}, {5, 19}, {16, 19}, {16, 16}});
        break;
    case Ic::Check:
        d.path({{5, 12.5}, {10, 17}, {19, 7}});
        break;
    case Ic::Refresh:
        d.arc(4, 4, 16, 16, 20, 7, 20, 17);
        d.arc(4, 4, 16, 16, 4, 17, 4, 7);
        d.path({{17, 4}, {21, 7}, {17, 10}});
        d.path({{7, 20}, {3, 17}, {7, 14}});
        break;
    case Ic::Copy:
        d.box(8, 8, 12, 12, 1);
        d.path({{16, 8}, {16, 5}, {4, 5}, {4, 16}, {8, 16}});
        break;
    case Ic::Add:
        d.seg(12, 5, 12, 19);
        d.seg(5, 12, 19, 12);
        break;
    case Ic::Bolt:
        d.shape({{13, 2}, {6, 13}, {11, 13}, {9, 22}, {18, 10}, {12.5, 10}});
        break;
    case Ic::Database:
        d.ell(3, 4, 18, 6);
        d.seg(3, 7, 3, 17);
        d.seg(21, 7, 21, 17);
        d.arc(3, 11, 18, 12, 21, 17, 3, 17);
        break;
    case Ic::File:
        d.shape({{6, 2}, {15, 2}, {19, 6}, {19, 22}, {6, 22}});
        d.path({{15, 2}, {15, 6}, {19, 6}});
        d.seg(9, 12, 16, 12);
        d.seg(9, 16, 16, 16);
        break;
    case Ic::Open:
        d.shape({{3, 8}, {9, 8}, {11, 6}, {18, 6}, {20, 9}, {20, 19}, {3, 19}});
        d.path({{13, 3}, {21, 3}, {21, 10}});
        break;
    case Ic::Save:
        d.box(4, 3, 16, 18, 1);
        d.box(8, 3, 9, 7, 0.3);
        d.box(7, 13, 10, 8, 0.3);
        break;
    case Ic::Clock:
        d.ell(3, 3, 18, 18);
        d.path({{12, 7}, {12, 12}, {16, 14}});
        break;
    case Ic::ArrowRight:
        d.path({{4, 12}, {20, 12}, {16, 8}, {20, 12}, {16, 16}});
        break;
    case Ic::Terminal:
        d.box(3, 4, 18, 16, 1.5);
        d.path({{7, 9}, {10, 12}, {7, 15}});
        d.seg(12, 16, 17, 16);
        break;
    case Ic::Warning:
        d.shape({{12, 3}, {22, 20}, {2, 20}});
        d.seg(12, 9, 12, 15);
        d.dot(11, 17, 2);
        break;
    case Ic::Sparkle:
        d.shape({{12, 2}, {14, 10}, {22, 12}, {14, 14}, {12, 22}, {10, 14}, {2, 12}, {10, 10}});
        d.seg(19, 3, 19, 6);
        d.seg(17.5, 4.5, 20.5, 4.5);
        break;
    case Ic::None:
        break;
    }

    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    DeleteObject(pen);
}

// Widget window-class registration lives here so that all UI modules share it.
void Widget::registerClass(HINSTANCE instance) {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = Widget::proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWidgetClass;
    RegisterClassExW(&wc);
}

const wchar_t* widgetClassName() { return kWidgetClass; }

}  // namespace ui
