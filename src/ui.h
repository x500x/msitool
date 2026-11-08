#pragma once

// ---------------------------------------------------------------------------
// msitool UI layer - public surface.
//
// Everything here is built directly on the Windows API (user32 / gdi32 /
// shell32 / ole32). No third-party or UI framework library is used: every
// widget is an owner-drawn HWND, every pixel is painted with plain GDI into a
// double buffered memory DC. The only "system edit" control used is the
// built-in EDIT window (for IME, caret, clipboard and selection behaviour);
// even its border, cue banner and focus ring are drawn by us.
// ---------------------------------------------------------------------------

#include "util.h"          // windows.h, shellapi.h, strings, vectors ...
#include <windowsx.h>      // GET_X_LPARAM / GET_Y_LPARAM
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ui {

constexpr COLORREF kNone = 0xFFFFFFFFu;

// ---- custom messages -------------------------------------------------------
constexpr UINT WM_APP_LOG       = WM_APP + 1;  // LPARAM = new std::wstring*, WPARAM = kind
constexpr UINT WM_APP_TASK_DONE = WM_APP + 2;  // LPARAM = new TaskResult*
constexpr UINT WM_APP_ESC       = WM_APP + 3;
constexpr UINT WM_APP_REFRESH   = WM_APP + 4;
constexpr UINT WM_APP_OPEN      = WM_APP + 5;
constexpr UINT WM_APP_FOCUS_NEXT = WM_APP + 6;   // WPARAM = current HWND, LPARAM = shift

struct TaskResult {
    bool          ok = false;
    std::wstring  message;
    unsigned long long elapsedMs = 0;
};

// ---------------------------------------------------------------------------
// Theme: Fluent light / dark palettes, DPI-scaled metrics and GDI resources.
// ---------------------------------------------------------------------------
struct Theme {
    bool dark = false;
    int  dpi  = 96;

    COLORREF bgTop, bgBottom, navBg, card, cardAlt, border, borderStrong;
    COLORREF text, subText, muted, disabled;
    COLORREF accent, accentHover, accentPressed, accentSoft, accentText;
    COLORREF control, controlHover, controlPressed;
    COLORREF gridHead, gridAlt, gridHover, gridLine;
    COLORREF success, warning, danger, infoBarOk, infoBarBad, infoBarWarn;
    COLORREF scrollThumb, scrollTrack, heroA, heroB, selection;

    HFONT fSmall = nullptr, fBody = nullptr, fStrong = nullptr,
          fSubtitle = nullptr, fTitle = nullptr, fMono = nullptr;
    HBRUSH brCard = nullptr, brControl = nullptr, brAccent = nullptr,
           brBg = nullptr, brHead = nullptr;

    int s(int px) const noexcept { return MulDiv(px, dpi, 96); }
    void rebuild(HWND hwnd, bool darkMode);
    void dispose() noexcept;
};

Theme& theme();
const wchar_t* widgetClassName();
int    S(int px);                         // scale from 96 DPI
void   enableDpiAwareness();
int    systemDpi(HWND hwnd);
COLORREF mix(COLORREF a, COLORREF b, int percentOfB);
bool   systemPrefersDark();

// ---------------------------------------------------------------------------
// Icons: all drawn as 24x24 vector line art with GDI (no icon resources, no
// icon font dependency, no image library).
// ---------------------------------------------------------------------------
enum class Ic {
    None, App, Home, Info, Table, Query, Extract, Export, Pack, Shield,
    Settings, Sun, Moon, Folder, Play, Stop, ChevronDown, ChevronRight,
    ArrowLeft, ArrowUp, Close, Min, Max,
    Restore, Check, Cross, Refresh, Search, Copy, Add, Bolt, Database, File,
    Open, Save, Clock, ArrowRight, Terminal, InfoCircle, Warning, Sparkle
};

// ---------------------------------------------------------------------------
// Gfx: double buffered painter. All coordinates are relative to the window
// whose HDC was passed in (usually the widget's client area).
// ---------------------------------------------------------------------------
class Gfx {
public:
    Gfx(HDC target, const RECT& bounds);
    ~Gfx();
    Gfx(const Gfx&) = delete;
    Gfx& operator=(const Gfx&) = delete;

    HDC  dc() const noexcept { return mem_ ? mem_ : target_; }
    void flush();
    void exclude(const RECT& r);               // don't paint over a child HWND
    void clip(const RECT& r);                 // intersect clipping rect
    void fill(const RECT& r, COLORREF c);
    void gradient(const RECT& r, COLORREF a, COLORREF b, bool vertical = true);
    void round(const RECT& r, int radius, COLORREF fill,
               COLORREF stroke = kNone, int strokeWidth = 1);
    void rect(const RECT& r, COLORREF stroke, int strokeWidth = 1);
    void circle(int cx, int cy, int radius, COLORREF fill,
                COLORREF stroke = kNone, int strokeWidth = 1);
    void line(int x1, int y1, int x2, int y2, COLORREF c, int width = 1);
    void text(const RECT& r, const std::wstring& s, COLORREF c, HFONT font,
              UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    void icon(Ic id, const RECT& r, COLORREF c, int strokeWidth = 2);
    SIZE measure(const std::wstring& s, HFONT font) const;

private:
    HDC     target_;
    RECT    bounds_;
    HDC     mem_ = nullptr;
    HBITMAP bmp_ = nullptr;
    HBITMAP old_ = nullptr;
    bool    flushed_ = false;
};

enum class Backdrop { Card, Nav };

// ---------------------------------------------------------------------------
// Widget: base of every owner-drawn control. One HWND per widget, with a
// single shared window procedure routing messages back to the C++ object.
// ---------------------------------------------------------------------------
class Widget {
public:
    Widget() = default;
    virtual ~Widget();

    static void registerClass(HINSTANCE instance);

    void create(HWND parent, int id, const RECT& rc, bool popup = false);
    virtual void setRect(const RECT& r);
    const RECT& rect() const noexcept { return rc_; }
    void setVisible(bool v);
    bool visible() const noexcept { return visible_; }
    void setEnabled(bool v);
    bool enabled() const noexcept { return enabled_; }
    void setPage(int p) noexcept { page_ = p; }
    int  page() const noexcept { return page_; }
    // Optional sub-tab on a page (used by the pack form). -1 = always shown.
    void setTab(int t) noexcept { tab_ = t; }
    int  tab() const noexcept { return tab_; }
    void setTip(const std::wstring& t) { tip_ = t; }
    virtual void invalidate();
    virtual void takeFocus();
    HWND hwnd() const noexcept { return hwnd_; }
    int  id() const noexcept { return id_; }
    int  width() const noexcept { return rc_.right - rc_.left; }
    int  height() const noexcept { return rc_.bottom - rc_.top; }

    std::function<void()> clicked;
    Backdrop backdrop = Backdrop::Card;   // colour used to clear the client area

    virtual void paint(Gfx& /*g*/) {}
    virtual void onClick() { if (clicked) clicked(); }
    virtual void onDoubleClick() {}
    virtual void onMouseMove(int /*x*/, int /*y*/) {}
    virtual void onWheel(int /*delta*/) {}
    virtual void onKey(int /*vk*/, bool /*down*/) {}
    virtual void onFocus(bool /*focused*/) {}
    virtual void onThemeChanged() {}
    virtual void afterCreate() {}
    virtual LRESULT message(UINT /*msg*/, WPARAM /*wp*/, LPARAM /*lp*/, bool& handled) {
        handled = false;
        return 0;
    }

protected:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    HWND  hwnd_ = nullptr;
    HWND  parent_ = nullptr;
    RECT  rc_{};
    int   id_ = 0;
    int   page_ = -1;
    int   tab_ = -1;
    bool  visible_ = true;
    bool  enabled_ = true;
    bool  hover_ = false;
    bool  pressed_ = false;
    bool  focus_ = false;
    bool  popup_ = false;
    POINT mouse_{};
    std::wstring tip_;

    bool hovered() const noexcept { return hover_; }
    bool pressed() const noexcept { return pressed_; }
    // Query the real focus instead of trusting a possibly stale flag: a
    // partially repainted control must never keep painting a focus cue.
    bool focused() const noexcept {
        HWND f = GetFocus();
        return f && hwnd_ && (f == hwnd_ || IsChild(hwnd_, f));
    }
};

// ---------------------------------------------------------------------------
// Controls
// ---------------------------------------------------------------------------
class Button : public Widget {
public:
    std::wstring text;
    Ic    icon = Ic::None;
    bool  accent = false;
    bool  subtle = false;
    bool  danger = false;
    bool  caption = false;       // square, borderless caption-bar button
    bool  captionClose = false;  // red hover on the close button
    int   glyphSize = 16;
    void  paint(Gfx& g) override;
    void  onKey(int vk, bool down) override;
};

class Check : public Widget {
public:
    enum class Style { Box, Switch };
    Style style = Style::Box;
    std::wstring text;
    bool checked = false;
    std::function<void(bool)> changed;
    void paint(Gfx& g) override;
    void onClick() override;
    void onKey(int vk, bool down) override;
};

class TextBox : public Widget {
public:
    bool multiline = false;
    bool readOnly = false;
    bool password = false;
    std::wstring placeholder;
    std::function<void()> enter;
    std::function<void()> changed;

    std::wstring value() const;
    void setValue(const std::wstring& s);
    void selectAll();
    void focusEdit();
    // Drag & drop feedback: 0 = off, 1 = a file drag is over the window and
    // this field accepts it, 2 = the drag currently hovers this field.
    void setDropGlow(int level);
    int  dropGlow() const noexcept { return dropGlow_; }
    void onThemeChanged() override;
    void invalidate() override;       // also refresh the inner EDIT child
    void takeFocus() override;        // focus the inner EDIT, not the frame
    bool isEdit(HWND h) const noexcept { return h && h == edit_; }
    void paint(Gfx& g) override;
    void afterCreate() override;
    void setRect(const RECT& r) override;   // re-layouts the inner EDIT
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;

private:
    static LRESULT CALLBACK editProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    void layoutEdit();
    HWND    edit_ = nullptr;
    WNDPROC oldEdit_ = nullptr;
    int     dropGlow_ = 0;
};

class Popup : public Widget {
public:
    std::vector<std::wstring> items;
    int  index = 0;
    int  hover = -1;
    std::function<void(int)> picked;
    void show(HWND owner, int x, int y, int width);
    void close();
    void paint(Gfx& g) override;
    void onMouseMove(int x, int y) override;
    void onKey(int vk, bool down) override;
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
};

class DropDown : public Widget {
public:
    std::vector<std::wstring> items;
    int  index = 0;
    std::function<void(int)> selected;
    void setItems(std::vector<std::wstring> values);
    void setIndex(int i);
    std::wstring value() const;
    void paint(Gfx& g) override;
    void onClick() override;
    void onKey(int vk, bool down) override;

private:
    void openPopup();
    std::unique_ptr<Popup> popup_;
};

class DataGrid : public Widget {
public:
    bool headerVisible = true;
    bool zebra = true;
    bool rounded = true;
    int  rowHeightPx = 0;            // 0 = default S(32)
    int  headHeightPx = 0;           // 0 = default S(36)
    std::function<void(int)> selectionChanged;
    std::function<void(int)> rowActivated;
    std::function<void(int row, POINT screen)> itemContextMenu;
    std::function<void(int col)> headerClicked;   // click a column to sort
    std::function<void(int row)> iconClicked;     // click the row's icon area
    std::function<void()> goUp;                   // Backspace
    std::function<void()> refreshRequested;       // F5

    void setData(std::vector<std::wstring> headers,
                 std::vector<std::vector<std::wstring>> rows,
                 std::vector<int> widths = {},
                 std::vector<Ic> rowIcons = {});
    void clear();
    void setColumnWidths(std::vector<int> widths);  // adaptive column sizing
    void setRect(const RECT& r) override;   // clamps scroll after a resize
    int  selected() const noexcept { return selected_; }
    void select(int row);
    int  rowCount() const noexcept { return static_cast<int>(rows_.size()); }
    std::wstring cell(int row, int col) const;
    std::wstring selectedRowText() const;
    void copyAll();
    void paint(Gfx& g) override;
    void onMouseMove(int x, int y) override;
    void onWheel(int delta) override;
    void onKey(int vk, bool down) override;
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;

private:
    std::vector<std::wstring>              headers_;
    std::vector<std::vector<std::wstring>> rows_;
    std::vector<int>                       widths_;
    std::vector<Ic>                        rowIcons_;
    int scrollY_ = 0, scrollX_ = 0;
    int hoverRow_ = -1, selected_ = -1;
    int headH_ = 0, rowH_ = 0;
    bool dragging_ = false;
    int  dragStartY_ = 0, dragStartScroll_ = 0;

    int  headHeight() const;
    int  rowHeight() const;
    int  totalWidth() const;
    int  maxScrollY() const;
    int  maxScrollX() const;
    void ensureVisible(int row);
    void scrollBy(int dy);
    void hitTest(int x, int y, int& row);
};

class LogView : public Widget {
public:
    enum class Kind { Info, Warn, Error, Ok };
    void append(const std::wstring& line, Kind kind = Kind::Info);
    void clear();
    int  lineCount() const noexcept { return static_cast<int>(lines_.size()); }
    void paint(Gfx& g) override;
    void onWheel(int delta) override;
    void onKey(int vk, bool down) override;
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;

private:
    std::vector<std::pair<Kind, std::wstring>> lines_;
    int  scrollY_ = 0;
    bool autoScroll_ = true;
    int  lineHeight() const;
    int  maxScroll() const;
};

class Progress : public Widget {
public:
    double value = 0.0;          // 0..1
    bool   indeterminate = false;
    int    phase = 0;            // advanced by the app timer
    void paint(Gfx& g) override;
};

class ToastView : public Widget {
public:
    std::wstring text;
    int  kind = 0;               // 0 info, 1 warn, 2 error, 3 ok
    void paint(Gfx& g) override;
};

// Explorer-style address bar: clickable breadcrumb segments.
class Breadcrumb : public Widget {
public:
    struct Segment { std::wstring text; int dir; };  // -1 = package root
    std::vector<Segment> segments;
    std::function<void(int dir)> picked;
    void setSegments(std::vector<Segment> s);
    void paint(Gfx& g) override;
    void onMouseMove(int x, int y) override;
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;
private:
    std::vector<RECT> rects_;
    int hover_ = -1;
    int hit(int x) const;
};

// Lightweight owner-drawn context menu (right click on grids / trees).
class Menu : public Widget {
public:
    struct Item {
        std::wstring text;
        Ic           icon = Ic::None;
        bool         enabled = true;
        bool         separator = false;
    };
    void show(HWND owner, POINT screen, std::vector<Item> items,
              std::function<void(int)> picked);
    void close();
    void paint(Gfx& g) override;
    void onMouseMove(int x, int y) override;
    void onKey(int vk, bool down) override;
    LRESULT message(UINT msg, WPARAM wp, LPARAM lp, bool& handled) override;

private:
    int rowAt(int y) const;
    std::vector<Item> items_;
    std::function<void(int)> picked_;
    int hover_ = -1;
    int itemH_ = 0;
    int sepH_ = 0;
    int pad_ = 0;
};

class NavItem : public Widget {
public:
    Ic   icon = Ic::Home;
    std::wstring text;
    bool selected = false;
    int  index = 0;
    std::function<void(int)> activated;
    void paint(Gfx& g) override;
    void onClick() override;
};

// ---------------------------------------------------------------------------
// Entry point. Returns the process exit code. Pass the file to open (if any).
// ---------------------------------------------------------------------------
int run(HINSTANCE instance, const std::wstring& initialFile);

// GUI mode preparation: redirect the CRT streams to NUL so the existing CLI
// code (util::outLine etc.) stays harmless without a console, and install the
// log sink that forwards every line to the window.
void prepareGuiProcess();
void setLogTarget(HWND hwnd);
void setClipboardText(HWND owner, const std::wstring& text);

}  // namespace ui
