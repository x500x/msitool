#include "ui.h"

#include "msidb.h"
#include "extract.h"
#include "pack.h"

#include <objbase.h>
#include <shobjidl_core.h>
#include <oleidl.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cwctype>
#include <thread>
#include <unordered_map>

namespace ui {
namespace {

// Project metadata for 设置 → 关于. Keep in sync with README / CHANGELOG.
constexpr wchar_t kAppVersion[] = L"1.0.0";
constexpr wchar_t kAppLicense[] = L"MIT License";
constexpr wchar_t kAppHome[]    = L"https://github.com/x500x/msitool";

// Build stamp for the About card: compiler, architecture and build date.
std::wstring buildStamp() {
    std::wstring date = util::widen(__DATE__);
    return util::fmt(L"%s · MSVC %d.%d · %s", date.c_str(),
                     _MSC_VER / 100, _MSC_VER % 100,
                     sizeof(void*) == 8 ? L"x64" : L"x86");
}

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
RECT R(int x, int y, int w, int h) { return RECT{x, y, x + w, y + h}; }
int W(const RECT& r) { return r.right - r.left; }
int H(const RECT& r) { return r.bottom - r.top; }
RECT shrink(const RECT& r, int x, int y) {
    return RECT{r.left + x, r.top + y, r.right - x, r.bottom - y};
}

// Explorer-style "Type" column: a friendly description for common kinds.
std::wstring fileKindText(const std::wstring& name) {
    std::wstring ext = util::toLower(util::path::ext(name));
    if (ext == L".txt" || ext == L".md" || ext == L".log" || ext == L".rtf")
        return L"文本文档";
    if (ext == L".exe" || ext == L".dll" || ext == L".sys" || ext == L".drv")
        return L"应用程序";
    if (ext == L".ini" || ext == L".cfg" || ext == L".conf" ||
        ext == L".xml" || ext == L".json" || ext == L".yaml")
        return L"配置文件";
    if (ext == L".png" || ext == L".jpg" || ext == L".jpeg" ||
        ext == L".bmp" || ext == L".gif" || ext == L".ico" || ext == L".svg")
        return L"图像文件";
    if (ext == L".cab" || ext == L".msi" || ext == L".zip" || ext == L".7z")
        return L"压缩文件";
    if (ext == L".mst" || ext == L".msp")
        return L"安装程序包";
    if (ext.empty()) return L"文件";
    return ext.substr(1) + L" 文件";
}

std::wstring humanSize(unsigned long long bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024ULL * 1024)
        return util::fmt(L"%.1f KiB", static_cast<double>(bytes) / 1024.0);
    if (bytes < 1024ULL * 1024 * 1024)
        return util::fmt(L"%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return util::fmt(L"%.2f GiB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
}

unsigned long long parseSize(const std::wstring& text) {
    std::wstring s = util::trim(text);
    if (s.empty()) return 0;
    if (s.size() >= 2 && (s.back() == L'B' || s.back() == L'b')) {
        s.pop_back();
        if (!s.empty() && (s.back() == L'i' || s.back() == L'I')) s.pop_back();
    }
    unsigned long long mult = 1024ULL * 1024ULL;
    if (!s.empty()) {
        wchar_t last = s.back();
        if (last == L'K' || last == L'k') { mult = 1024ULL; s.pop_back(); }
        else if (last == L'M' || last == L'm') { mult = 1024ULL * 1024ULL; s.pop_back(); }
        else if (last == L'G' || last == L'g') { mult = 1024ULL * 1024ULL * 1024ULL; s.pop_back(); }
    }
    if (s.empty() || !iswdigit(static_cast<wint_t>(s.front()))) return 0;
    return _wcstoui64(s.c_str(), nullptr, 10) * mult;
}

std::vector<std::wstring> splitList(const std::wstring& text) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L';' || c == L'\n') {
            std::wstring v = util::trim(cur);
            if (!v.empty()) out.push_back(v);
            cur.clear();
        } else cur += c;
    }
    std::wstring v = util::trim(cur);
    if (!v.empty()) out.push_back(v);
    return out;
}

LogView::Kind classifyLog(bool isError, const std::wstring& line) {
    if (util::startsWith(line, L"[warn]")) return LogView::Kind::Warn;
    if (util::startsWith(line, L"[v] ")) return LogView::Kind::Info;
    if (isError || util::startsWith(line, L"error") || util::startsWith(line, L"FAIL"))
        return LogView::Kind::Error;
    if (line.find(L"Done") != std::wstring::npos ||
        line.find(L"完成") != std::wstring::npos ||
        line.find(L"VALID") != std::wstring::npos ||
        line.find(L"OK") != std::wstring::npos)
        return LogView::Kind::Ok;
    return LogView::Kind::Info;
}

// ---- settings in HKCU\Software\msitool -------------------------------------
const wchar_t* kSettingsKey = L"Software\\msitool";

std::wstring loadSetting(const wchar_t* name) {
    wchar_t buf[2048]{};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name,
                     RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
        return buf;
    return L"";
}

void saveSetting(const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

int loadIntSetting(const wchar_t* name, int fallback) {
    DWORD v = static_cast<DWORD>(fallback), size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, name,
                     RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS)
        return static_cast<int>(v);
    return fallback;
}

void saveIntSetting(const wchar_t* name, int value) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0,
                        KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    DWORD v = static_cast<DWORD>(value);
    RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    RegCloseKey(key);
}

// ---- modern IFileDialog based pickers --------------------------------------
enum class Pick { Open, Folder, Save };

bool pickPath(HWND owner, Pick kind, const wchar_t* title,
              const COMDLG_FILTERSPEC* filters, UINT count,
              const wchar_t* defaultExt, std::wstring& out,
              const std::wstring& startDir = L"") {
    IFileDialog* dlg = nullptr;
    CLSID clsid = kind == Pick::Save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    if (FAILED(CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    opts |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR;
    if (kind == Pick::Folder) opts |= FOS_PICKFOLDERS;
    else if (kind == Pick::Open) opts |= FOS_FILEMUSTEXIST;
    else opts |= FOS_OVERWRITEPROMPT;
    dlg->SetOptions(opts);
    if (title) dlg->SetTitle(title);
    if (filters && count) dlg->SetFileTypes(count, filters);
    // Open the picker at the folder the user is currently extracting to.
    if (!startDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr,
                                                  IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    if (kind == Pick::Save && defaultExt) {
        IFileSaveDialog* save = nullptr;
        if (SUCCEEDED(dlg->QueryInterface(IID_PPV_ARGS(&save)))) {
            save->SetDefaultExtension(defaultExt);
            save->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            wchar_t* path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                out = path;
                CoTaskMemFree(path);
                ok = true;
            }
            item->Release();
        }
    }
    dlg->Release();
    return ok;
}

// Multi-select variant of Pick::Open: used by 打包 "only these files".
bool pickPaths(HWND owner, const wchar_t* title, const COMDLG_FILTERSPEC* filters,
               UINT count, std::vector<std::wstring>& outs,
               const std::wstring& startDir = L"") {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                               IID_PPV_ARGS(&dlg))))
        return false;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST |
                    FOS_NOCHANGEDIR | FOS_ALLOWMULTISELECT);
    if (title) dlg->SetTitle(title);
    if (filters && count) dlg->SetFileTypes(count, filters);
    if (!startDir.empty()) {
        IShellItem* folder = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(startDir.c_str(), nullptr,
                                                 IID_PPV_ARGS(&folder)))) {
            dlg->SetFolder(folder);
            folder->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(dlg->Show(owner))) {
        IShellItemArray* items = nullptr;
        if (SUCCEEDED(dlg->GetResults(&items))) {
            DWORD n = 0;
            items->GetCount(&n);
            for (DWORD i = 0; i < n; ++i) {
                IShellItem* item = nullptr;
                if (FAILED(items->GetItemAt(i, &item))) continue;
                wchar_t* path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                    outs.emplace_back(path);
                    CoTaskMemFree(path);
                    ok = true;
                }
                item->Release();
            }
            items->Release();
        }
    }
    dlg->Release();
    return ok;
}

// ---- DWM (loaded dynamically: no extra import library required) -----------
void setDwmAttribute(HWND hwnd, int attribute, int value) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (!dwm) return;
    using Fn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    auto fn = reinterpret_cast<Fn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (fn) {
        int v = value;
        fn(hwnd, static_cast<DWORD>(attribute), &v, sizeof(v));
    }
    FreeLibrary(dwm);
}

constexpr UINT kTimer = 1;
constexpr int kIdOpen = 40001;
constexpr int kIdRefresh = 40002;

enum Page {
    PgHome = 0, PgInfo, PgTables, PgQuery, PgExtract,
    PgExport, PgPack, PgVerify, PgSettings, PgCount
};

const wchar_t* kPageTitle[PgCount] = {
    L"概览", L"包信息", L"数据表", L"SQL 查询", L"解包",
    L"导出", L"打包", L"验证", L"设置"
};
const wchar_t* kPageSubtitle[PgCount] = {
    L"查看、解包和构建 Windows Installer 包",
    L"摘要信息、关键属性与 CAB 媒体",
    L"浏览数据库中的每一张表",
    L"用 MSI SQL 直接查询包内容",
    L"把包里的文件按目录树还原出来",
    L"导出 CAB、表、流和二进制资源",
    L"把一个目录打包成自包含的 MSI",
    L"检查文件是否为合法的安装包",
    L"外观、关于与快捷键"
};
const Ic kPageIcon[PgCount] = {
    Ic::Home, Ic::Info, Ic::Table, Ic::Query, Ic::Extract,
    Ic::Export, Ic::Pack, Ic::Shield, Ic::Settings
};

// ---------------------------------------------------------------------------
// Drag & drop
//
// A drop is routed by *position first, content second*: every page publishes
// the fields that accept a drop, the field under the cursor wins, and every
// other spot falls back to the payload itself. That fallback keeps the two
// workflows strictly apart: a folder is always pack input (源目录, switching to
// the pack page if needed) and an .msi / .msp is always read input (opened on
// the read side). The pack page never opens a package, so the pack form can
// never show the state of the read side.
// ---------------------------------------------------------------------------
enum DropKind {
    DkNone = 0,
    DkSourceDir,    // pack: source directory
    DkOutputMsi,    // pack: output .msi path
    DkManifest,     // pack: pack.ini manifest
    DkCert,         // pack: .pfx signing certificate
    DkOutDir,       // extract / export: destination directory
    DkPackage,      // open an .msi / .msp (not used as a fallback any more)
    DkAuto          // page background: route by content
};

struct DropZone {
    RECT           hit{};      // inflated area that selects this zone
    RECT           core{};     // exact field rect, used to resolve overlaps
    RECT           frame{};    // card outlined by the window (may be empty)
    DropKind       kind = DkNone;
    std::wstring   name;       // field name used in the hint
    TextBox*       glow = nullptr;   // field that lights up (nullptr = frame)
};

RECT grow(const RECT& r, int x, int y) {
    return RECT{r.left - x, r.top - y, r.right + x, r.bottom + y};
}

// Squared distance from a point to a rect, 0 when the point is inside it.
int distToRect(const RECT& r, POINT p) {
    int l = static_cast<int>(r.left), t = static_cast<int>(r.top);
    int rr = static_cast<int>(r.right) - 1, b = static_cast<int>(r.bottom) - 1;
    int x = static_cast<int>(p.x), y = static_cast<int>(p.y);
    int dx = std::max(0, std::max(l - x, x - rr));
    int dy = std::max(0, std::max(t - y, y - b));
    return dx * dx + dy * dy;
}

bool extIs(const std::wstring& path, std::initializer_list<const wchar_t*> list) {
    std::wstring e = util::toLower(util::path::ext(path));
    if (e.empty()) return false;
    for (const wchar_t* s : list) if (e == s) return true;
    return false;
}

// True when `path` is `root` itself or lives inside it.
bool pathUnder(const std::wstring& path, const std::wstring& rootIn) {
    if (rootIn.empty() || path.size() < rootIn.size()) return false;
    std::wstring root = rootIn;
    bool sep = root.back() == L'\\' || root.back() == L'/';
    if (!sep) {
        while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/'))
            root.pop_back();
    }
    if (path.size() < root.size()) return false;
    if (!util::iequals(path.substr(0, root.size()), root)) return false;
    if (path.size() == root.size() || sep) return true;
    wchar_t c = path[root.size()];
    return c == L'\\' || c == L'/';
}

// The directory a dropped item stands for: the folder itself, or the folder
// that holds the file.
std::wstring itemDir(const std::wstring& p) {
    std::wstring full = util::path::full(p);
    if (util::path::isDirectory(full)) return full;
    return util::path::parent(full);
}

// Deepest directory that contains every dropped item. Empty when the items
// live on different drives, i.e. when no single source directory exists.
std::wstring commonRoot(const std::vector<std::wstring>& ps) {
    if (ps.empty()) return L"";
    std::wstring cur = itemDir(ps[0]);
    for (size_t i = 1; i < ps.size() && !cur.empty(); ++i) {
        std::wstring other = itemDir(ps[i]);
        while (!cur.empty() && !pathUnder(other, cur)) {
            std::wstring up = util::path::parent(cur);
            if (up == cur) { cur.clear(); break; }
            cur = up;
        }
    }
    return cur;
}

// CF_HDROP of a drag / drop as a path list. Empty when the payload carries no
// files (plain text, images, shell "shell items" only), which the caller
// turns into DROPEFFECT_NONE so the cursor shows "not allowed".
std::vector<std::wstring> dropPaths(IDataObject* obj) {
    std::vector<std::wstring> out;
    if (!obj) return out;
    FORMATETC fe{};
    fe.cfFormat = CF_HDROP;
    fe.ptd = nullptr;
    fe.dwAspect = DVASPECT_CONTENT;
    fe.lindex = -1;
    fe.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (FAILED(obj->GetData(&fe, &med))) return out;
    if (med.tymed == TYMED_HGLOBAL && med.hGlobal) {
        HDROP drop = static_cast<HDROP>(GlobalLock(med.hGlobal));
        if (drop) {
            UINT n = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
            for (UINT i = 0; i < n; ++i) {
                UINT need = DragQueryFileW(drop, i, nullptr, 0);
                if (!need) continue;
                std::vector<wchar_t> buf(static_cast<size_t>(need) + 2, L'\0');
                if (DragQueryFileW(drop, i, buf.data(), need + 1))
                    out.emplace_back(buf.data());
            }
            GlobalUnlock(med.hGlobal);
        }
    }
    ReleaseStgMedium(&med);
    return out;
}

// ---------------------------------------------------------------------------
// Application window
// ---------------------------------------------------------------------------
class AppDropTarget;         // defined below, OLE entry point for drag & drop

class App {
public:
    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

    HWND hwnd_ = nullptr;
    HINSTANCE hinst_ = nullptr;
    HACCEL accel_ = nullptr;
    std::vector<std::unique_ptr<Widget>> widgets_;
    int nextId_ = 1000;
    int page_ = PgHome;
    bool running_ = false;
    bool verifyOk_ = false;
    int themeMode_ = 2;              // 0 light, 1 dark, 2 follow system
    std::wstring package_, initialFile_;
    std::unique_ptr<msi::Db> db_;
    std::vector<std::wstring> tableNames_, filteredTables_;
    std::wstring status_ = L"就绪 · 打开一个 .msi / .msp 文件开始";
    std::wstring statusRight_;
    std::wstring tableInfo_, queryInfo_, verifyText_, homeSummary_;
    std::wstring toastText_;
    int toastKind_ = 0;              // 0 info, 1 warn, 2 error, 3 ok
    bool toastVisible_ = false;
    bool toastSticky_ = false;       // held open while a file drag is active
    ULONGLONG toastUntil_ = 0;
    std::wstring lastOutputDir_, lastPackOutput_;

    struct Card {
        RECT r; std::wstring title, subtitle; Ic icon; bool hero;
    };
    struct Label {
        RECT r; std::wstring text; HFONT font; COLORREF color; UINT fmt;
    };
    struct Bar {
        RECT r; std::wstring text; int kind; Ic icon;
    };
    std::vector<Card> cards_;
    std::vector<Label> labels_;
    std::vector<Bar> bars_;

    // shared widgets -------------------------------------------------------
    Button* btnTheme_ = nullptr;
    Button* btnMin_ = nullptr;
    Button* btnMax_ = nullptr;
    Button* btnClose_ = nullptr;
    NavItem* nav_[PgCount] = {};
    TextBox* tbPackage_ = nullptr;
    Button* btnBrowse_ = nullptr;
    Button* btnOpen_ = nullptr;
    LogView* log_ = nullptr;
    Button* btnClearLog_ = nullptr;
    Button* btnOpenFolder_ = nullptr;
    Progress* progress_ = nullptr;

    // home -----------------------------------------------------------------
    DataGrid* gridHome_ = nullptr;
    Button* btnHomeVerify_ = nullptr;
    Button* btnHomeExtract_ = nullptr;
    Button* btnHomeExport_ = nullptr;
    // info -----------------------------------------------------------------
    int      infoTab_ = 0;             // 0 summary, 1 properties, 2 media
    Button*  btnInfoSummary_ = nullptr;
    Button*  btnInfoProps_ = nullptr;
    Button*  btnInfoMedia_ = nullptr;
    TextBox* tbInfoSearch_ = nullptr;
    Button*  btnInfoCopy_ = nullptr;
    DataGrid* gridInfo_ = nullptr;
    RECT     infoDetailRect_{};
    std::wstring infoTitle_, infoSubtitle_, infoCount_, infoDetail_;
    std::wstring infoStats_[4];
    std::vector<std::wstring>              infoHeaders_[3];
    std::vector<std::vector<std::wstring>> infoRows_[3];
    std::vector<std::wstring>              infoDetails_[3];
    std::vector<std::wstring>              infoShownDetails_;
    // tables ---------------------------------------------------------------
    TextBox* tbTableSearch_ = nullptr;
    DataGrid* gridTables_ = nullptr;
    DataGrid* gridTableData_ = nullptr;
    Button* btnTableCopy_ = nullptr;
    // query ----------------------------------------------------------------
    TextBox* tbSql_ = nullptr;
    DropDown* ddExamples_ = nullptr;
    Button* btnQueryRun_ = nullptr;
    Button* btnQueryCopy_ = nullptr;
    Button* btnQueryClear_ = nullptr;
    DataGrid* gridQueryResult_ = nullptr;
    // extract --------------------------------------------------------------
    bool     extractAdv_ = false;       // collapsible options panel
    bool     extractLog_ = false;       // collapsible log drawer
    Button*  btnExtractAdv_ = nullptr;
    Button*  btnExtractLog_ = nullptr;
    Button*  btnExtractAll_ = nullptr;
    Button*  btnExtractSel_ = nullptr;
    Button*  btnExtractOpen_ = nullptr;
    Button*  btnNavBack_ = nullptr;
    Button*  btnNavFwd_ = nullptr;
    Button*  btnNavUp_ = nullptr;
    Button*  btnNavRefresh_ = nullptr;
    Button*  btnRecentDirs_ = nullptr;
    Breadcrumb* crumb_ = nullptr;
    std::vector<std::wstring> recentOutDirs_;
    TextBox* tbOutDir_ = nullptr;
    Button*  btnOutBrowse_ = nullptr;
    Check*   chkFlat_ = nullptr;
    Check*   chkDry_ = nullptr;
    Check*   chkSkip_ = nullptr;
    Check*   chkSource_ = nullptr;
    Check*   chkKeepTemp_ = nullptr;
    Check*   chkVerbose_ = nullptr;
    TextBox* tbTransforms_ = nullptr;
    Button*  btnTransBrowse_ = nullptr;
    TextBox* tbBaseMsi_ = nullptr;
    Button*  btnBaseBrowse_ = nullptr;

    // package file browser (Explorer-like tree + list)
    struct BDir {
        std::wstring key, name, parentKey, targetRel;
        std::vector<int> children;
        int parent = -1;
        int depth = 0;
    };
    struct BFile {
        std::wstring key, name, component, relPath;
        unsigned long long size = 0;
        int sequence = 0;
        int dir = -1;
    };
    TextBox*  tbBrowseFilter_ = nullptr;
    DataGrid* gridBrowseDirs_ = nullptr;
    DataGrid* gridBrowseFiles_ = nullptr;
    std::unique_ptr<Menu> menu_;
    std::vector<BDir>  bDirs_;
    std::vector<BFile> bFiles_;
    std::vector<int>   bRoots_;
    std::vector<int>   bVisible_;     // expanded tree, flattened
    std::vector<std::wstring> bExpanded_;
    // Right-hand Explorer list: (isDirectory, index into bDirs_/bFiles_).
    std::vector<std::pair<bool, int>> bRows_;
    int                bSelDir_ = -1;   // -1 = package root
    std::wstring       bSelDirKey_;
    std::wstring       browseStatus_, autoOutDir_;
    bool               suppressTreeSel_ = false;
    std::vector<int>   navHistory_;      // visited directory indices
    int                navPos_ = -1;
    int                sortCol_ = -1;   // -1 = folders first, then name
    bool               sortAsc_ = true;
    // export ---------------------------------------------------------------
    DropDown* ddExportMode_ = nullptr;
    TextBox* tbExportDir_ = nullptr;
    Button* btnExportBrowse_ = nullptr;
    Button* btnExport_ = nullptr;
    // pack -----------------------------------------------------------------
    int    packTab_ = 0;              // 0 basic, 1 advanced, 2 signing
    Button* btnTabBasic_ = nullptr;
    Button* btnTabAdvanced_ = nullptr;
    Button* btnTabSign_ = nullptr;
    TextBox* tbSrcDir_ = nullptr;      Button* btnSrcBrowse_ = nullptr;
    TextBox* tbOutMsi_ = nullptr;      Button* btnOutMsiBrowse_ = nullptr;
    TextBox* tbName_ = nullptr;
    TextBox* tbVersion_ = nullptr;
    TextBox* tbManufacturer_ = nullptr;
    DropDown* ddPlatform_ = nullptr;
    TextBox* tbLanguage_ = nullptr;
    DropDown* ddUi_ = nullptr;
    TextBox* tbCabSize_ = nullptr;
    TextBox* tbConfig_ = nullptr;      Button* btnConfigBrowse_ = nullptr;
    DropDown* ddLargeFiles_ = nullptr;
    TextBox* tbSignCert_ = nullptr;    Button* btnSignCertBrowse_ = nullptr;
    TextBox* tbSignPassword_ = nullptr;
    TextBox* tbSignTime_ = nullptr;
    Check* chkForce_ = nullptr;
    Check* chkPackDry_ = nullptr;
    Check* chkPackVerbose_ = nullptr;
    Check* chkEmptyDirs_ = nullptr;
    Button* btnPack_ = nullptr;
    // drag & drop -----------------------------------------------------------
    // The pack form is split over three tabs, so the drop targets are
    // collected from the currently visible fields plus one window-wide
    // fallback; the outline of the form card is drawn by the window itself.
    IDropTarget* dropTarget_ = nullptr;
    std::vector<DropZone> zones_;
    std::vector<std::wstring> dragPaths_;
    int  zoneHit_ = -1;
    bool dragging_ = false;
    RECT packForm_{};
    // verify ---------------------------------------------------------------
    DataGrid* gridVerify_ = nullptr;
    Button* btnVerify_ = nullptr;
    // settings -------------------------------------------------------------
    Button* btnLight_ = nullptr;
    Button* btnDark_ = nullptr;
    Button* btnSystem_ = nullptr;
    Link*   linkHome_ = nullptr;
    // A separate always-on-top popup window (like the context menu), so no
    // sibling widget, card or layout repaint can ever cover the notification.
    std::unique_ptr<ToastView> toastView_;
    RECT toastClientRect_{};          // client-space rect, for repaint-on-hide

    // ---- creation --------------------------------------------------------
    template <class T> T* add(int page) {
        auto p = std::make_unique<T>();
        T* raw = p.get();
        raw->setPage(page);
        raw->create(hwnd_, ++nextId_, R(0, 0, 1, 1));
        widgets_.push_back(std::move(p));
        return raw;
    }

    bool create(HINSTANCE inst, const std::wstring& initialFile) {
        hinst_ = inst;
        initialFile_ = initialFile;
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = App::proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = L"msitool_main_v1";
        if (!RegisterClassExW(&wc)) return false;

        int w = theme().s(1360), h = theme().s(920);
        RECT work{};
        if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0)) {
            int maxW = (work.right - work.left) - theme().s(80);
            int maxH = (work.bottom - work.top) - theme().s(80);
            if (maxW > 0 && w > maxW) w = maxW;
            if (maxH > 0 && h > maxH) h = maxH;
        }
        hwnd_ = CreateWindowExW(0, wc.lpszClassName, L"msitool",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                                CW_USEDEFAULT, CW_USEDEFAULT, w, h,
                                nullptr, nullptr, inst, this);
        return hwnd_ != nullptr;
    }

    void onCreate() {
        themeMode_ = loadIntSetting(L"ThemeMode", 2);
        applyTheme();
        buildWidgets();
        ACCEL acc[] = {
            {FCONTROL | FVIRTKEY, L'O', kIdOpen},
            {FVIRTKEY, VK_F5, kIdRefresh},
        };
        accel_ = CreateAcceleratorTableW(acc, 2);
        SetTimer(hwnd_, kTimer, 40, nullptr);
        DragAcceptFiles(hwnd_, TRUE);
        installDropTarget();
        setDwmAttribute(hwnd_, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, 2 /*ROUND*/);
        setLogTarget(hwnd_);
        loadRecentOutDirs();
        std::wstring last = loadSetting(L"LastPackage");
        if (!initialFile_.empty()) loadPackage(initialFile_);
        else if (!last.empty() && util::path::exists(last)) loadPackage(last);
        else {
            tbPackage_->setValue(L"");
            refreshEmptyState();
        }
        switchPage(PgHome);
        // Don't let the first EDIT grab the initial focus: the cue banner
        // should be visible and no control should look "active" on launch.
        SetFocus(hwnd_);
        // Force one more non-client calculation before the window becomes
        // visible. Without this, DWM can draw the stock caption/frame from
        // the initial WM_NCCALCSIZE pass until a minimize/restore occurs.
        SetWindowPos(hwnd_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                     SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    // ---- theme -----------------------------------------------------------
    void applyTheme() {
        bool dark = themeMode_ == 1 || (themeMode_ == 2 && systemPrefersDark());
        theme().rebuild(hwnd_, dark);
        setDwmAttribute(hwnd_, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, dark ? 1 : 0);
        for (auto& w : widgets_) { w->onThemeChanged(); w->invalidate(); }
        if (toastView_) { toastView_->onThemeChanged(); toastView_->invalidate(); }
        if (btnTheme_) {
            btnTheme_->icon = dark ? Ic::Sun : Ic::Moon;
            btnTheme_->invalidate();
        }
        if (btnLight_) {
            btnLight_->accent = themeMode_ == 0;
            btnDark_->accent = themeMode_ == 1;
            btnSystem_->accent = themeMode_ == 2;
            btnLight_->invalidate(); btnDark_->invalidate(); btnSystem_->invalidate();
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void setThemeMode(int mode) {
        themeMode_ = mode;
        saveIntSetting(L"ThemeMode", mode);
        applyTheme();
        layout();
    }

    // ---- widget construction ---------------------------------------------
    Button* makeButton(int page, const wchar_t* text, Ic icon,
                       bool accent, const std::function<void()>& fn) {
        Button* b = add<Button>(page);
        b->text = text;
        b->icon = icon;
        b->accent = accent;
        b->clicked = fn;
        return b;
    }

    // TextBox needs its style flags set before Widget::create() runs, because
    // create() calls afterCreate() which builds the inner EDIT control.
    TextBox* makeText(int page, const wchar_t* cue, bool multi = false,
                      bool readOnly = false, bool password = false) {
        auto p = std::make_unique<TextBox>();
        TextBox* t = p.get();
        t->setPage(page);
        t->placeholder = cue;
        t->multiline = multi;
        t->readOnly = readOnly;
        t->password = password;
        t->create(hwnd_, ++nextId_, R(0, 0, 1, 1));
        widgets_.push_back(std::move(p));
        return t;
    }

    void buildWidgets() {
        // caption bar ------------------------------------------------------
        btnTheme_ = add<Button>(-1);
        btnTheme_->backdrop = Backdrop::Nav;
        btnTheme_->caption = true;
        btnTheme_->glyphSize = 16;
        btnTheme_->clicked = [this] {
            setThemeMode(theme().dark ? 0 : 1);
            toast(theme().dark ? L"已切换到深色主题" : L"已切换到浅色主题", 0);
        };
        btnMin_ = add<Button>(-1);
        btnMin_->backdrop = Backdrop::Nav;
        btnMin_->caption = true; btnMin_->icon = Ic::Min; btnMin_->glyphSize = 12;
        btnMin_->clicked = [this] { ShowWindow(hwnd_, SW_MINIMIZE); };
        btnMax_ = add<Button>(-1);
        btnMax_->backdrop = Backdrop::Nav;
        btnMax_->caption = true; btnMax_->icon = Ic::Max; btnMax_->glyphSize = 12;
        btnMax_->clicked = [this] { toggleMaximize(); };
        btnClose_ = add<Button>(-1);
        btnClose_->backdrop = Backdrop::Nav;
        btnClose_->caption = true; btnClose_->captionClose = true;
        btnClose_->icon = Ic::Close; btnClose_->glyphSize = 12;
        btnClose_->clicked = [this] { PostMessageW(hwnd_, WM_CLOSE, 0, 0); };

        // navigation -------------------------------------------------------
        for (int i = 0; i < PgCount; ++i) {
            nav_[i] = add<NavItem>(-1);
            nav_[i]->backdrop = Backdrop::Nav;
            nav_[i]->text = kPageTitle[i];
            nav_[i]->icon = kPageIcon[i];
            nav_[i]->index = i;
            nav_[i]->activated = [this](int p) { switchPage(p); };
        }

        // file bar (page -2: visible everywhere but Settings) ---------------
        tbPackage_ = makeText(-2, L"选择或拖入 .msi / .msp 文件");
        tbPackage_->enter = [this] {
            std::wstring p = tbPackage_->value();
            if (!p.empty()) loadPackage(p);
        };
        btnBrowse_ = makeButton(-2, L"浏览", Ic::Folder, false,
                                [this] { browsePackage(); });
        btnOpen_ = makeButton(-2, L"打开", Ic::Open, true,
                              [this] {
                                  std::wstring p = tbPackage_->value();
                                  if (p.empty()) browsePackage();
                                  else loadPackage(p);
                              });

        // shared task log ---------------------------------------------------
        log_ = add<LogView>(-3);
        btnClearLog_ = makeButton(-3, L"清空", Ic::Copy, false,
                                  [this] { log_->clear(); });
        btnOpenFolder_ = makeButton(-3, L"打开目录", Ic::Folder, false,
                                    [this] { openOutputFolder(); });
        // On the extract page these live in the collapsible log drawer.
        log_->setTab(2);
        btnClearLog_->setTab(2);
        btnOpenFolder_->setTab(2);
        progress_ = add<Progress>(-1);
        progress_->backdrop = Backdrop::Nav;

        // ---- Home ---------------------------------------------------------
        gridHome_ = add<DataGrid>(PgHome);
        btnHomeVerify_ = makeButton(PgHome, L"验证这个包", Ic::Shield, true,
                                    [this] { switchPage(PgVerify); runVerify(); });
        btnHomeExtract_ = makeButton(PgHome, L"解包到目录", Ic::Extract, false,
                                     [this] { switchPage(PgExtract); });
        btnHomeExport_ = makeButton(PgHome, L"导出归档", Ic::Export, false,
                                    [this] { switchPage(PgExport); });

        // ---- Info ---------------------------------------------------------
        // One full-width grid behind a tab strip + search, instead of three
        // cramped tables: all the data stays readable at any window size.
        btnInfoSummary_ = makeButton(PgInfo, L"摘要", Ic::Info, false,
                                     [this] { setInfoTab(0); });
        btnInfoProps_ = makeButton(PgInfo, L"属性", Ic::Table, false,
                                   [this] { setInfoTab(1); });
        btnInfoMedia_ = makeButton(PgInfo, L"媒体 / CAB", Ic::Database, false,
                                   [this] { setInfoTab(2); });
        btnInfoSummary_->accent = true;
        tbInfoSearch_ = makeText(PgInfo, L"筛选当前表格…");
        tbInfoSearch_->changed = [this] { applyInfoFilter(); };
        btnInfoCopy_ = makeButton(PgInfo, L"复制", Ic::Copy, false, [this] {
            gridInfo_->copyAll();
            toast(L"已复制当前表格为 TSV", 3);
        });
        gridInfo_ = add<DataGrid>(PgInfo);
        gridInfo_->rowHeightPx = S(28);   // denser: more rows visible at once
        gridInfo_->headHeightPx = S(32);
        gridInfo_->selectionChanged = [this](int row) { updateInfoDetail(row); };

        // ---- Tables -------------------------------------------------------
        tbTableSearch_ = makeText(PgTables, L"筛选表名…");
        tbTableSearch_->changed = [this] { filterTables(); };
        gridTables_ = add<DataGrid>(PgTables);
        gridTables_->headerVisible = false;
        gridTables_->selectionChanged = [this](int row) { showTableAt(row); };
        gridTableData_ = add<DataGrid>(PgTables);
        btnTableCopy_ = makeButton(PgTables, L"复制", Ic::Copy, false,
                                   [this] {
                                       gridTableData_->copyAll();
                                       toast(L"表内容已复制为 TSV", 3);
                                   });

        // ---- Query --------------------------------------------------------
        tbSql_ = makeText(PgQuery, L"SELECT * FROM `Property`", true);
        tbSql_->enter = [this] { runQuery(); };
        ddExamples_ = add<DropDown>(PgQuery);
        ddExamples_->setItems({
            L"示例查询…",
            L"SELECT * FROM `Property`",
            L"SELECT `File`,`FileName`,`FileSize` FROM `File`",
            L"SELECT * FROM `Component`",
            L"SELECT * FROM `Feature`",
            L"SELECT * FROM `Media`",
            L"SELECT * FROM `CustomAction`",
            L"SELECT `Name`,`Data` FROM `_Streams`",
        });
        ddExamples_->selected = [this](int i) {
            if (i > 0) {
                tbSql_->setValue(ddExamples_->items[i]);
                runQuery();
            }
            ddExamples_->setIndex(0);
        };
        btnQueryRun_ = makeButton(PgQuery, L"运行", Ic::Play, true,
                                  [this] { runQuery(); });
        btnQueryCopy_ = makeButton(PgQuery, L"复制结果", Ic::Copy, false,
                                   [this] {
                                       gridQueryResult_->copyAll();
                                       toast(L"查询结果已复制为 TSV", 3);
                                   });
        btnQueryClear_ = makeButton(PgQuery, L"清空", Ic::Cross, false,
                                    [this] {
                                        tbSql_->setValue(L"");
                                        gridQueryResult_->clear();
                                        queryInfo_ = L"就绪";
                                        layout();
                                    });
        gridQueryResult_ = add<DataGrid>(PgQuery);

        // ---- Extract : Explorer-like browser -------------------------------
        // Command / address bar.
        btnNavBack_ = makeButton(PgExtract, L"", Ic::ArrowLeft, false,
                                 [this] { navBack(); });
        btnNavFwd_ = makeButton(PgExtract, L"", Ic::ArrowRight, false,
                                [this] { navForward(); });
        btnNavUp_ = makeButton(PgExtract, L"", Ic::ArrowUp, false,
                               [this] { navUp(); });
        btnNavRefresh_ = makeButton(PgExtract, L"", Ic::Refresh, false,
                                    [this] { rebuildBrowse(); });
        crumb_ = add<Breadcrumb>(PgExtract);
        crumb_->picked = [this](int dir) { openDir(dir, false); };
        tbBrowseFilter_ = makeText(PgExtract, L"搜索 当前目录");
        tbBrowseFilter_->changed = [this] { refreshBrowseList(); };
        btnExtractAll_ = makeButton(PgExtract, L"解包全部", Ic::Extract, true,
                                    [this] { startExtractAll(); });
        btnExtractSel_ = makeButton(PgExtract, L"解包选中", Ic::File, false,
                                    [this] { startExtractSelected(); });
        btnExtractAdv_ = makeButton(PgExtract, L"选项", Ic::Settings, false,
                                    [this] { setExtractAdvanced(!extractAdv_); });
        btnExtractLog_ = makeButton(PgExtract, L"日志", Ic::Terminal, false,
                                    [this] { setLogOpen(!extractLog_); });

        // Navigation tree (left pane).
        gridBrowseDirs_ = add<DataGrid>(PgExtract);
        gridBrowseDirs_->headerVisible = false;
        gridBrowseDirs_->rounded = false;
        gridBrowseDirs_->rowHeightPx = S(28);
        gridBrowseDirs_->headHeightPx = 0;
        gridBrowseDirs_->selectionChanged = [this](int row) {
            if (!suppressTreeSel_ && row >= 0 &&
                row < static_cast<int>(bVisible_.size()))
                openDir(bVisible_[row], true);
        };
        gridBrowseDirs_->rowActivated = [this](int row) { toggleBrowseDir(row); };
        gridBrowseDirs_->iconClicked = [this](int row) { toggleBrowseDir(row); };
        gridBrowseDirs_->goUp = [this] { navUp(); };
        gridBrowseDirs_->refreshRequested = [this] { rebuildBrowse(); };
        gridBrowseDirs_->itemContextMenu = [this](int row, POINT p) { showDirMenu(row, p); };

        // Details view (right pane).
        gridBrowseFiles_ = add<DataGrid>(PgExtract);
        gridBrowseFiles_->rounded = false;
        gridBrowseFiles_->rowHeightPx = S(28);
        gridBrowseFiles_->headHeightPx = S(32);
        gridBrowseFiles_->selectionChanged = [this](int row) { onBrowseRowSelected(row); };
        gridBrowseFiles_->rowActivated = [this](int row) { activateBrowseRow(row); };
        gridBrowseFiles_->headerClicked = [this](int col) { sortBy(col); };
        gridBrowseFiles_->goUp = [this] { navUp(); };
        gridBrowseFiles_->refreshRequested = [this] { rebuildBrowse(); };
        gridBrowseFiles_->itemContextMenu = [this](int row, POINT p) { showItemMenu(row, p); };

        // Always-visible "解包到" location bar. Choosing the destination is
        // a primary action here, so it must not be buried in a folded panel.
        tbOutDir_ = makeText(PgExtract, L"解包到（留空自动使用 <包名>_files）");
        tbOutDir_->changed = [this] { refreshBrowseList(); };
        btnOutBrowse_ = makeButton(PgExtract, L"浏览…", Ic::Folder, false,
                                   [this] { chooseExtractOutDir(); });
        btnRecentDirs_ = makeButton(PgExtract, L"最近", Ic::Clock, false,
                                    [this] { showRecentOutDirs(); });
        btnExtractOpen_ = makeButton(PgExtract, L"打开", Ic::Open, false,
                                     [this] {
                                         std::wstring d = util::trim(tbOutDir_->value());
                                         if (!d.empty())
                                             lastOutputDir_ = util::path::full(d);
                                         openOutputFolder();
                                     });

        // Collapsible options panel (tab 1): advanced switches only.
        chkFlat_ = makeCheck(PgExtract, L"平铺所有文件", false);
        chkDry_ = makeCheck(PgExtract, L"仅预览 (dry-run)", false);
        chkSkip_ = makeCheck(PgExtract, L"跳过已存在文件", false);
        chkSource_ = makeCheck(PgExtract, L"使用源映像目录树", false);
        chkSource_->changed = [this](bool) { if (page_ == PgExtract) rebuildBrowse(); };
        chkKeepTemp_ = makeCheck(PgExtract, L"保留临时 CAB", false);
        chkVerbose_ = makeCheck(PgExtract, L"详细日志", true);
        tbTransforms_ = makeText(PgExtract, L"可选：一个或多个 .mst，用 ; 分隔");
        btnTransBrowse_ = makeButton(PgExtract, L"浏览", Ic::Folder, false,
                                     [this] { browseFileFor(tbTransforms_, true,
                                         L"选择转换文件", L"MST 转换", L"*.mst"); });
        tbBaseMsi_ = makeText(PgExtract, L"可选：解包 .msp 时的目标 MSI");
        btnBaseBrowse_ = makeButton(PgExtract, L"浏览", Ic::Folder, false,
                                    [this] { browseFileFor(tbBaseMsi_, false,
                                        L"选择基准 MSI", L"MSI 包", L"*.msi"); });
        for (Widget* w : std::initializer_list<Widget*>{
                 chkFlat_, chkDry_, chkSkip_, chkSource_, chkKeepTemp_, chkVerbose_,
                 tbTransforms_, btnTransBrowse_, tbBaseMsi_, btnBaseBrowse_})
            w->setTab(1);

        // ---- Export -------------------------------------------------------
        ddExportMode_ = add<DropDown>(PgExport);
        ddExportMode_->setItems({L"仅导出 CAB + manifest.txt", L"完整归档（表 / 流 / 二进制 / 摘要）"});
        tbExportDir_ = makeText(PgExport, L"输出目录");
        btnExportBrowse_ = makeButton(PgExport, L"浏览", Ic::Folder, false,
                                      [this] { browseFolderFor(tbExportDir_, L"选择导出目录"); });
        btnExport_ = makeButton(PgExport, L"开始导出", Ic::Export, true,
                                [this] { startExport(); });

        // ---- Pack ---------------------------------------------------------
        // The form is split into three tabs so every field stays readable at
        // any window size; the action button lives in the tab bar.
        btnTabBasic_ = makeButton(PgPack, L"基本", Ic::Pack, false,
                                  [this] { setPackTab(0); });
        btnTabAdvanced_ = makeButton(PgPack, L"高级", Ic::Settings, false,
                                     [this] { setPackTab(1); });
        btnTabSign_ = makeButton(PgPack, L"签名", Ic::Shield, false,
                                 [this] { setPackTab(2); });
        btnTabBasic_->accent = true;

        tbSrcDir_ = makeText(PgPack, L"要打包的源目录");
        btnSrcBrowse_ = makeButton(PgPack, L"浏览", Ic::Folder, false,
                                   [this] { browseFolderFor(tbSrcDir_, L"选择源目录");
                                            suggestPackDefaults(); });
        tbOutMsi_ = makeText(PgPack, L"输出 .msi 路径");
        btnOutMsiBrowse_ = makeButton(PgPack, L"保存", Ic::Save, false,
                                      [this] { browseSaveMsi(); });
        tbName_ = makeText(PgPack, L"留空则使用目录名");
        tbVersion_ = makeText(PgPack, L"1.0.0");
        tbVersion_->setValue(L"1.0.0");
        tbManufacturer_ = makeText(PgPack, L"msitool");
        tbManufacturer_->setValue(L"msitool");
        ddPlatform_ = add<DropDown>(PgPack);
        ddPlatform_->setItems({L"Intel (x86)", L"x64"});
        tbLanguage_ = makeText(PgPack, L"1033");
        tbLanguage_->setValue(L"1033");
        ddUi_ = add<DropDown>(PgPack);
        ddUi_->setItems({L"none（无安装界面）", L"basic（基础进度）", L"minimal（内置向导）"});
        tbCabSize_ = makeText(PgPack, L"留空为单个 CAB，例如 16M / 1G");
        tbConfig_ = makeText(PgPack, L"可选：pack.ini 清单");
        btnConfigBrowse_ = makeButton(PgPack, L"浏览", Ic::Folder, false,
                                      [this] { browseFileFor(tbConfig_, false,
                                          L"选择清单", L"INI 清单", L"*.ini"); });
        ddLargeFiles_ = add<DropDown>(PgPack);
        ddLargeFiles_->setItems({L"layout（>2GiB 作为伴随文件）", L"error（拒绝大文件）"});
        tbSignCert_ = makeText(PgPack, L"可选：.pfx 代码签名证书");
        btnSignCertBrowse_ = makeButton(PgPack, L"浏览", Ic::Folder, false,
                                        [this] { browseFileFor(tbSignCert_, false,
                                            L"选择证书", L"PFX 证书", L"*.pfx"); });
        tbSignPassword_ = makeText(PgPack, L"证书密码", false, false, true);
        tbSignTime_ = makeText(PgPack, L"可选：RFC 3161 时间戳 URL");
        chkForce_ = makeCheck(PgPack, L"覆盖已存在文件", false);
        chkPackDry_ = makeCheck(PgPack, L"仅预览 (dry-run)", false);
        chkPackVerbose_ = makeCheck(PgPack, L"详细日志", true);
        chkEmptyDirs_ = makeCheck(PgPack, L"保留空目录", true);
        btnPack_ = makeButton(PgPack, L"开始打包", Ic::Pack, true,
                              [this] { startPack(); });

        auto tag = [this](int tab, std::initializer_list<Widget*> ws) {
            for (Widget* w : ws) w->setTab(tab);
        };
        tag(0, {tbSrcDir_, btnSrcBrowse_, tbOutMsi_, btnOutMsiBrowse_,
                tbName_, tbVersion_, tbManufacturer_, ddPlatform_,
                tbLanguage_, ddUi_});
        tag(1, {tbCabSize_, tbConfig_, btnConfigBrowse_, ddLargeFiles_,
                chkForce_, chkPackDry_, chkPackVerbose_, chkEmptyDirs_});
        tag(2, {tbSignCert_, btnSignCertBrowse_, tbSignPassword_, tbSignTime_});

        // ---- Verify -------------------------------------------------------
        gridVerify_ = add<DataGrid>(PgVerify);
        btnVerify_ = makeButton(PgVerify, L"重新验证", Ic::Refresh, false,
                                [this] { runVerify(); });

        // ---- Settings -----------------------------------------------------
        btnLight_ = makeButton(PgSettings, L"浅色", Ic::Sun, false,
                               [this] { setThemeMode(0); });
        btnDark_ = makeButton(PgSettings, L"深色", Ic::Moon, false,
                              [this] { setThemeMode(1); });
        btnSystem_ = makeButton(PgSettings, L"跟随系统", Ic::Sparkle, false,
                                [this] { setThemeMode(2); });
        linkHome_ = add<Link>(PgSettings);
        linkHome_->text = kAppHome;
        linkHome_->url = kAppHome;
        linkHome_->mono = true;

        // Toast is a separate child window so it can float above the cards,
        // grids and every other widget.
        toastView_ = std::make_unique<ToastView>();

        applyTheme();
        layout();
    }

    Check* makeCheck(int page, const wchar_t* text, bool checked) {
        Check* c = add<Check>(page);
        c->text = text;
        c->checked = checked;
        return c;
    }

    // ---- layout ----------------------------------------------------------
    void addCard(RECT r, std::wstring title, std::wstring sub = L"",
                 Ic icon = Ic::None, bool hero = false) {
        cards_.push_back({r, std::move(title), std::move(sub), icon, hero});
    }
    void addLabel(RECT r, const std::wstring& text, HFONT font, COLORREF color,
                  UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
        labels_.push_back({r, text, font, color, fmt});
    }

    struct Base {
        RECT client, header, filebar, body;
    };

    Base baseLayout() {
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        int cap = S(40), navW = S(216), statusH = S(34), m = S(20);
        int left = navW + m, right = rc.right - m;
        int top = cap + S(16);
        RECT header = R(left, top, right - left, S(46));
        RECT filebar = R(left, header.bottom + S(10), right - left, S(76));
        // The extract page is an Explorer-like full-page browser: no page
        // title and no redundant "current package" bar, so the file list is
        // the main subject and gets the whole client area. The pack page is
        // the other way round: it produces a package, it does not read one,
        // so showing "当前包文件" there only mixes the two workflows.
        int bodyTop = page_ == PgExtract ? top
                    : showsFileBar() ? filebar.bottom + S(16)
                                     : header.bottom + S(16);
        RECT body = R(left, bodyTop, right - left,
                      rc.bottom - statusH - S(16) - bodyTop);
        return {rc, header, filebar, body};
    }

    void layout() {
        cards_.clear();
        labels_.clear();
        bars_.clear();
        Base b = baseLayout();
        Theme& t = theme();
        int cap = S(40), navW = S(216), statusH = S(34);
        int bw = S(46);
        int cx = b.client.right - bw * 4;
        btnTheme_->setRect(R(cx, 0, bw, cap));
        btnMin_->setRect(R(cx + bw, 0, bw, cap));
        btnMax_->setRect(R(cx + bw * 2, 0, bw, cap));
        btnClose_->setRect(R(cx + bw * 3, 0, bw, cap));
        btnMax_->icon = IsZoomed(hwnd_) ? Ic::Restore : Ic::Max;

        int ny = cap + S(74);
        for (int i = 0; i < PgCount; ++i)
            nav_[i]->setRect(R(S(8), ny + i * S(44), navW - S(16), S(40)));

        progress_->setRect(R(b.client.right - S(220),
                             b.client.bottom - statusH + S(14), S(200), S(6)));
        if (toastVisible_ && toastView_ && toastView_->hwnd()) {
            positionToast();
        }

        // Static texts for caption / nav / header / file bar.
        addLabel(R(S(20), cap + S(14), navW - S(40), S(22)), L"导航",
                 t.fSmall, t.muted);
        addLabel(R(S(20), b.client.bottom - statusH - S(32), navW - S(40), S(22)),
                 L"Msi API · FDI / FCI · 纯 Win32", t.fSmall, t.muted);
        if (page_ != PgExtract) {
            addLabel(R(b.header.left, b.header.top, W(b.header), S(28)),
                     kPageTitle[page_], t.fTitle, t.text);
            addLabel(R(b.header.left, b.header.top + S(28), W(b.header), S(20)),
                     kPageSubtitle[page_], t.fSmall, t.subText);
        }

        if (showsFileBar()) {
            addCard(b.filebar, L"", L"");
            addLabel(R(b.filebar.left + S(16), b.filebar.top + S(8), S(240), S(18)),
                     L"当前包文件", t.fSmall, t.subText);
            addLabel(R(b.filebar.right - S(330), b.filebar.top + S(8), S(314), S(18)),
                     L"支持把 .msi / .msp 直接拖进窗口", t.fSmall, t.muted,
                     DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            int fy = b.filebar.top + S(30);
            tbPackage_->setRect(R(b.filebar.left + S(16), fy,
                                  W(b.filebar) - S(32) - S(180), S(34)));
            btnBrowse_->setRect(R(b.filebar.right - S(16) - S(172), fy, S(84), S(34)));
            btnOpen_->setRect(R(b.filebar.right - S(16) - S(84), fy, S(84), S(34)));
        }

        switch (page_) {
        case PgHome:     layoutHome(b.body); break;
        case PgInfo:     layoutInfo(b.body); break;
        case PgTables:   layoutTables(b.body); break;
        case PgQuery:    layoutQuery(b.body); break;
        case PgExtract:  layoutExtract(b.body); break;
        case PgExport:   layoutExport(b.body); break;
        case PgPack:     layoutPack(b.body); break;
        case PgVerify:   layoutVerify(b.body); break;
        case PgSettings: layoutSettings(b.body); break;
        default: break;
        }
        for (auto& w : widgets_) w->invalidate();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void layoutHome(const RECT& body) {
        Theme& t = theme();
        RECT hero = R(body.left, body.top, W(body), S(132));
        addCard(hero, L"msitool", L"查看、解包与构建 Windows Installer 包 · 现代化 Fluent 界面",
                Ic::Sparkle, true);
        int y = hero.bottom + S(16);
        int h = std::max(S(180), static_cast<int>(body.bottom - y));
        int gap = S(16);
        int lw = (W(body) - gap) * 60 / 100;
        RECT info = R(body.left, y, lw, h);
        RECT act = R(info.right + gap, y, W(body) - lw - gap, h);
        addCard(info, L"包摘要", L"打开包后自动填充", Ic::Info);
        addCard(act, L"快捷操作", L"常用任务一步直达", Ic::Bolt);
        gridHome_->setRect(R(info.left + S(12), info.top + S(48),
                             W(info) - S(24),
                             std::max(S(80), static_cast<int>(H(info) - S(60)))));
        int ax = act.left + S(16), aw = W(act) - S(32);
        int ay = act.top + S(52);
        btnHomeVerify_->setRect(R(ax, ay, aw, S(38)));
        btnHomeExtract_->setRect(R(ax, ay + S(48), aw, S(38)));
        btnHomeExport_->setRect(R(ax, ay + S(96), aw, S(38)));
        addLabel(R(ax, ay + S(148), aw, S(22)), L"验证包结构并查看摘要信息",
                 t.fSmall, t.muted);
        addLabel(R(ax, ay + S(172), aw, S(22)), L"解包会重建安装目录树，不会安装任何内容",
                 t.fSmall, t.muted);
    }

    void layoutInfo(const RECT& body) {
        Theme& t = theme();
        const int gap = S(16);

        // 1) Header: the handful of facts you actually want at a glance.
        RECT hero = R(body.left, body.top, W(body), S(124));
        addCard(hero, infoTitle_, infoSubtitle_, Ic::Database, true);
        int segW = (W(hero) - S(56)) / 4;
        for (int i = 0; i < 4; ++i) {
            addLabel(R(hero.left + S(28) + i * segW, hero.top + S(100),
                       segW - S(10), S(22)),
                     infoStats_[i], t.fSmall, RGB(232, 244, 255),
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // 2) Tab strip + filter + copy.
        RECT tabBar = R(body.left, hero.bottom + gap, W(body), S(46));
        addCard(tabBar, L"", L"");
        int tw = S(120), th = S(46) - S(10);
        int tx = tabBar.left + S(10), ty = tabBar.top + S(5);
        btnInfoSummary_->setRect(R(tx, ty, tw, th));
        btnInfoProps_->setRect(R(tx + tw + S(8), ty, tw, th));
        btnInfoMedia_->setRect(R(tx + (tw + S(8)) * 2, ty, tw, th));
        btnInfoCopy_->setRect(R(tabBar.right - S(10) - S(100), ty, S(100), th));
        tbInfoSearch_->setRect(R(tabBar.right - S(10) - S(100) - S(8) - S(260),
                                 ty, S(260), th));

        // 3) One full-width table + a detail strip for the selected row.
        RECT content = R(body.left, tabBar.bottom + gap, W(body),
                         body.bottom - tabBar.bottom - gap);
        static const wchar_t* kTitle[3] = {
            L"SummaryInformation", L"Property 属性", L"媒体 / CAB"};
        static const Ic kIcon[3] = {Ic::Info, Ic::Table, Ic::Database};
        addCard(content, kTitle[infoTab_], infoCount_, kIcon[infoTab_]);
        // The detail strip only appears after a row is selected; otherwise
        // the table gets the full remaining height.
        bool detail = !infoDetail_.empty();
        int detailH = detail ? S(34) : 0;
        int gridH = std::max(S(80), H(content) - S(48) - S(12) -
                                       (detail ? detailH + S(10) : 0));
        gridInfo_->setRect(R(content.left + S(12), content.top + S(48),
                             W(content) - S(24), gridH));
        infoDetailRect_ = detail
            ? R(content.left + S(12), content.bottom - detailH - S(12),
                W(content) - S(24), detailH)
            : RECT{0, 0, 0, 0};
    }

    void layoutTables(const RECT& body) {
        int listW = S(264);
        RECT lc = R(body.left, body.top, listW, H(body));
        addCard(lc, L"表", tableInfo_, Ic::Table);
        tbTableSearch_->setRect(R(lc.left + S(12), lc.top + S(48),
                                  W(lc) - S(24), S(34)));
        gridTables_->setRect(R(lc.left + S(12), lc.top + S(48) + S(42),
                               W(lc) - S(24), H(lc) - S(48) - S(42) - S(12)));
        RECT rc = R(lc.right + S(16), body.top, W(body) - listW - S(16), H(body));
        addCard(rc, L"表内容", L"选中左侧的表，或双击行查看", Ic::Table);
        gridTableData_->setRect(R(rc.left + S(12), rc.top + S(48),
                                 W(rc) - S(24), H(rc) - S(60)));
        btnTableCopy_->setRect(R(rc.right - S(116), rc.top + S(10), S(104), S(30)));
    }

    void layoutQuery(const RECT& body) {
        int eh = S(196);
        RECT ec = R(body.left, body.top, W(body), eh);
        addCard(ec, L"SQL 查询", L"只读视图 · 支持 MSI SQL 与 ? 参数", Ic::Terminal);
        tbSql_->setRect(R(ec.left + S(12), ec.top + S(48), W(ec) - S(24), S(84)));
        int by = ec.top + S(48) + S(84) + S(12);
        int x = ec.left + S(12);
        ddExamples_->setRect(R(x, by, S(230), S(34)));
        btnQueryRun_->setRect(R(x + S(238), by, S(120), S(34)));
        btnQueryCopy_->setRect(R(x + S(366), by, S(112), S(34)));
        btnQueryClear_->setRect(R(x + S(486), by, S(100), S(34)));
        RECT rc = R(body.left, ec.bottom + S(16), W(body), body.bottom - ec.bottom - S(16));
        addCard(rc, L"结果", queryInfo_, Ic::Table);
        gridQueryResult_->setRect(R(rc.left + S(12), rc.top + S(48),
                                   W(rc) - S(24), H(rc) - S(60)));
    }

    void layoutLogCard(RECT c, int topPad) {
        // The card header paints a title at S(9)..S(33) and a subtitle at
        // S(30)..S(48). The log view must start below both, otherwise its
        // white rounded frame clips the lower half of the small caption.
        topPad = std::max(topPad, S(54));
        addCard(c, L"运行日志", L"命令输出实时显示 · Ctrl+C 复制", Ic::Terminal);
        log_->setRect(R(c.left + S(12), c.top + topPad,
                        W(c) - S(24),
                        std::max(S(24), H(c) - topPad - S(12))));
        btnClearLog_->setRect(R(c.right - S(96), c.top + S(10), S(84), S(30)));
        btnOpenFolder_->setRect(R(c.right - S(212), c.top + S(10), S(108), S(30)));
    }

    void layoutExtract(const RECT& body) {
        Theme& t = theme();
        const int gap = S(8);
        const int cmdH = S(46);
        const int optH = extractAdv_ ? S(104) : 0;
        const int statusH = S(26);
        int logH = 0;   // computed below, after the panels above are placed

        // 1) Explorer-style command / address bar.
        RECT cmd = R(body.left, body.top, W(body), cmdH);
        addCard(cmd, L"", L"");
        int by = cmd.top + (cmdH - S(32)) / 2;
        int bx = cmd.left + S(8);
        auto navBtn = [&](Button* b) {
            b->setRect(R(bx, by, S(32), S(32)));
            bx += S(32) + S(4);
        };
        navBtn(btnNavBack_);
        navBtn(btnNavFwd_);
        navBtn(btnNavUp_);
        navBtn(btnNavRefresh_);
        bx += S(8);
        int rightX = cmd.right - S(8);
        auto rightBtn = [&](Button* b, int w) {
            rightX -= w;
            b->setRect(R(rightX, by, w, S(32)));
            rightX -= S(6);
        };
        rightBtn(btnExtractLog_, S(72));
        rightBtn(btnExtractAdv_, S(72));
        rightBtn(btnExtractSel_, S(104));
        rightBtn(btnExtractAll_, S(116));
        int searchW = S(200);
        tbBrowseFilter_->setRect(R(rightX - searchW, by, searchW, S(32)));
        int crumbW = rightX - searchW - S(8) - bx;
        crumb_->setRect(R(bx, by, std::max(S(120), crumbW), S(32)));

        // 2) Always-visible destination bar ("解包到").
        const int locH = S(38);
        int locTop = cmd.bottom + gap;
        RECT loc = R(body.left, locTop, W(body), locH);
        addCard(loc, L"", L"");
        int ly = loc.top + (locH - S(28)) / 2;
        int lx = loc.left + S(12);
        addLabel(R(lx, ly, S(58), S(28)), L"解包到", t.fSmall, t.subText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        lx += S(62);
        int rightL = loc.right - S(12);
        auto locBtn = [&](Button* b, int w) {
            rightL -= w;
            b->setRect(R(rightL, ly, w, S(28)));
            rightL -= S(6);
        };
        locBtn(btnExtractOpen_, S(72));
        locBtn(btnRecentDirs_, S(72));
        locBtn(btnOutBrowse_, S(84));
        tbOutDir_->setRect(R(lx, ly, std::max(S(120), rightL - lx), S(28)));

        // 3) Collapsible advanced options (switches / transforms only).
        int y = locTop + locH + gap;
        if (optH) {
            RECT opt = R(body.left, y, W(body), optH);
            addCard(opt, L"", L"");
            int ox = opt.left + S(12), oy = opt.top + S(10);
            int colW = (W(opt) - S(24) - S(16)) / 2;
            int fieldW = colW - S(80);
            auto fieldAt = [&](int row, TextBox* tb, Button* browse,
                               const wchar_t* label) {
                int yy = oy + row * S(40);
                addLabel(R(ox, yy, colW, S(16)), label, t.fSmall, t.subText);
                tb->setRect(R(ox, yy + S(17), fieldW, S(26)));
                browse->setRect(R(ox + fieldW + S(6), yy + S(17), S(72), S(26)));
            };
            fieldAt(0, tbTransforms_, btnTransBrowse_, L"MST 转换");
            fieldAt(1, tbBaseMsi_, btnBaseBrowse_, L"基准 MSI");
            int rx = ox + colW + S(16);
            Check* checks[] = {chkFlat_, chkDry_, chkSkip_, chkSource_,
                               chkKeepTemp_, chkVerbose_};
            // 3 columns x 2 rows keeps the panel short enough that the last
            // row can never spill into the file list below.
            for (int i = 0; i < 6; ++i) {
                int c = i % 3, r = i / 3;
                checks[i]->setRect(R(rx + c * (colW / 3), oy + r * S(40),
                                     colW / 3 - S(8), S(34)));
            }
            y += optH;
        }

        // 4) Roomy, height-adaptive log drawer. It takes roughly 38% of the
        //    page so a dozen log lines are visible, but never squeezes the
        //    file browser below a usable minimum (e.g. when options are open).
        if (extractLog_) {
            logH = std::min(S(380), std::max(S(220), H(body) * 38 / 100));
            const int minMain = S(190);
            const int maxLog = (body.bottom - statusH) - (y + gap) - minMain - gap;
            if (logH > maxLog) logH = std::max(0, maxLog);
        }

        // 5) Main Explorer area: navigation tree + details list.
        int mainTop = y + gap;
        int mainBottom = body.bottom - statusH - (logH ? logH + gap : 0);
        RECT main = R(body.left, mainTop, W(body),
                      std::max(S(120), mainBottom - mainTop));
        int treeW = std::min(S(260), W(main) * 26 / 100);
        int listW = W(main) - treeW - S(8);
        gridBrowseDirs_->setRect(R(main.left, main.top, treeW, H(main)));
        gridBrowseFiles_->setRect(R(main.left + treeW + S(8), main.top,
                                    listW, H(main)));
        gridBrowseDirs_->setColumnWidths({treeW - S(8)});
        int fixed = S(90) + S(130) + S(170) + S(70) + S(100);
        gridBrowseFiles_->setColumnWidths({
            std::max(S(180), listW - fixed), S(90), S(130), S(170), S(70), S(100)});

        // 6) Page status strip.
        addLabel(R(main.left + S(4), main.bottom + S(2),
                   W(main) - S(300), statusH),
                 browseStatus_, t.fSmall, t.subText,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        addLabel(R(main.right - S(292), main.bottom + S(2), S(288), statusH),
                 L"双击文件解包 · 双击文件夹进入 · 右键更多", t.fSmall, t.muted,
                 DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

        // 7) Collapsible log drawer.
        if (logH) {
            RECT lc = R(body.left, body.bottom - logH, W(body), logH);
            layoutLogCard(lc, S(54));
        }
    }

    void layoutExport(const RECT& body) {
        Theme& t = theme();
        int optW = S(430);
        RECT oc = R(body.left, body.top, optW, H(body));
        RECT lc = R(oc.right + S(16), body.top, W(body) - optW - S(16), H(body));
        addCard(oc, L"导出选项", L"把包内容完整导出到目录", Ic::Export);
        layoutLogCard(lc, S(54));
        int x = oc.left + S(16), w = W(oc) - S(32);
        int y = oc.top + S(48);
        addLabel(R(x, y, w, S(18)), L"导出模式", t.fSmall, t.subText); y += S(20);
        ddExportMode_->setRect(R(x, y, w, S(34))); y += S(34) + S(18);
        addLabel(R(x, y, w, S(18)), L"输出目录", t.fSmall, t.subText); y += S(20);
        tbExportDir_->setRect(R(x, y, w - S(80), S(32)));
        btnExportBrowse_->setRect(R(x + w - S(72), y, S(72), S(32)));
        y += S(32) + S(24);
        btnExport_->setRect(R(x, y, S(184), S(40)));
        addLabel(R(x, y + S(54), w, S(60)),
                 L"完整归档会导出全部 .idt 表、原始流、Binary 条目与 summary.txt；"
                 L"仅导出 CAB 时会附带 manifest.txt。",
                 t.fSmall, t.muted, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    void layoutPack(const RECT& body) {
        Theme& t = theme();
        const int gap = S(16);
        const int tabH = S(46);

        // Everything below is derived from the current client size: log
        // height, form height, row pitch, control height and column width all
        // adapt when the window is resized / maximized.
        int logH = std::min(S(170), std::max(S(96), H(body) * 22 / 100));
        int formH = H(body) - tabH - gap - logH - gap;
        const int minFormH = S(230);
        if (formH < minFormH) {
            logH = std::max(S(72), H(body) - tabH - gap * 2 - minFormH);
            formH = H(body) - tabH - gap - logH - gap;
        }
        RECT tabBar  = R(body.left, body.top, W(body), tabH);
        RECT form    = R(body.left, tabBar.bottom + gap, W(body), formH);
        RECT logCard = R(body.left, form.bottom + gap, W(body), logH);
        packForm_ = form;        // drop fallback target of this page

        static const wchar_t* kTitle[3] = {
            L"基本信息", L"高级选项", L"代码签名（可选）"};
        static const wchar_t* kSub[3] = {
            L"源目录、输出路径与产品标识 · 支持拖放",
            L"分卷、清单、大文件与构建开关 · 支持拖放",
            L"Authenticode 签名需要 signtool.exe 与证书 · 支持拖放"};
        static const Ic kIcon[3] = {Ic::Pack, Ic::Settings, Ic::Shield};
        addCard(tabBar, L"", L"");
        addCard(form, kTitle[packTab_], kSub[packTab_], kIcon[packTab_]);
        addCard(logCard, L"运行日志", L"FCI 压缩与 Msi 建库过程", Ic::Terminal);

        // Tab strip + always-visible primary action.
        int tw = S(112), th = tabH - S(10), tx = tabBar.left + S(10);
        int ty = tabBar.top + S(5);
        btnTabBasic_->setRect(R(tx, ty, tw, th));
        btnTabAdvanced_->setRect(R(tx + tw + S(8), ty, tw, th));
        btnTabSign_->setRect(R(tx + (tw + S(8)) * 2, ty, tw, th));
        btnPack_->setRect(R(tabBar.right - S(10) - S(170), ty, S(170), th));

        // Two adaptive columns inside the form card.
        const int padX = S(18), colGap = S(28);
        int innerW = W(form) - padX * 2 - colGap;
        int colW = innerW / 2;
        int leftX = form.left + padX;
        int rightX = leftX + colW + colGap;
        const int headerPad = S(58);
        const int rows = 4;
        int usable = std::max(S(120), H(form) - headerPad - S(16));
        int rowH = std::min(S(76), std::max(S(34), usable / rows));
        int labelH = S(18);
        // Keep a little breathing room between a control and the next row's
        // label even when the window is at its minimum size.
        int ctrlH = std::min(S(32), std::max(S(20), rowH - labelH - S(6)));

        auto field = [&](int col, int row, const wchar_t* label, Widget* ctrl,
                         Button* browse) {
            int x = col == 0 ? leftX : rightX;
            int y = form.top + headerPad + row * rowH;
            addLabel(R(x, y, colW, labelH), label, t.fSmall, t.subText);
            int cy = y + labelH + S(2);
            int bw = browse ? S(78) : 0;
            ctrl->setRect(R(x, cy, colW - bw - (bw ? S(8) : 0), ctrlH));
            if (browse) browse->setRect(R(x + colW - bw, cy, bw, ctrlH));
        };
        auto note = [&](int col, int row, int spanRows, const wchar_t* text) {
            int x = col == 0 ? leftX : rightX;
            int y = form.top + headerPad + row * rowH + S(6);
            int h = std::min(rowH * spanRows,
                             static_cast<int>(form.bottom - y - S(10)));
            if (h > S(16))
                addLabel(R(x, y, colW, h), text, t.fSmall, t.muted,
                         DT_LEFT | DT_TOP | DT_WORDBREAK);
        };

        if (packTab_ == 0) {
            field(0, 0, L"源目录", tbSrcDir_, btnSrcBrowse_);
            field(0, 1, L"输出 MSI", tbOutMsi_, btnOutMsiBrowse_);
            field(0, 2, L"产品名称", tbName_, nullptr);
            field(0, 3, L"产品版本", tbVersion_, nullptr);
            field(1, 0, L"制造商", tbManufacturer_, nullptr);
            field(1, 1, L"平台", ddPlatform_, nullptr);
            field(1, 2, L"语言 (LCID)", tbLanguage_, nullptr);
            field(1, 3, L"安装界面", ddUi_, nullptr);
        } else if (packTab_ == 1) {
            field(0, 0, L"CAB 分卷大小", tbCabSize_, nullptr);
            field(0, 1, L"配置文件 (pack.ini)", tbConfig_, btnConfigBrowse_);
            field(0, 2, L"大文件处理", ddLargeFiles_, nullptr);
            note(0, 3, 1, L"留空表示单个 CAB；支持 16M / 1G 等形式。清单可描述 Feature、快捷方式、注册表、服务与环境变量。");
            int cbw = colW / 2 - S(6);
            int cbh = std::min(S(38), rowH);
            int cby = form.top + headerPad;
            chkForce_->setRect(R(rightX, cby, cbw, cbh));
            chkPackDry_->setRect(R(rightX + colW / 2, cby, cbw, cbh));
            chkPackVerbose_->setRect(R(rightX, cby + cbh, cbw, cbh));
            chkEmptyDirs_->setRect(R(rightX + colW / 2, cby + cbh, cbw, cbh));
            note(1, 2, 2, L"开关作用于本次打包：dry-run 只打印计划不写文件；详细日志会把每个文件与 CAB 的构建过程输出到右侧日志。");
        } else {
            field(0, 0, L"签名证书 (.pfx)", tbSignCert_, btnSignCertBrowse_);
            field(0, 1, L"证书密码", tbSignPassword_, nullptr);
            field(0, 2, L"时间戳 URL", tbSignTime_, nullptr);
            note(0, 3, 1, L"三项都留空即不签名；签名会写入 \\x05DigitalSignature 流并再次自检。");
            note(1, 0, 4, L"打包完成后会自动打开生成的 MSI，方便立即检查表、CAB 与验证报告。\n\n源目录与输出 MSI 在「基本」标签页，构建开关在「高级」标签页；无论当前在哪个标签页，右上角的「开始打包」都会使用全部字段。");
        }

        // Log card is shared with the extract / export pages.
        int logBody = std::max(S(24), H(logCard) - S(66));
        log_->setRect(R(logCard.left + S(12), logCard.top + S(54),
                        W(logCard) - S(24), logBody));
        btnClearLog_->setRect(R(logCard.right - S(96), logCard.top + S(8),
                                S(84), S(30)));
        btnOpenFolder_->setRect(R(logCard.right - S(212), logCard.top + S(8),
                                  S(108), S(30)));
    }

    void layoutVerify(const RECT& body) {
        RECT bar = R(body.left, body.top, W(body), S(58));
        bars_.push_back({bar, verifyText_, verifyOk_ ? 3 : 2,
                         verifyOk_ ? Ic::Check : Ic::Warning});
        RECT card = R(body.left, bar.bottom + S(16), W(body),
                      body.bottom - bar.bottom - S(16));
        addCard(card, L"验证报告", L"签名、MsiOpenDatabase、_Tables、媒体与摘要",
                Ic::Shield);
        gridVerify_->setRect(R(card.left + S(12), card.top + S(48),
                               W(card) - S(24), H(card) - S(60)));
        btnVerify_->setRect(R(card.right - S(128), card.top + S(10), S(116), S(30)));
    }

    void layoutSettings(const RECT& body) {
        Theme& t = theme();
        RECT c1 = R(body.left, body.top, W(body), S(150));
        addCard(c1, L"外观", L"Fluent 设计 · 支持系统主题跟随与高 DPI", Ic::Sun);
        int x = c1.left + S(16);
        btnLight_->setRect(R(x, c1.top + S(52), S(150), S(36)));
        btnDark_->setRect(R(x + S(158), c1.top + S(52), S(150), S(36)));
        btnSystem_->setRect(R(x + S(316), c1.top + S(52), S(150), S(36)));
        addLabel(R(x, c1.top + S(100), W(c1) - S(32), S(22)),
                 util::fmt(L"当前：%s · 界面缩放 %d%% · 每个像素由 GDI 绘制，无任何第三方 UI 库",
                           theme().dark ? L"深色" : L"浅色", theme().dpi * 100 / 96),
                 t.fSmall, t.muted);

        RECT c2 = R(body.left, c1.bottom + S(16), W(body), S(190));
        addCard(c2, L"关于 msitool",
                util::fmt(L"版本 %s · %s · 项目主页与构建信息",
                          kAppVersion, kAppLicense),
                Ic::Info);
        int c2pad = S(16);
        int leftW = W(c2) * 56 / 100 - c2pad * 2;
        addLabel(R(c2.left + c2pad, c2.top + S(50), leftW, S(170)),
                 L"用 MSVC + C++17 + Windows Installer (Msi) API 与 cabinet.dll\n"
                 L"(FDI/FCI) 编写的 MSI / MSP 查看、解包与打包工具。\n\n"
                 L"UI 层为纯 Win32 API（user32 / gdi32 / shell32 / ole32）自绘：\n"
                 L"Fluent 亮暗主题、圆角卡片、导航、数据网格、进度与日志。\n"
                 L"命令行模式完全保留：在终端带参数运行即可使用全部命令。",
                 t.fBody, t.subText, DT_LEFT | DT_TOP | DT_WORDBREAK);

        int infoX = c2.left + c2pad + leftW + c2pad;
        int infoKeyW = S(76);
        int infoValW = c2.right - c2pad - (infoX + infoKeyW);
        int rowH = S(23);
        int rowY = c2.top + S(46);
        auto infoRow = [&](const wchar_t* key, const std::wstring& val) {
            addLabel(R(infoX, rowY, infoKeyW, rowH), key, t.fMono, t.muted,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            addLabel(R(infoX + infoKeyW, rowY, infoValW, rowH), val,
                     t.fMono, t.subText,
                     DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            rowY += rowH;
        };
        infoRow(L"版本", std::wstring(L"v") + kAppVersion);
        infoRow(L"协议", kAppLicense);
        addLabel(R(infoX, rowY, infoKeyW, rowH), L"项目主页", t.fMono, t.muted,
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (linkHome_)
            linkHome_->setRect(R(infoX + infoKeyW, rowY, infoValW, rowH));
        rowY += rowH;
        infoRow(L"构建", buildStamp());
        infoRow(L"命令行", L"msitool.exe --help");
        infoRow(L"依赖", L"msi.dll · cabinet.dll（Windows 自带）");

        RECT c3 = R(body.left, c2.bottom + S(16), W(body),
                    std::max(S(120), static_cast<int>(body.bottom - c2.bottom - S(16))));
        addCard(c3, L"快捷键", L"", Ic::Terminal);
        addLabel(R(c3.left + S(16), c3.top + S(50), W(c3) - S(32), S(120)),
                 L"Ctrl + O     打开文件\n"
                 L"F5               重新载入当前包\n"
                 L"Ctrl + C     复制数据网格 / 日志内容\n"
                 L"Ctrl + Enter  在 SQL 编辑器中运行查询\n"
                 L"Tab / Shift+Tab  在输入框之间切换焦点\n"
                 L"Esc              关闭下拉菜单 / 取消焦点",
                 t.fMono, t.subText, DT_LEFT | DT_TOP | DT_WORDBREAK);
    }

    void switchPage(int p) {
        if (p < 0 || p >= PgCount) return;
        if (running_ && p != page_) { toast(L"任务进行中，请稍候", 1); return; }
        page_ = p;
        for (int i = 0; i < PgCount; ++i) nav_[i]->selected = (i == p);
        setVisibility();
        layout();
        if (p == PgVerify && !package_.empty()) runVerify();
    }

    // The "current package" bar belongs to the read side of the tool. It is
    // hidden on 解包 (its own browser shows the package), on 打包 (which builds
    // a package) and on 设置.
    bool showsFileBar() const {
        return page_ != PgSettings && page_ != PgExtract && page_ != PgPack;
    }

    void setVisibility() {
        for (auto& w : widgets_) {
            int p = w->page();
            bool v = p >= 0 ? (p == page_)
                   : p == -1 ? true
                   : p == -2 ? showsFileBar()
                   : (page_ == PgExtract || page_ == PgExport || page_ == PgPack);
            if (page_ == PgPack && w->tab() >= 0 && w->tab() != packTab_)
                v = false;
            if (page_ == PgExtract && w->tab() == 1 && !extractAdv_)
                v = false;
            if (page_ == PgExtract && w->tab() == 2 && !extractLog_)
                v = false;
            w->setVisible(v);
        }
    }

    void setPackTab(int tab) {
        packTab_ = tab;
        btnTabBasic_->accent = (tab == 0);
        btnTabAdvanced_->accent = (tab == 1);
        btnTabSign_->accent = (tab == 2);
        btnTabBasic_->invalidate();
        btnTabAdvanced_->invalidate();
        btnTabSign_->invalidate();
        setVisibility();
        layout();
    }

    // ---- painting --------------------------------------------------------
    void paint(HDC hdc, const RECT& rc) {
        Theme& t = theme();
        Gfx g(hdc, rc);
        for (auto& w : widgets_)
            if (w->visible() && w->hwnd()) g.exclude(w->rect());

        g.gradient(rc, t.bgTop, t.bgBottom);
        int cap = S(40), navW = S(216), statusH = S(34);
        g.fill(R(0, 0, rc.right, cap), t.navBg);
        g.line(0, cap - 1, rc.right, cap - 1, t.border);
        g.fill(R(0, cap, navW, rc.bottom - statusH - cap), t.navBg);
        g.line(navW - 1, cap, navW - 1, rc.bottom - statusH, t.border);
        // Brand icon in the caption bar.
        RECT brand{R(S(16), (cap - S(24)) / 2, S(24), S(24))};
        g.icon(Ic::App, brand, t.accent, 2);
        g.text(R(S(50), 0, S(220), cap), L"msitool", t.text, t.fStrong,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        g.text(R(S(142), 0, S(240), cap), L"MSI / MSP 工具箱", t.muted, t.fSmall,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE);

        for (const Card& c : cards_) {
            if (c.hero) {
                SaveDC(g.dc());
                HRGN rgn = CreateRoundRectRgn(c.r.left, c.r.top, c.r.right, c.r.bottom,
                                              S(12), S(12));
                SelectClipRgn(g.dc(), rgn);
                DeleteObject(rgn);
                g.gradient(c.r, t.heroA, t.heroB, false);
                // decorative concentric arcs
                for (int i = 0; i < 3; ++i) {
                    int r = S(70) + i * S(52);
                    g.circle(c.r.right - S(90), c.r.top + S(10), r, kNone,
                             mix(t.heroB, RGB(255, 255, 255), 22), S(1));
                }
                g.text(R(c.r.left + S(28), c.r.top + S(24), W(c.r) - S(120), S(38)),
                       c.title, RGB(255, 255, 255), t.fTitle,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                g.text(R(c.r.left + S(28), c.r.top + S(66), W(c.r) - S(120), S(24)),
                       c.subtitle, RGB(232, 244, 255), t.fBody,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
                RECT ic{c.r.right - S(96), c.r.top + S(30), c.r.right - S(32), c.r.top + S(94)};
                if (c.icon != Ic::None)
                    g.icon(c.icon, ic, RGB(255, 255, 255), 2);
                RestoreDC(g.dc(), -1);
            } else {
                g.round(c.r, S(10), t.card, t.border, 1);
                if (c.icon != Ic::None && !c.title.empty()) {
                    RECT ir{c.r.left + S(16), c.r.top + S(12),
                            c.r.left + S(36), c.r.top + S(32)};
                    g.icon(c.icon, ir, t.accent, 2);
                }
                if (!c.title.empty()) {
                    int tx = c.icon == Ic::None ? c.r.left + S(16) : c.r.left + S(44);
                    g.text(R(tx, c.r.top + S(9), W(c.r) - S(60), S(24)),
                           c.title, t.text, t.fSubtitle,
                           DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (!c.subtitle.empty())
                        g.text(R(tx, c.r.top + S(30), W(c.r) - S(60), S(16)),
                               c.subtitle, t.muted, t.fSmall,
                               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }
        }

        for (const Bar& bar : bars_) {
            COLORREF bg = bar.kind == 3 ? t.infoBarOk
                        : bar.kind == 2 ? t.infoBarBad : t.infoBarWarn;
            COLORREF fg = bar.kind == 3 ? t.success
                        : bar.kind == 2 ? t.danger : t.warning;
            g.round(bar.r, S(8), bg, mix(bg, fg, 30), 1);
            RECT ir{bar.r.left + S(14), bar.r.top + S(14),
                    bar.r.left + S(42), bar.r.top + S(44)};
            g.icon(bar.icon, ir, fg, 2);
            g.text(R(bar.r.left + S(52), bar.r.top, W(bar.r) - S(64), H(bar.r)),
                   bar.text, t.text, t.fStrong,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        for (const Label& l : labels_)
            g.text(l.r, l.text, l.color, l.font, l.fmt);

        // Drag & drop: outline the card that will receive the drop when the
        // target is the window-wide fallback (field targets glow themselves).
        if (dragging_ && zoneHit_ >= 0 && zoneHit_ < (int)zones_.size()) {
            const DropZone& z = zones_[zoneHit_];
            if (W(z.frame) > 0) {
                g.round(z.frame, S(10), kNone, t.accent, S(2));
                RECT chip = R(z.frame.right - S(200), z.frame.top + S(9),
                              S(190), S(28));
                g.round(chip, S(8), t.accentSoft, t.accent, 1);
                g.icon(z.kind == DkAuto ? Ic::Sparkle : Ic::Add,
                       R(chip.left + S(6), chip.top + S(7), S(14), S(14)),
                       t.accent, 2);
                g.text(R(chip.left + S(24), chip.top, W(chip) - S(28), H(chip)),
                       z.kind == DkAuto ? L"拖放自动分配"
                                        : L"拖放到此处",
                       t.accent, t.fSmall,
                       DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            }
        }

        // Info page: detail strip for the selected table row (or a hint).
        if (page_ == PgInfo && !infoDetail_.empty() &&
            infoDetailRect_.right > infoDetailRect_.left) {
            g.round(infoDetailRect_, S(6), t.cardAlt, t.border, 1);
            g.text(shrink(infoDetailRect_, S(12), 0), infoDetail_, t.subText,
                   t.fSmall,
                   DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        }

        // Status bar.
        g.fill(R(0, rc.bottom - statusH, rc.right, statusH), t.navBg);
        g.line(0, rc.bottom - statusH, rc.right, rc.bottom - statusH, t.border);
        g.text(R(S(24), rc.bottom - statusH, rc.right - S(460), statusH),
               status_, t.subText, t.fSmall,
               DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (!statusRight_.empty())
            g.text(R(rc.right - S(440), rc.bottom - statusH, S(200), statusH),
                   statusRight_, t.muted, t.fSmall,
                   DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        g.flush();
    }

    void positionToast() {
        if (!toastView_ || !toastView_->hwnd()) return;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        toastClientRect_ = R(rc.right - S(390),
                             rc.bottom - S(34) - S(76), S(370), S(64));
        POINT origin{toastClientRect_.left, toastClientRect_.top};
        ClientToScreen(hwnd_, &origin);
        RECT screen = R(origin.x, origin.y, W(toastClientRect_),
                        H(toastClientRect_));
        SetWindowPos(toastView_->hwnd(), HWND_TOP, screen.left, screen.top,
                     W(screen), H(screen), SWP_NOACTIVATE | SWP_SHOWWINDOW);
        toastView_->setRect(screen);   // popup rects are screen coordinates
        toastView_->invalidate();
    }

    // `sticky` keeps the notification open: used by the drag & drop hint, so
    // the text does not disappear half way through a drag.
    void toast(const std::wstring& text, int kind, bool sticky = false) {
        toastText_ = text;
        toastKind_ = kind;
        toastUntil_ = sticky ? ~0ULL : GetTickCount64() + 3600ULL;
        toastVisible_ = true;
        toastSticky_ = sticky;
        if (toastView_) {
            toastView_->text = text;
            toastView_->kind = kind;
            if (!toastView_->hwnd()) {
                RECT rc{};
                GetClientRect(hwnd_, &rc);
                POINT origin{rc.right - S(390), rc.bottom - S(34) - S(76)};
                ClientToScreen(hwnd_, &origin);
                toastView_->create(hwnd_, 0,
                                   R(origin.x, origin.y, S(370), S(64)), true);
                if (toastView_->hwnd()) {
                    LONG ex = GetWindowLongW(toastView_->hwnd(), GWL_EXSTYLE);
                    SetWindowLongW(toastView_->hwnd(), GWL_EXSTYLE,
                                   ex | WS_EX_NOACTIVATE);
                }
            }
            positionToast();
            toastView_->setVisible(true);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void hideToast() {
        toastVisible_ = false;
        toastSticky_ = false;
        toastText_.clear();
        if (toastView_ && toastView_->hwnd())
            toastView_->setVisible(false);
        // Repaint the widgets that were underneath the popup.
        RECT r = toastClientRect_;
        if (r.right > r.left) {
            for (auto& w : widgets_) {
                if (!w->visible() || !w->hwnd()) continue;
                RECT wr = w->rect();
                RECT overlap{};
                if (IntersectRect(&overlap, &wr, &r)) w->invalidate();
            }
            InvalidateRect(hwnd_, &r, TRUE);
        }
        InvalidateRect(hwnd_, nullptr, TRUE);
    }

    void toggleMaximize() {
        ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
        btnMax_->icon = IsZoomed(hwnd_) ? Ic::Restore : Ic::Max;
        btnMax_->invalidate();
        layout();
    }

    // ---- file / package actions ------------------------------------------
    bool ensureDb() {
        if (!package_.empty() && (!db_ || !util::iequals(db_->path(), package_))) {
            try {
                db_ = std::make_unique<msi::Db>(package_);
            } catch (const util::Error& e) {
                toast(L"无法打开数据库：" + e.message(), 2);
                return false;
            }
        }
        return db_ != nullptr;
    }

    void refreshEmptyState() {
        gridHome_->setData({L"属性", L"值"}, {{L"状态", L"尚未打开包"},
            {L"提示", L"点击「浏览」或直接把 .msi / .msp 拖进窗口"},
            {L"命令行", L"在终端带参数运行即可使用全部 CLI 命令"}});
        for (int i = 0; i < 3; ++i) {
            infoHeaders_[i].clear();
            infoRows_[i].clear();
            infoDetails_[i].clear();
        }
        infoShownDetails_.clear();
        infoTitle_ = L"尚未打开包";
        infoSubtitle_ = L"打开一个 .msi / .msp 文件以查看摘要、属性与媒体信息";
        for (int i = 0; i < 4; ++i) infoStats_[i].clear();
        applyInfoFilter();
        gridTables_->clear();
        gridTableData_->clear();
        gridVerify_->clear();
        tableInfo_ = L"共 0 张表";
        queryInfo_ = L"就绪";
        verifyText_ = L"请先打开一个 .msi / .msp 包";
        statusRight_.clear();
        bExpanded_.clear();
        bSelDirKey_.clear();
        autoOutDir_.clear();
        rebuildBrowse();
    }

    void loadPackage(const std::wstring& path) {
        if (path.empty()) return;
        std::wstring full = util::path::full(path);
        msi::FileProbe p = msi::probe(full);
        if (!p.error.empty()) {
            toast(L"不是有效的安装包：" + p.error, 2);
            return;
        }
        package_ = full;
        tbPackage_->setValue(full);
        db_.reset();
        if (!ensureDb()) return;
        saveSetting(L"LastPackage", full);
        applyDefaultOutDir();
        populate();
        runVerify();
        status_ = L"已打开 " + util::path::fileName(full);
        toast(L"已载入 " + util::path::fileName(full), 3);
        // A package belongs to the read / extract side. The pack page has no
        // "current package" bar, so never leave a freshly opened package
        // invisible behind the pack form.
        if (page_ == PgPack && !running_) switchPage(PgInfo);
        layout();
    }

    void populate() {
        if (!ensureDb()) return;
        // tables -----------------------------------------------------------
        tableNames_ = db_->tableNames();
        tableInfo_ = util::fmt(L"共 %llu 张表",
                               static_cast<unsigned long long>(tableNames_.size()));
        filterTables();
        if (!filteredTables_.empty()) {
            gridTables_->select(0);
            showTableAt(0);
        } else gridTableData_->clear();

        // home -------------------------------------------------------------
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        unsigned long long size = 0;
        if (GetFileAttributesExW(package_.c_str(), GetFileExInfoStandard, &fad))
            size = (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        auto prop = [&](const wchar_t* k) { return db_->property(k); };
        std::vector<extract::MediaInfo> media = extract::readMedia(*db_);
        size_t cabs = 0;
        for (const auto& m : media) if (!m.cabinet.empty()) ++cabs;
        gridHome_->setData({L"属性", L"值"}, {
            {L"文件名", util::path::fileName(package_)},
            {L"包类型", db_->isPatch() ? L"MSP 修补包" : L"MSI 安装数据库"},
            {L"文件大小", humanSize(size)},
            {L"ProductName", prop(L"ProductName")},
            {L"ProductVersion", prop(L"ProductVersion")},
            {L"Manufacturer", prop(L"Manufacturer")},
            {L"ProductCode", prop(L"ProductCode")},
            {L"UpgradeCode", prop(L"UpgradeCode")},
            {L"数据表", std::to_wstring(tableNames_.size()) + L" 张"},
            {L"CAB / 媒体", std::to_wstring(cabs) + L" 个 CAB · " +
                            std::to_wstring(media.size()) + L" 条 Media 记录"},
            {L"路径", package_},
        }, {S(150), S(420)});

        // info page: header facts + three tabbed data sets -----------------
        infoTitle_ = prop(L"ProductName").empty()
            ? util::path::fileName(package_) : prop(L"ProductName");
        infoSubtitle_ = L"版本 " + prop(L"ProductVersion") + L"  ·  " +
                        prop(L"Manufacturer") + L"  ·  " +
                        (db_->isPatch() ? L"MSP 修补包" : L"MSI 安装数据库");
        infoStats_[0] = L"文件大小   " + humanSize(size);
        infoStats_[1] = L"数据表   " + std::to_wstring(tableNames_.size()) + L" 张";
        infoStats_[2] = L"CAB   " + std::to_wstring(cabs) + L" 个";
        infoStats_[3] = L"流   " +
                        std::to_wstring(db_->streamNames().size()) + L" 个";

        infoHeaders_[0] = {L"属性", L"值"};
        infoRows_[0].clear();
        infoDetails_[0].clear();
        for (const auto& x : db_->summary()) {
            infoRows_[0].push_back({x.name, x.value});
            infoDetails_[0].push_back(x.name + L" = " + x.value);
        }

        msi::QueryResult pr = db_->query(L"SELECT * FROM `Property`");
        infoHeaders_[1] = pr.columns.empty()
            ? std::vector<std::wstring>{L"属性", L"值"} : pr.columns;
        infoRows_[1] = pr.rows;
        infoDetails_[1].clear();
        for (const auto& row : pr.rows) {
            std::wstring d = row.empty() ? L"" : row[0];
            if (row.size() > 1) d += L" = " + row[1];
            infoDetails_[1].push_back(d);
        }

        infoHeaders_[2] = {L"DiskId", L"LastSequence", L"Cabinet",
                           L"类型", L"大小", L"状态"};
        infoRows_[2].clear();
        infoDetails_[2].clear();
        for (const auto& x : media) {
            std::wstring type = x.cabinet.empty() ? L"未压缩布局"
                              : x.embedded ? L"内嵌 (#stream)" : L"外部 CAB";
            std::wstring state = x.cabinet.empty() ? L"layout 文件"
                                : x.available ? L"可用" : L"缺失";
            infoRows_[2].push_back({std::to_wstring(x.diskId),
                                    std::to_wstring(x.lastSequence),
                                    x.cabinet.empty() ? L"(none)" : x.cabinet,
                                    type, humanSize(x.size), state});
            std::wstring src = x.embedded ? (L"内嵌流 " + x.streamName)
                             : x.sourcePath.empty() ? L"(无)" : x.sourcePath;
            infoDetails_[2].push_back(
                L"DiskId " + std::to_wstring(x.diskId) + L"  ·  来源：" + src +
                L"  ·  大小 " + humanSize(x.size) +
                L"  ·  " + (x.available ? L"文件可用" : L"文件缺失"));
        }
        applyInfoFilter();

        statusRight_ = util::fmt(L"%llu 张表 · %llu 个 CAB",
                                 static_cast<unsigned long long>(tableNames_.size()),
                                 static_cast<unsigned long long>(cabs));

        rebuildBrowse();
    }

    void setInfoTab(int tab) {
        infoTab_ = tab;
        btnInfoSummary_->accent = (tab == 0);
        btnInfoProps_->accent = (tab == 1);
        btnInfoMedia_->accent = (tab == 2);
        btnInfoSummary_->invalidate();
        btnInfoProps_->invalidate();
        btnInfoMedia_->invalidate();
        applyInfoFilter();
    }

    // Filter the active data set and feed the single full-width grid. The
    // detail strings stay aligned with the filtered rows so selecting a row
    // always shows the correct source even while a filter is active.
    void applyInfoFilter() {
        int tab = infoTab_;
        const auto& headers = infoHeaders_[tab];
        const auto& rows = infoRows_[tab];
        const auto& details = infoDetails_[tab];
        std::wstring filter = util::toLower(util::trim(tbInfoSearch_->value()));
        std::vector<std::vector<std::wstring>> shown;
        infoShownDetails_.clear();
        for (size_t i = 0; i < rows.size(); ++i) {
            bool hit = filter.empty();
            if (!hit) {
                for (const auto& cell : rows[i]) {
                    if (util::toLower(cell).find(filter) != std::wstring::npos) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                shown.push_back(rows[i]);
                infoShownDetails_.push_back(i < details.size() ? details[i] : L"");
            }
        }
        gridInfo_->setData(headers, shown);
        infoCount_ = util::fmt(L"共 %llu 项 · 显示 %llu 项 · 选中行查看完整内容",
                               static_cast<unsigned long long>(rows.size()),
                               static_cast<unsigned long long>(shown.size()));
        infoDetail_.clear();
        layout();
    }

    void updateInfoDetail(int row) {
        if (row < 0 || row >= static_cast<int>(infoShownDetails_.size())) return;
        if (infoShownDetails_[row].empty()) return;
        infoDetail_ = infoShownDetails_[row];
        layout();   // the detail strip appears and the table shrinks a little
    }

    void filterTables() {
        std::wstring filter = util::toLower(util::trim(tbTableSearch_->value()));
        filteredTables_.clear();
        std::vector<std::vector<std::wstring>> rows;
        for (const auto& n : tableNames_) {
            if (!filter.empty() && util::toLower(n).find(filter) == std::wstring::npos)
                continue;
            filteredTables_.push_back(n);
            rows.push_back({n});
        }
        gridTables_->setData({L"表"}, rows, {S(230)});
        tableInfo_ = util::fmt(L"共 %llu 张表 · 显示 %llu 张",
                               static_cast<unsigned long long>(tableNames_.size()),
                               static_cast<unsigned long long>(filteredTables_.size()));
    }

    void showTableAt(int row) {
        if (row < 0 || row >= static_cast<int>(filteredTables_.size())) return;
        if (!ensureDb()) return;
        try {
            msi::QueryResult r = db_->query(L"SELECT * FROM " +
                                            msi::quoteIdent(filteredTables_[row]));
            gridTableData_->setData(r.columns, r.rows);
            status_ = filteredTables_[row] + L" · " +
                      std::to_wstring(r.rows.size()) + L" 行";
        } catch (const util::Error& e) {
            gridTableData_->clear();
            toast(L"读取表失败：" + e.message(), 2);
        }
    }

    void runQuery() {
        std::wstring sql = util::trim(tbSql_->value());
        if (sql.empty()) { toast(L"请输入 SQL 查询", 1); return; }
        if (!ensureDb()) { toast(L"请先打开一个包", 1); return; }
        auto start = std::chrono::steady_clock::now();
        try {
            msi::QueryResult r = db_->query(sql);
            gridQueryResult_->setData(r.columns, r.rows);
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - start).count();
            queryInfo_ = util::fmt(L"%llu 行 · %llu 列 · %lld ms",
                                   static_cast<unsigned long long>(r.rows.size()),
                                   static_cast<unsigned long long>(r.columns.size()),
                                   static_cast<long long>(ms));
            status_ = L"查询完成";
            layout();
        } catch (const util::Error& e) {
            queryInfo_ = L"查询失败";
            toast(L"查询失败：" + e.message(), 2);
            layout();
        }
    }

    void runVerify() {
        if (package_.empty()) {
            verifyOk_ = false;
            verifyText_ = L"请先打开一个 .msi / .msp 包";
            gridVerify_->setData({L"检查项", L"结果"}, {{L"状态", L"未打开包"}});
            layout();
            return;
        }
        std::vector<std::vector<std::wstring>> rows;
        auto add = [&](const wchar_t* k, std::wstring v) {
            rows.push_back({k, std::move(v)});
        };
        msi::FileProbe p = msi::probe(package_);
        add(L"路径", p.path);
        add(L"大小", humanSize(p.size));
        add(L"OLE 复合文档签名", p.signatureOk
            ? L"OK  (D0 CF 11 E0 A1 B1 1A E1)"
            : L"FAIL  (got " + p.signatureHex + L")");
        add(L"包类型", p.isPatch ? L".msp 修补包（patch 打开模式）" : L".msi 数据库（只读）");
        bool ok = p.error.empty();
        std::wstring reason = p.error;
        if (!ok) {
            add(L"预检", L"FAIL - " + p.error);
        } else {
            add(L"预检", L"OK");
            try {
                msi::Db db(package_);
                add(L"MsiOpenDatabase", L"OK");
                if (!p.isPatch) {
                    UINT vr = MsiVerifyPackageW(package_.c_str());
                    add(L"MsiVerifyPackage", vr == ERROR_SUCCESS
                        ? L"OK（可安装的包）" : L"报告 " + util::msiError(vr));
                }
                add(L"_Tables", L"OK - " + std::to_wstring(db.tableNames().size()) + L" 张表");
                add(L"_Streams", std::to_wstring(db.streamNames().size()) + L" 个流");
                std::vector<extract::MediaInfo> media = extract::readMedia(db);
                size_t cabs = 0;
                for (const auto& m : media) if (!m.cabinet.empty()) ++cabs;
                add(L"Media / CAB", std::to_wstring(media.size()) + L" 条媒体记录 · " +
                                    std::to_wstring(cabs) + L" 个 CAB");
                std::wstring title, templ;
                for (const auto& s : db.summary()) {
                    if (s.name == L"Title") title = s.value;
                    if (util::startsWith(s.name, L"Template")) templ = s.value;
                }
                add(L"SummaryInformation", L"OK - Title=\"" + title +
                                           L"\", Template=\"" + templ + L"\"");
            } catch (const util::Error& e) {
                reason = e.message();
                add(L"MsiOpenDatabase / 结构", L"FAIL - " + reason);
                ok = false;
            }
        }
        add(L"结论", ok ? L"有效的 Windows Installer 包"
                        : L"无效：不是可打开的 Windows Installer 包");
        gridVerify_->setData({L"检查项", L"结果"}, rows, {S(210), S(600)});
        verifyOk_ = ok;
        verifyText_ = ok ? L"验证通过 · 这是一个有效的 Windows Installer 包"
                         : L"验证失败 · " + reason;
        status_ = ok ? L"验证通过" : L"验证失败";
        layout();
    }

    // ---- pickers ---------------------------------------------------------
    void browsePackage() {
        COMDLG_FILTERSPEC f[] = {
            {L"Windows Installer 包 (*.msi;*.msp)", L"*.msi;*.msp"},
            {L"所有文件 (*.*)", L"*.*"}};
        std::wstring p;
        if (pickPath(hwnd_, Pick::Open, L"打开 MSI / MSP 包", f, 2, nullptr, p))
            loadPackage(p);
    }

    void browseFolderFor(TextBox* target, const wchar_t* title) {
        std::wstring p;
        if (pickPath(hwnd_, Pick::Folder, title, nullptr, 0, nullptr, p)) {
            target->setValue(p);
            lastOutputDir_ = p;
        }
    }

    void browseFileFor(TextBox* target, bool append, const wchar_t* title,
                       const wchar_t* filterName, const wchar_t* filterSpec) {
        COMDLG_FILTERSPEC f[] = {{filterName, filterSpec},
                                 {L"所有文件 (*.*)", L"*.*"}};
        std::wstring p;
        if (pickPath(hwnd_, Pick::Open, title, f, 2, nullptr, p)) {
            if (append && !target->value().empty())
                target->setValue(target->value() + L";" + p);
            else target->setValue(p);
        }
    }

    void browseSaveMsi() {
        COMDLG_FILTERSPEC f[] = {{L"Windows Installer 包 (*.msi)", L"*.msi"}};
        std::wstring p;
        if (pickPath(hwnd_, Pick::Save, L"保存 MSI", f, 1, L"msi", p))
            tbOutMsi_->setValue(p);
    }

    void suggestPackDefaults() {
        std::wstring src = tbSrcDir_->value();
        if (src.empty()) return;
        if (tbName_->value().empty())
            tbName_->setValue(util::path::fileName(src));
        if (tbOutMsi_->value().empty())
            tbOutMsi_->setValue(util::path::join(util::path::parent(src),
                                                util::path::fileName(src) + L".msi"));
    }

    void openOutputFolder() {
        std::wstring dir = lastOutputDir_;
        if (dir.empty() && !package_.empty()) dir = util::path::dirName(package_);
        if (dir.empty()) { toast(L"还没有输出目录", 1); return; }
        ShellExecuteW(hwnd_, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // ---- background tasks ------------------------------------------------
    void setBusy(bool busy) {
        Button* runButtons[] = {btnExtractAll_, btnExtractSel_, btnExport_, btnPack_,
                                btnOpen_, btnHomeVerify_, btnVerify_};
        for (Button* b : runButtons) if (b) b->setEnabled(!busy);
        if (gridBrowseDirs_) gridBrowseDirs_->setEnabled(!busy);
        if (gridBrowseFiles_) gridBrowseFiles_->setEnabled(!busy);
        if (btnExtractOpen_) btnExtractOpen_->setEnabled(!busy);
        if (progress_) {
            progress_->indeterminate = busy;
            if (!busy) progress_->value = 0;
            progress_->invalidate();
        }
    }

    void startTask(std::function<void()> work, const std::wstring& title,
                   const std::wstring& doneMessage) {
        if (running_) return;
        running_ = true;
        db_.reset();                 // MSI handles are never shared with workers
        log_->clear();
        progress_->phase = 0;
        setBusy(true);
        status_ = title;
        statusRight_ = L"任务运行中…";
        InvalidateRect(hwnd_, nullptr, TRUE);
        HWND h = hwnd_;
        auto begin = std::chrono::steady_clock::now();
        std::thread([h, work = std::move(work), doneMessage, begin]() {
            bool ok = true;
            std::wstring message = doneMessage;
            try {
                work();
            } catch (const util::Error& e) {
                ok = false; message = e.message();
                util::errLine(L"error: " + message);
            } catch (const std::exception& e) {
                ok = false; message = util::widen(e.what());
                util::errLine(L"error: " + message);
            } catch (...) {
                ok = false; message = L"未知错误";
                util::errLine(L"error: " + message);
            }
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - begin).count();
            auto* result = new TaskResult{ok, message, static_cast<unsigned long long>(ms)};
            if (!PostMessageW(h, WM_APP_TASK_DONE, ok ? 1 : 0,
                              reinterpret_cast<LPARAM>(result)))
                delete result;
        }).detach();
    }

    void onTaskDone(WPARAM wp, LPARAM lp) {
        std::unique_ptr<TaskResult> res(reinterpret_cast<TaskResult*>(lp));
        if (!res) return;
        running_ = false;
        setBusy(false);
        statusRight_.clear();
        (void)wp;
        // Refreshing the package views can set its own status text, so do it
        // first and report the task result last.
        if (res->ok && !lastPackOutput_.empty() &&
            util::path::exists(lastPackOutput_)) {
            std::wstring packed = lastPackOutput_;
            lastPackOutput_.clear();
            loadPackage(packed);           // inspect the freshly built MSI
            // The pack page itself no longer shows packages: follow the result
            // to the summary page instead of hiding it behind the form.
            if (res->ok && page_ == PgPack) switchPage(PgInfo);
        } else {
            lastPackOutput_.clear();
            if (!package_.empty()) populate();
        }
        status_ = util::fmt(L"%s · %llu ms",
                            res->ok ? L"任务完成" : L"任务失败", res->elapsedMs);
        log_->append(res->ok ? L"✔ 完成：" + res->message
                             : L"✘ 失败：" + res->message,
                     res->ok ? LogView::Kind::Ok : LogView::Kind::Error);
        toast(res->ok ? res->message : L"失败：" + res->message,
              res->ok ? 3 : 2);
        layout();
    }

    // ---- extract: Explorer browser + selective extraction ----------------
    std::wstring defaultExtractDir() const {
        std::wstring dir = util::path::dirName(package_);
        std::wstring stem = util::path::fileName(package_);
        size_t dot = stem.find_last_of(L'.');
        if (dot != std::wstring::npos) stem = stem.substr(0, dot);
        return util::path::join(dir, stem + L"_files");
    }

    void applyDefaultOutDir() {
        std::wstring cur = util::trim(tbOutDir_->value());
        if (cur.empty() || cur == autoOutDir_) {
            autoOutDir_ = defaultExtractDir();
            tbOutDir_->setValue(autoOutDir_);
        }
    }

    // ---- destination folder selection ----------------------------------
    void loadRecentOutDirs() {
        recentOutDirs_.clear();
        std::wstring all = loadSetting(L"RecentOutDirs");
        size_t start = 0;
        while (start < all.size()) {
            size_t end = all.find(L'\n', start);
            std::wstring part = all.substr(
                start, end == std::wstring::npos ? end : end - start);
            part = util::trim(part);
            if (!part.empty() &&
                std::find(recentOutDirs_.begin(), recentOutDirs_.end(), part) ==
                    recentOutDirs_.end())
                recentOutDirs_.push_back(part);
            if (end == std::wstring::npos) break;
            start = end + 1;
        }
        if (recentOutDirs_.size() > 6) recentOutDirs_.resize(6);
    }

    void saveRecentOutDirs() const {
        std::wstring all;
        for (const auto& d : recentOutDirs_) {
            if (!all.empty()) all += L"\n";
            all += d;
        }
        saveSetting(L"RecentOutDirs", all);
    }

    void addRecentOutDir(const std::wstring& dir) {
        std::wstring full = util::path::full(dir);
        if (full.empty()) return;
        auto it = std::find(recentOutDirs_.begin(), recentOutDirs_.end(), full);
        if (it != recentOutDirs_.end()) recentOutDirs_.erase(it);
        recentOutDirs_.insert(recentOutDirs_.begin(), full);
        if (recentOutDirs_.size() > 6) recentOutDirs_.resize(6);
        saveRecentOutDirs();
    }

    void chooseExtractOutDir() {
        std::wstring current = util::trim(tbOutDir_->value());
        std::wstring start = current.empty()
            ? util::path::dirName(package_) : util::path::full(current);
        std::wstring dir;
        if (pickPath(hwnd_, Pick::Folder, L"选择解包文件的存放目录", nullptr, 0,
                     nullptr, dir, start)) {
            tbOutDir_->setValue(dir);
            lastOutputDir_ = util::path::full(dir);
            addRecentOutDir(dir);
            refreshBrowseList();
            toast(L"解包目录：" + dir, 0);
        }
    }

    void showRecentOutDirs() {
        std::vector<Menu::Item> items;
        for (const auto& d : recentOutDirs_)
            items.push_back({d, Ic::Folder, true, false});
        if (items.empty())
            items.push_back({L"（暂无最近目录，点「浏览…」选择）", Ic::InfoCircle,
                             false, false});
        items.push_back({L"", Ic::None, true, true});
        int browseIndex = static_cast<int>(items.size());
        items.push_back({L"浏览其它目录…", Ic::Folder, true, false});
        RECT br{};
        GetWindowRect(btnRecentDirs_->hwnd(), &br);
        POINT anchor{br.left, br.bottom};
        showMenu(anchor, std::move(items), [this, browseIndex](int cmd) {
            if (cmd >= 0 && cmd < static_cast<int>(recentOutDirs_.size())) {
                tbOutDir_->setValue(recentOutDirs_[cmd]);
                lastOutputDir_ = util::path::full(recentOutDirs_[cmd]);
                refreshBrowseList();
                toast(L"解包目录：" + recentOutDirs_[cmd], 0);
            } else if (cmd == browseIndex) {
                chooseExtractOutDir();
            }
        });
    }

    bool ensureOutputDir() {
        if (util::trim(tbOutDir_->value()).empty()) {
            autoOutDir_ = defaultExtractDir();
            tbOutDir_->setValue(autoOutDir_);
        }
        if (util::trim(tbOutDir_->value()).empty()) {
            toast(L"请选择输出目录", 1);
            return false;
        }
        return true;
    }

    extract::Options extractOptions(std::vector<std::wstring> onlyFiles) {
        extract::Options o;
        o.outDir = util::trim(tbOutDir_->value());
        o.flat = chkFlat_->checked;
        o.dryRun = chkDry_->checked;
        o.skipExisting = chkSkip_->checked;
        o.sourceLayout = chkSource_->checked;
        o.keepTemp = chkKeepTemp_->checked;
        o.verbose = chkVerbose_->checked;
        o.transforms = splitList(tbTransforms_->value());
        o.baseMsi = util::trim(tbBaseMsi_->value());
        o.onlyFiles = std::move(onlyFiles);
        return o;
    }

    void setExtractAdvanced(bool open) {
        extractAdv_ = open;
        btnExtractAdv_->accent = open;
        btnExtractAdv_->invalidate();
        setVisibility();
        layout();
    }

    void setLogOpen(bool open) {
        extractLog_ = open;
        btnExtractLog_->accent = open;
        btnExtractLog_->invalidate();
        setVisibility();
        layout();
    }

    void startExtractKeys(std::vector<std::wstring> keys, const std::wstring& label) {
        if (package_.empty()) { toast(L"请先打开一个包", 1); return; }
        if (running_) return;
        if (!ensureOutputDir()) return;
        extract::Options o = extractOptions(std::move(keys));
        lastOutputDir_ = util::path::full(o.outDir);
        saveSetting(L"LastOutDir", lastOutputDir_);
        addRecentOutDir(o.outDir);
        std::wstring path = package_;
        bool patch = db_ && db_->isPatch();
        startTask([path, patch, o] {
            msi::Db db(path);
            if (!patch) {
                for (const auto& tr : o.transforms) {
                    db.applyTransform(tr);
                    util::verboseLine(o.verbose, L"applied transform " + tr);
                }
            }
            if (patch) extract::extractPatch(db, o);
            else extract::extractPackage(db, o);
        }, label, o.onlyFiles.empty() ? L"解包完成" : L"已解包");
    }

    void startExtractAll() {
        startExtractKeys({}, L"正在解包全部文件…");
    }

    std::vector<std::wstring> keysForRow(int row) const {
        std::vector<std::wstring> keys;
        if (row < 0 || row >= static_cast<int>(bRows_.size())) return keys;
        auto item = bRows_[row];
        if (item.first) {
            std::function<void(int)> walk = [&](int idx) {
                for (const auto& f : bFiles_)
                    if (f.dir == idx) keys.push_back(f.key);
                for (int c : bDirs_[idx].children) walk(c);
            };
            walk(item.second);
        } else {
            keys.push_back(bFiles_[item.second].key);
        }
        return keys;
    }

    void startExtractSelected() {
        int row = gridBrowseFiles_ ? gridBrowseFiles_->selected() : -1;
        auto keys = keysForRow(row);
        if (keys.empty()) {
            toast(L"请先在文件列表中选择一个文件或文件夹", 1);
            return;
        }
        startExtractKeys(std::move(keys), L"正在解包所选内容…");
    }

    void activateBrowseRow(int row) {
        if (row < 0 || row >= static_cast<int>(bRows_.size())) return;
        auto item = bRows_[row];
        if (item.first) {
            openDir(item.second);
        } else {
            const BFile& f = bFiles_[item.second];
            startExtractKeys({f.key}, L"正在解包 " + f.name);
        }
    }

    std::vector<std::wstring> collectDirFiles(int dir) const {
        std::vector<std::wstring> keys;
        std::function<void(int)> walk = [&](int idx) {
            for (const auto& f : bFiles_)
                if (f.dir == idx) keys.push_back(f.key);
            for (int c : bDirs_[idx].children) walk(c);
        };
        if (dir >= 0 && dir < static_cast<int>(bDirs_.size())) walk(dir);
        return keys;
    }

    bool isExpanded(const std::wstring& key) const {
        return std::find(bExpanded_.begin(), bExpanded_.end(), key) != bExpanded_.end();
    }

    void expandPathTo(int dir) {
        for (int i = dir, guard = 0; i >= 0 && i < static_cast<int>(bDirs_.size()) &&
                                     guard < 128;
             i = bDirs_[i].parent, ++guard) {
            if (!isExpanded(bDirs_[i].key)) bExpanded_.push_back(bDirs_[i].key);
        }
    }

    void pushHistory(int dir) {
        if (navPos_ >= 0 && navPos_ < static_cast<int>(navHistory_.size()) &&
            navHistory_[navPos_] == dir)
            return;
        if (navPos_ + 1 < static_cast<int>(navHistory_.size()))
            navHistory_.resize(navPos_ + 1);
        navHistory_.push_back(dir);
        navPos_ = static_cast<int>(navHistory_.size()) - 1;
        if (navHistory_.size() > 64) {
            navHistory_.erase(navHistory_.begin());
            --navPos_;
        }
    }

    void navBack() {
        if (navPos_ > 0) openDir(navHistory_[--navPos_], true);
    }
    void navForward() {
        if (navPos_ + 1 < static_cast<int>(navHistory_.size()))
            openDir(navHistory_[++navPos_], true);
    }
    void navUp() {
        int parent = (bSelDir_ < 0 || bSelDir_ >= static_cast<int>(bDirs_.size()))
                         ? -1 : bDirs_[bSelDir_].parent;
        openDir(parent);
    }

    void openDir(int dir, bool fromHistory = false) {
        if (dir < -1 || dir >= static_cast<int>(bDirs_.size())) return;
        if (dir == bSelDir_) return;
        bSelDir_ = dir;
        bSelDirKey_ = dir < 0 ? L"<root>" : bDirs_[dir].key;
        if (!fromHistory) pushHistory(dir);
        if (dir >= 0) expandPathTo(dir);
        rebuildVisibleDirs();   // highlights the current folder in the tree
        refreshBrowseList();
    }

    void updateBreadcrumb() {
        std::vector<Breadcrumb::Segment> segs;
        segs.push_back({package_.empty() ? L"包内容" : util::path::fileName(package_), -1});
        std::vector<int> chain;
        for (int i = bSelDir_, guard = 0;
             i >= 0 && i < static_cast<int>(bDirs_.size()) && guard < 128;
             i = bDirs_[i].parent, ++guard)
            chain.push_back(i);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            segs.push_back({bDirs_[*it].name, *it});
        crumb_->setSegments(std::move(segs));
    }

    void sortBy(int col) {
        if (sortCol_ == col) sortAsc_ = !sortAsc_;
        else { sortCol_ = col; sortAsc_ = true; }
        refreshBrowseList();
    }

    void rebuildBrowse() {
        bDirs_.clear();
        bFiles_.clear();
        bRoots_.clear();
        bVisible_.clear();
        bRows_.clear();
        bSelDir_ = -1;
        browseStatus_ = L"尚未打开包";
        gridBrowseDirs_->clear();
        gridBrowseFiles_->clear();
        crumb_->setSegments({});
        if (package_.empty() || !ensureDb()) {
            layout();
            return;
        }
        if (db_->isPatch()) {
            browseStatus_ = L"补丁包（.msp）：请使用「解包全部」";
            layout();
            return;
        }
        try {
            extract::BrowseTree t = extract::browse(*db_, chkSource_->checked);
            std::unordered_map<std::wstring, int> byKey;
            bDirs_.reserve(t.dirs.size());
            for (const auto& d : t.dirs) {
                BDir b;
                b.key = d.key;
                b.name = d.name;
                b.parentKey = d.parentKey;
                b.targetRel = d.targetRel;
                byKey[util::toLower(d.key)] = static_cast<int>(bDirs_.size());
                bDirs_.push_back(std::move(b));
            }
            for (int i = 0; i < static_cast<int>(bDirs_.size()); ++i) {
                auto it = byKey.find(util::toLower(bDirs_[i].parentKey));
                if (it != byKey.end() && it->second != i) {
                    bDirs_[i].parent = it->second;
                    bDirs_[it->second].children.push_back(i);
                } else {
                    bDirs_[i].parent = -1;
                    bRoots_.push_back(i);
                }
            }
            auto less = [this](int a, int b) {
                return util::toLower(bDirs_[a].name) < util::toLower(bDirs_[b].name);
            };
            std::sort(bRoots_.begin(), bRoots_.end(), less);
            for (auto& d : bDirs_) std::sort(d.children.begin(), d.children.end(), less);

            bFiles_.reserve(t.files.size());
            for (const auto& f : t.files) {
                BFile b;
                b.key = f.key;
                b.name = f.name;
                b.component = f.component;
                b.relPath = f.relPath;
                b.size = f.size;
                b.sequence = f.sequence;
                auto it = byKey.find(util::toLower(f.dirKey));
                b.dir = it != byKey.end() ? it->second : -1;
                bFiles_.push_back(std::move(b));
            }

            if (bExpanded_.empty())
                for (int r : bRoots_) bExpanded_.push_back(bDirs_[r].key);

            // Restore the folder the user was looking at, or start at the
            // package root just like Explorer opening a drive.
            int target = -1;
            if (!bSelDirKey_.empty() && bSelDirKey_ != L"<root>") {
                auto it = byKey.find(util::toLower(bSelDirKey_));
                if (it != byKey.end()) target = it->second;
            }
            bSelDir_ = target;
            bSelDirKey_ = target < 0 ? L"<root>" : bDirs_[target].key;
            if (target >= 0) expandPathTo(target);
            navHistory_.clear();
            navHistory_.push_back(bSelDir_);
            navPos_ = 0;
            rebuildVisibleDirs();
            refreshBrowseList();
        } catch (const util::Error& e) {
            browseStatus_ = L"无法读取目录结构：" + e.message();
        }
        layout();
    }

    void rebuildVisibleDirs() {
        bVisible_.clear();
        std::function<void(int, int)> walk = [&](int i, int depth) {
            bDirs_[i].depth = depth;
            bVisible_.push_back(i);
            if (isExpanded(bDirs_[i].key))
                for (int c : bDirs_[i].children) walk(c, depth + 1);
        };
        for (int r : bRoots_) walk(r, 0);

        std::vector<std::vector<std::wstring>> rows;
        std::vector<Ic> icons;
        rows.reserve(bVisible_.size());
        for (int i : bVisible_) {
            std::wstring line(static_cast<size_t>(bDirs_[i].depth) * 2, L' ');
            line += bDirs_[i].name;
            rows.push_back({line});
            icons.push_back(bDirs_[i].children.empty()
                                ? Ic::Folder
                                : isExpanded(bDirs_[i].key) ? Ic::ChevronDown
                                                            : Ic::ChevronRight);
        }
        int selPos = -1;
        for (int k = 0; k < static_cast<int>(bVisible_.size()); ++k)
            if (bVisible_[k] == bSelDir_) selPos = k;
        gridBrowseDirs_->setData({L"目录"}, rows, {S(240)}, icons);
        if (selPos >= 0) {
            suppressTreeSel_ = true;
            gridBrowseDirs_->select(selPos);
            suppressTreeSel_ = false;
        }
    }

    void toggleBrowseDir(int row) {
        if (row < 0 || row >= static_cast<int>(bVisible_.size())) return;
        int dir = bVisible_[row];
        if (bDirs_[dir].children.empty()) { openDir(dir); return; }
        const std::wstring key = bDirs_[dir].key;
        auto it = std::find(bExpanded_.begin(), bExpanded_.end(), key);
        if (it == bExpanded_.end()) bExpanded_.push_back(key);
        else bExpanded_.erase(it);
        rebuildVisibleDirs();
    }

    void onBrowseRowSelected(int row) {
        std::wstring base = util::fmt(L"%llu 个项目",
                                      static_cast<unsigned long long>(bRows_.size()));
        if (row >= 0 && row < static_cast<int>(bRows_.size())) {
            auto item = bRows_[row];
            if (item.first)
                base += L"  ·  选中文件夹 " + bDirs_[item.second].name;
            else
                base += L"  ·  选中 " + bFiles_[item.second].name + L" (" +
                        humanSize(bFiles_[item.second].size) + L")";
        }
        browseStatus_ = base;
        layout();   // the status strip is a static label rebuilt by layout()
    }

    void refreshBrowseList() {
        if (!gridBrowseFiles_) return;
        bRows_.clear();
        std::vector<std::vector<std::wstring>> rows;
        std::vector<Ic> icons;
        std::wstring filter = util::toLower(util::trim(tbBrowseFilter_->value()));
        std::wstring out = util::trim(tbOutDir_->value());
        bool flat = chkFlat_->checked;

        std::wstring headers[6] = {L"名称", L"大小", L"类型",
                                   L"组件", L"序号", L"状态"};
        if (sortCol_ >= 0 && sortCol_ < 6)
            headers[sortCol_] += sortAsc_ ? L"  ▲" : L"  ▼";

        auto nameOf = [this](int fileIndex) {
            return util::toLower(bFiles_[fileIndex].name);
        };
        auto cmpFiles = [&](int a, int b) {
            const BFile& x = bFiles_[a];
            const BFile& y = bFiles_[b];
            bool less;
            switch (sortCol_) {
            case 1:  less = x.size < y.size; break;
            case 2:  less = fileKindText(x.name) < fileKindText(y.name); break;
            case 3:  less = util::toLower(x.component) < util::toLower(y.component); break;
            case 4:  less = x.sequence < y.sequence; break;
            default: less = nameOf(a) < nameOf(b); break;
            }
            return sortAsc_ ? less : !less;
        };

        // Folders first, exactly like Explorer's details view.
        std::vector<int> dirs;
        for (int i = 0; i < static_cast<int>(bDirs_.size()); ++i)
            if (bDirs_[i].parent == bSelDir_) dirs.push_back(i);
        std::sort(dirs.begin(), dirs.end(), [this](int a, int b) {
            return util::toLower(bDirs_[a].name) < util::toLower(bDirs_[b].name);
        });
        for (int d : dirs) {
            if (!filter.empty() &&
                util::toLower(bDirs_[d].name).find(filter) == std::wstring::npos)
                continue;
            bRows_.push_back({true, d});
            rows.push_back({bDirs_[d].name, L"", L"文件夹", L"", L"", L""});
            icons.push_back(Ic::Folder);
        }

        std::vector<int> fs;
        for (int i = 0; i < static_cast<int>(bFiles_.size()); ++i)
            if (bFiles_[i].dir == bSelDir_) fs.push_back(i);
        std::stable_sort(fs.begin(), fs.end(), cmpFiles);
        for (int i : fs) {
            const BFile& f = bFiles_[i];
            if (!filter.empty()) {
                std::wstring hay = util::toLower(f.name + L" " + f.component + L" " + f.key);
                if (hay.find(filter) == std::wstring::npos) continue;
            }
            bool done = false;
            if (!out.empty()) {
                std::wstring target = flat ? util::path::join(out, f.name)
                                           : util::path::join(out, f.relPath);
                done = util::path::exists(target);
            }
            rows.push_back({f.name, humanSize(f.size), fileKindText(f.name),
                            f.component, std::to_wstring(f.sequence),
                            done ? L"✓ 已解包" : L"未解包"});
            icons.push_back(Ic::File);
            bRows_.push_back({false, i});
        }

        gridBrowseFiles_->setData({headers[0], headers[1], headers[2],
                                   headers[3], headers[4], headers[5]},
                                  rows,
                                  {S(340), S(90), S(130), S(170), S(70), S(100)},
                                  icons);
        size_t dirCount = 0;
        for (const auto& r : bRows_) if (r.first) ++dirCount;
        browseStatus_ = util::fmt(L"%llu 个项目  ·  %llu 个文件夹  ·  %llu 个文件%s",
                                  static_cast<unsigned long long>(bRows_.size()),
                                  static_cast<unsigned long long>(dirCount),
                                  static_cast<unsigned long long>(bRows_.size() - dirCount),
                                  filter.empty() ? L"" : L"  ·  已筛选");
        updateBreadcrumb();
        layout();
    }

    void showMenu(POINT p, std::vector<Menu::Item> items,
                  std::function<void(int)> picked) {
        if (!menu_) menu_ = std::make_unique<Menu>();
        menu_->show(hwnd_, p, std::move(items), std::move(picked));
    }

    // Right-click in the details view: folder, file, or empty space.
    void showItemMenu(int row, POINT p) {
        if (row < 0 || row >= static_cast<int>(bRows_.size())) {
            showMenu(p, {
                {L"解包全部", Ic::Extract, true, false},
                {L"刷新", Ic::Refresh, true, false},
                {L"显示全部目录", Ic::ChevronDown, true, false},
                {L"", Ic::None, true, true},
                {L"打开输出目录", Ic::Open, true, false},
            }, [this](int cmd) {
                switch (cmd) {
                case 0: startExtractAll(); break;
                case 1: rebuildBrowse(); break;
                case 2:
                    bExpanded_.clear();
                    for (auto& d : bDirs_)
                        if (!d.children.empty()) bExpanded_.push_back(d.key);
                    rebuildVisibleDirs();
                    break;
                case 4: openOutputFolder(); break;
                default: break;
                }
            });
            return;
        }
        auto item = bRows_[row];
        if (item.first) {
            showDirMenuFor(item.second, p);
            return;
        }
        int fi = item.second;
        const BFile f = bFiles_[fi];
        std::wstring out = util::trim(tbOutDir_->value());
        std::wstring target = chkFlat_->checked ? util::path::join(out, f.name)
                                                : util::path::join(out, f.relPath);
        bool extracted = !out.empty() && util::path::exists(target);
        showMenu(p, {
            {L"解包此文件", Ic::Extract, true, false},
            {L"解包到…", Ic::Folder, true, false},
            {L"", Ic::None, true, true},
            {L"复制相对路径", Ic::Copy, true, false},
            {L"复制文件键", Ic::Table, true, false},
            {L"在资源管理器中显示", Ic::Open, extracted, false},
        }, [this, f, target](int cmd) {
            switch (cmd) {
            case 0: startExtractKeys({f.key}, L"正在解包 " + f.name); break;
            case 1: {
                std::wstring current = util::trim(tbOutDir_->value());
                std::wstring start = current.empty()
                    ? util::path::dirName(package_) : util::path::full(current);
                std::wstring dir;
                if (pickPath(hwnd_, Pick::Folder, L"选择解包目录", nullptr, 0,
                             nullptr, dir, start)) {
                    tbOutDir_->setValue(dir);
                    startExtractKeys({f.key}, L"正在解包 " + f.name);
                }
                break;
            }
            case 3:
                setClipboardText(hwnd_, f.relPath);
                toast(L"已复制相对路径", 3);
                break;
            case 4:
                setClipboardText(hwnd_, f.key);
                toast(L"已复制文件键", 3);
                break;
            case 5: {
                if (!util::path::exists(target)) { toast(L"文件尚未解包", 1); break; }
                std::wstring args = L"/select,\"" + target + L"\"";
                ShellExecuteW(hwnd_, L"open", L"explorer.exe", args.c_str(),
                              nullptr, SW_SHOWNORMAL);
                break;
            }
            default: break;
            }
        });
    }

    void showDirMenuFor(int dir, POINT p) {
        if (dir < 0 || dir >= static_cast<int>(bDirs_.size())) return;
        std::wstring name = bDirs_[dir].name;
        std::wstring rel = bDirs_[dir].targetRel;
        showMenu(p, {
            {L"打开", Ic::Open, true, false},
            {L"解包此目录（含子目录）", Ic::Extract, true, false},
            {L"解包全部", Ic::Extract, true, false},
            {L"", Ic::None, true, true},
            {L"复制目录路径", Ic::Copy, true, false},
            {L"全部展开", Ic::ChevronDown, true, false},
            {L"全部折叠", Ic::ChevronRight, true, false},
        }, [this, dir, name, rel](int cmd) {
            switch (cmd) {
            case 0: openDir(dir); break;
            case 1: {
                auto keys = collectDirFiles(dir);
                if (keys.empty()) { toast(L"该目录没有文件", 1); break; }
                startExtractKeys(std::move(keys), L"正在解包目录 " + name);
                break;
            }
            case 2: startExtractAll(); break;
            case 4:
                setClipboardText(hwnd_, rel.empty() ? name : rel);
                toast(L"已复制目录路径", 3);
                break;
            case 5:
                bExpanded_.clear();
                for (auto& d : bDirs_)
                    if (!d.children.empty()) bExpanded_.push_back(d.key);
                rebuildVisibleDirs();
                break;
            case 6:
                bExpanded_.clear();
                for (int r : bRoots_) bExpanded_.push_back(bDirs_[r].key);
                rebuildVisibleDirs();
                break;
            default: break;
            }
        });
    }

    void showDirMenu(int row, POINT p) {
        int dir = (row >= 0 && row < static_cast<int>(bVisible_.size()))
                      ? bVisible_[row] : bSelDir_;
        showDirMenuFor(dir, p);
    }

    void startExport() {
        if (package_.empty()) { toast(L"请先打开一个包", 1); return; }
        std::wstring dir = util::trim(tbExportDir_->value());
        if (dir.empty()) { toast(L"请选择输出目录", 1); return; }
        int mode = ddExportMode_->index;
        lastOutputDir_ = util::path::full(dir);
        std::wstring path = package_;
        startTask([path, dir, mode] {
            msi::Db db(path);
            if (mode == 0) extract::exportCabs(db, dir, true);
            else extract::exportArchive(db, dir, true);
        }, L"正在导出 " + util::path::fileName(path), L"导出完成");
    }

    void startPack() {
        pack::Options o;
        o.inputDir = util::trim(tbSrcDir_->value());
        o.outputMsi = util::trim(tbOutMsi_->value());
        if (o.inputDir.empty() || o.outputMsi.empty()) {
            toast(L"请填写源目录与输出 MSI 路径", 1);
            return;
        }
        o.productName = util::trim(tbName_->value());
        o.version = util::trim(tbVersion_->value());
        if (o.version.empty()) o.version = L"1.0.0";
        o.manufacturer = util::trim(tbManufacturer_->value());
        if (o.manufacturer.empty()) o.manufacturer = L"msitool";
        o.platform = ddPlatform_->index == 1 ? L"x64" : L"Intel";
        o.language = _wtoi(tbLanguage_->value().c_str());
        if (o.language <= 0) o.language = 1033;
        static const wchar_t* uiNames[] = {L"none", L"basic", L"minimal"};
        o.ui = uiNames[std::min(2, std::max(0, ddUi_->index))];
        o.uiSet = true;
        o.largeFiles = ddLargeFiles_->index == 1 ? L"error" : L"layout";
        o.largeFilesSet = true;
        o.cabSize = parseSize(tbCabSize_->value());
        o.cabSizeSet = true;
        o.configFile = util::trim(tbConfig_->value());
        o.signCert = util::trim(tbSignCert_->value());
        o.signCertSet = !o.signCert.empty();
        o.signPassword = tbSignPassword_->value();
        o.signTimestamp = util::trim(tbSignTime_->value());
        o.force = chkForce_->checked;
        o.dryRun = chkPackDry_->checked;
        o.verbose = chkPackVerbose_->checked;
        o.keepEmptyDirs = chkEmptyDirs_->checked;
        lastOutputDir_ = util::path::dirName(util::path::full(o.outputMsi));
        lastPackOutput_ = util::path::full(o.outputMsi);
        startTask([o] { pack::pack(o); },
                  L"正在打包 " + util::path::fileName(o.outputMsi), L"打包完成");
    }

    // ---- drag & drop -----------------------------------------------------
    //
    // RegisterDragDrop() on the frame window also receives drags that hover a
    // child control: OLE walks up the parent chain to find the target, and it
    // keeps telling us the cursor position while the drag moves. The legacy
    // WM_DROPFILES path (kept as a fallback) only reports the landing point.

    // Defined after AppDropTarget below: it has to instantiate that class.
    void installDropTarget();

    void releaseDropTarget() {
        if (dropTarget_) {
            dragCancel();
            RevokeDragDrop(hwnd_);
            dropTarget_->Release();
            dropTarget_ = nullptr;
        }
    }

    // Drop targets of the page (and tab) currently shown, most specific
    // first; the last one always covers the whole window.
    void buildZones() {
        zones_.clear();
        zoneHit_ = -1;
        RECT rc{};
        GetClientRect(hwnd_, &rc);
        auto row = [&](TextBox* tb, std::initializer_list<Button*> extra) {
            RECT r = tb->rect();
            for (Button* b : extra)
                if (b && b->visible()) r.right = std::max(r.right, b->rect().right);
            return r;
        };
        auto field = [&](TextBox* tb, DropKind kind, const wchar_t* name,
                         std::initializer_list<Button*> extra) {
            if (!tb || !tb->visible() || !tb->enabled()) return;
            DropZone z;
            z.core = row(tb, extra);
            z.hit = grow(z.core, S(14), S(8));
            z.kind = kind;
            z.name = name;
            z.glow = tb;
            zones_.push_back(z);
        };
        auto fallback = [&](RECT hit, RECT frame, DropKind kind,
                            const wchar_t* name) {
            DropZone z;
            z.hit = hit;
            z.frame = frame;
            z.kind = kind;
            z.name = name;
            zones_.push_back(z);
        };

        switch (page_) {
        case PgPack:
            field(tbSrcDir_,   DkSourceDir,  L"源目录",  {btnSrcBrowse_});
            field(tbOutMsi_,   DkOutputMsi,  L"输出 MSI", {btnOutMsiBrowse_});
            field(tbConfig_,   DkManifest,   L"配置文件", {btnConfigBrowse_});
            field(tbSignCert_, DkCert,       L"签名证书", {btnSignCertBrowse_});
            fallback(rc, packForm_, DkAuto, L"自动分配");
            break;
        case PgExtract:
            field(tbOutDir_, DkOutDir, L"解包到",
                  {btnOutBrowse_, btnRecentDirs_, btnExtractOpen_});
            fallback(rc, {}, DkAuto, L"自动分配");
            break;
        case PgExport:
            field(tbExportDir_, DkOutDir, L"输出目录", {btnExportBrowse_});
            fallback(rc, {}, DkAuto, L"自动分配");
            break;
        default:
            fallback(rc, {}, DkAuto, L"自动分配");
            break;
        }
    }

    // Field zones win over the window-wide fallback, and when the generous
    // hit rects of two rows overlap (very short rows at a small window size)
    // the field actually closest to the cursor wins.
    int zoneAt(POINT c) const {
        int best = -1, bestDist = 0x7fffffff;
        for (size_t i = 0; i < zones_.size(); ++i) {
            const DropZone& z = zones_[i];
            if (!z.glow || !PtInRect(&z.hit, c)) continue;
            int d = distToRect(z.core, c);
            if (d < bestDist) { bestDist = d; best = static_cast<int>(i); }
        }
        if (best >= 0) return best;
        for (size_t i = 0; i < zones_.size(); ++i)
            if (!zones_[i].glow && PtInRect(&zones_[i].hit, c))
                return static_cast<int>(i);
        return -1;
    }

    POINT toClient(POINTL screen) const {
        POINT c{screen.x, screen.y};
        ScreenToClient(hwnd_, &c);
        return c;
    }

    // We only ever *read* the dropped paths, so COPY is the only effect we
    // may report: returning MOVE or LINK tells the source to delete or
    // relink the original files after the drop.
    DWORD allowedEffect() const {
        return dragPaths_.empty() ? DROPEFFECT_NONE : DROPEFFECT_COPY;
    }

    std::wstring dragHint() const {
        if (zoneHit_ < 0 || zoneHit_ >= static_cast<int>(zones_.size()))
            return L"";
        const DropZone& z = zones_[zoneHit_];
        if (z.kind != DkAuto)
            return util::fmt(L"松开：设为「%s」", z.name.c_str());
        // Nothing but the page itself under the cursor: describe what the
        // payload will become. The pack page never advertises "open package",
        // and every other page states that a folder belongs to packing.
        bool hasDir = false, hasPackage = false, hasManifest = false,
             hasCert = false;
        for (const std::wstring& p : dragPaths_) {
            if (util::path::isDirectory(p)) { hasDir = true; continue; }
            if (extIs(p, {L".msi", L".msp"})) hasPackage = true;
            else if (extIs(p, {L".pfx", L".p12"})) hasCert = true;
            else if (extIs(p, {L".ini", L".cfg", L".conf"})) hasManifest = true;
        }
        if (hasDir)
            return page_ == PgPack ? L"松开：设为「源目录」"
                                   : L"松开：设为「源目录」并切到「打包」页";
        if (hasPackage)
            return page_ == PgPack ? L"松开：打包页不打开安装包"
                                   : L"松开：打开安装包 (.msi / .msp)";
        if (hasCert)
            return page_ == PgPack ? L"松开：设为「签名证书」"
                                   : L"松开：设为「签名证书」并切到「打包」页";
        if (hasManifest)
            return page_ == PgPack ? L"松开：设为「配置文件」"
                                   : L"松开：设为「配置文件」并切到「打包」页";
        return L"松开：按内容分配";
    }

    void glowZones() {
        for (size_t i = 0; i < zones_.size(); ++i) {
            if (zones_[i].glow)
                zones_[i].glow->setDropGlow(dragging_ &&
                                            static_cast<int>(i) == zoneHit_ ? 2 : 1);
        }
    }

    void dragEnter(IDataObject* obj, POINTL pt, DWORD* effect) {
        dragPaths_ = dropPaths(obj);
        buildZones();
        dragging_ = !dragPaths_.empty();
        *effect = allowedEffect();
        if (dragging_) dragMove(pt, effect);
        else toast(L"这里只能拖入文件或文件夹", 1);
    }

    void dragMove(POINTL pt, DWORD* effect) {
        *effect = allowedEffect();
        if (!dragging_) return;
        int hit = zoneAt(toClient(pt));
        if (hit == zoneHit_) return;
        zoneHit_ = hit;
        glowZones();
        std::wstring hint = dragHint();
        if (hint.empty()) { if (toastSticky_) hideToast(); }
        else toast(hint, 0, true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void dragCancel() {
        releaseDrag();
        dragPaths_.clear();
    }

    void dragDrop(IDataObject* obj, POINTL pt, DWORD* effect) {
        std::vector<std::wstring> paths = dropPaths(obj);
        if (paths.empty()) paths = dragPaths_;
        applyDrop(dropKindAt(pt), paths);
        dragCancel();
        if (paths.empty()) { *effect = DROPEFFECT_NONE; return; }
        *effect = DROPEFFECT_COPY;
    }

    // What a landing point means. A field keeps its own meaning; every other
    // spot (page background, caption bar, a row the layout does not claim) is
    // routed by content, on every page. That is what keeps the two workflows
    // apart: a folder is always pack input, a package is always read input.
    DropKind dropKindAt(POINTL pt) const { return dropKindFor(toClient(pt)); }

    DropKind dropKindFor(POINT c) const {
        int hit = zoneAt(c);
        return hit >= 0 ? zones_[hit].kind : DkAuto;
    }

    void releaseDrag() {
        if (!dragging_) return;
        dragging_ = false;
        zoneHit_ = -1;
        glowZones();
        zones_.clear();
        if (toastSticky_) hideToast();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Landing point decides the field; the field then decides what a folder,
    // a package or a manifest means. Content only breaks ties in DkAuto.
    void applyDrop(DropKind kind, const std::vector<std::wstring>& paths) {
        if (paths.empty()) return;
        switch (kind) {
        case DkSourceDir:  dropSource(paths);  break;
        case DkOutputMsi:  dropOutputMsi(paths); break;
        case DkManifest:   dropManifest(paths); break;
        case DkCert:       dropCert(paths);   break;
        case DkOutDir:     dropOutDir(paths); break;
        case DkAuto:       dropAuto(paths);   break;
        default:           dropOpen(paths);   break;
        }
    }

    // "源目录": a dropped folder is used as is; a dropped file (or several
    // items) collapses to the deepest directory that still covers them all.
    void dropSource(const std::vector<std::wstring>& paths) {
        std::wstring dir = commonRoot(paths);
        if (dir.empty()) {
            toast(L"拖放的项不在同一个位置，无法确定源目录", 2);
            return;
        }
        // A folder is pack input, no matter which page received the drop: the
        // pack form is the only place where it becomes visible.
        const bool movedToPack = page_ != PgPack && !running_;
        if (page_ != PgPack) switchPage(PgPack);
        if (packTab_ != 0) setPackTab(0);
        tbSrcDir_->setValue(dir);
        suggestPackDefaults();
        bool oneDir = paths.size() == 1 && util::path::isDirectory(paths[0]);
        logLine(L"[drop] 源目录 = " + dir);
        std::wstring suffix = movedToPack ? L"（已切到「打包」页）" : L"";
        toast(oneDir ? L"源目录：" + dir + suffix
                     : L"源目录：" + dir +
                           L"（打包以目录为单位，含其下全部内容）" + suffix,
              oneDir ? 3 : 1);
    }

    // "输出 MSI": a dropped .msi becomes the target path, a dropped directory
    // becomes the folder a "<product name>.msi" is written into.
    void dropOutputMsi(const std::vector<std::wstring>& paths) {
        const std::wstring& p = paths.front();
        if (packTab_ != 0) setPackTab(0);
        if (util::path::isDirectory(p)) {
            std::wstring name = util::trim(tbName_->value());
            if (name.empty()) {
                std::wstring src = util::trim(tbSrcDir_->value());
                name = src.empty() ? L"package" : util::path::fileName(src);
            }
            std::wstring out = util::path::join(util::path::full(p), name + L".msi");
            tbOutMsi_->setValue(out);
            lastOutputDir_ = util::path::full(p);
            logLine(L"[drop] 输出 = " + out);
            toast(L"输出 MSI：" + out, 3);
            return;
        }
        if (extIs(p, {L".msi"})) {
            std::wstring full = util::path::full(p);
            bool exists = util::path::exists(full);
            tbOutMsi_->setValue(full);
            lastOutputDir_ = util::path::dirName(full);
            logLine(L"[drop] 输出 = " + full);
            toast(L"输出 MSI：" + full +
                      (exists ? L"（勾选「覆盖已存在文件」才能写入）" : L""),
                  exists ? 1 : 3);
            return;
        }
        toast(L"「输出 MSI」需要 .msi 文件，或一个用于存放包的目录", 2);
    }

    // "配置文件": an INI manifest; dropping a directory looks for the
    // well-known manifest names inside it.
    void dropManifest(const std::vector<std::wstring>& paths) {
        static const wchar_t* kNames[] = {L"pack.ini", L"msitool.ini",
                                          L"manifest.ini", L"pack.cfg"};
        std::wstring file;
        for (const std::wstring& p : paths) {
            if (util::path::isDirectory(p)) {
                for (const wchar_t* n : kNames) {
                    std::wstring cand = util::path::join(p, n);
                    if (util::path::exists(cand)) { file = cand; break; }
                }
            } else if (file.empty()) {
                file = p;
            }
            if (!file.empty()) break;
        }
        if (file.empty()) {
            toast(L"没找到清单：拖入 .ini / .cfg / .txt，或含 pack.ini 的目录", 2);
            return;
        }
        std::wstring full = util::path::full(file);
        bool known = extIs(full, {L".ini", L".cfg", L".conf", L".txt"});
        if (page_ != PgPack) switchPage(PgPack);   // the form is pack input
        if (packTab_ != 1) setPackTab(1);
        tbConfig_->setValue(full);
        logLine(L"[drop] 清单 = " + full);
        toast(L"配置文件：" + util::path::fileName(full) +
                  (known ? L"" : L"（清单为 INI 格式）"),
              known ? 3 : 1);
    }

    void dropCert(const std::vector<std::wstring>& paths) {
        for (const std::wstring& p : paths) {
            if (util::path::isDirectory(p)) continue;
            if (!extIs(p, {L".pfx", L".p12"})) continue;
            std::wstring full = util::path::full(p);
            if (page_ != PgPack) switchPage(PgPack);   // the form is pack input
            if (packTab_ != 2) setPackTab(2);
            tbSignCert_->setValue(full);
            logLine(L"[drop] 证书 = " + full);
            toast(L"签名证书：" + util::path::fileName(full), 3);
            return;
        }
        toast(L"请拖入 .pfx / .p12 代码签名证书", 2);
    }

    // Destination folder of the extract / export pages ("解包到" / "输出目录").
    void dropOutDir(const std::vector<std::wstring>& paths) {
        std::wstring dir;
        for (const std::wstring& p : paths)
            if (util::path::isDirectory(p)) { dir = util::path::full(p); break; }
        if (dir.empty()) dir = itemDir(paths.front());
        if (dir.empty()) { toast(L"请拖入一个目录", 1); return; }
        if (page_ == PgExtract) {
            tbOutDir_->setValue(dir);
            refreshBrowseList();
        } else if (page_ == PgExport) {
            tbExportDir_->setValue(dir);
        }
        lastOutputDir_ = dir;
        addRecentOutDir(dir);
        toast(L"输出目录：" + dir, 3);
    }

    // Blank area of any page: the content decides which workflow the drop
    // belongs to. Folders build packages, packages are opened (read side),
    // manifests and certificates fill the pack form. The pack page never
    // opens a package, so a package dropped there cannot leak read-side state
    // into the pack form.
    void dropAuto(const std::vector<std::wstring>& paths) {
        for (const std::wstring& p : paths) {
            if (!util::path::isDirectory(p)) continue;
            dropSource(paths);          // folder = source dir (switches page)
            return;
        }
        const std::wstring& p = paths.front();
        if (extIs(p, {L".pfx", L".p12"})) { dropCert(paths); return; }
        if (extIs(p, {L".ini", L".cfg", L".conf"})) { dropManifest(paths); return; }
        if (extIs(p, {L".msi", L".msp"})) {
            if (page_ == PgPack) {
                toast(L"打包页不打开安装包：这里只接收源目录 / 清单 / 证书；"
                      L"要查看或解包请用「概览」「包信息」「解包」", 1);
                return;
            }
            dropOpen(paths);
            return;
        }
        dropSource(paths);              // a single file = its folder
    }

    void dropOpen(const std::vector<std::wstring>& paths) {
        for (const std::wstring& p : paths)
            if (extIs(p, {L".msi", L".msp"})) { loadPackage(p); return; }
        toast(L"请拖入 .msi / .msp 安装包；文件夹会作为「打包」源目录", 1);
    }

    void logLine(const std::wstring& text) {
        if (log_) log_->append(text, LogView::Kind::Info);
    }

    // ---- window procedure helpers ----------------------------------------
    // Legacy WM_DROPFILES fallback: same routing, but the position only is
    // known after the drop, so there is no hover feedback here.
    void onDrop(HDROP drop) {
        std::vector<std::wstring> paths;
        UINT n = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            UINT need = DragQueryFileW(drop, i, nullptr, 0);
            if (!need) continue;
            std::vector<wchar_t> buf(static_cast<size_t>(need) + 2, L'\0');
            if (DragQueryFileW(drop, i, buf.data(), need + 1))
                paths.emplace_back(buf.data());
        }
        POINT where{};
        DragQueryPoint(drop, &where);      // client coordinates
        DragFinish(drop);
        if (paths.empty()) return;
        buildZones();
        DropKind kind = dropKindFor(where);
        zones_.clear();
        applyDrop(kind, paths);
    }

    void focusNext(HWND current, bool backwards) {
        std::vector<Widget*> list;
        for (auto& w : widgets_) {
            if (!w->visible() || !w->enabled()) continue;
            if (dynamic_cast<NavItem*>(w.get()) || dynamic_cast<Progress*>(w.get()) ||
                dynamic_cast<ToastView*>(w.get()) ||
                w.get() == btnTheme_ || w.get() == btnMin_ ||
                w.get() == btnMax_ || w.get() == btnClose_)
                continue;
            if (w->page() == -1) continue;
            list.push_back(w.get());
        }
        std::sort(list.begin(), list.end(), [](Widget* a, Widget* b) {
            if (a->rect().top != b->rect().top) return a->rect().top < b->rect().top;
            return a->rect().left < b->rect().left;
        });
        if (list.empty()) return;
        int idx = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            auto* tb = dynamic_cast<TextBox*>(list[i]);
            if (list[i]->hwnd() == current || (tb && tb->isEdit(current))) {
                idx = static_cast<int>(i);
                break;
            }
        }
        idx = (idx + (backwards ? -1 : 1) + static_cast<int>(list.size())) %
              static_cast<int>(list.size());
        auto* tb = dynamic_cast<TextBox*>(list[idx]);
        if (tb) tb->focusEdit();
        else list[idx]->takeFocus();
    }

    LRESULT handle(UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
            auto* self = static_cast<App*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            onCreate();
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd_, &ps);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            paint(dc, rc);
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCCALCSIZE: {
            // NEVER let DefWindowProc compute a non-client area. The very
            // first sizing pass arrives with wParam == FALSE; falling through
            // there leaves a standard caption bar until the window is
            // minimized/restored (which recalculates with wParam == TRUE).
            // Maximized windows need a small inset for DWM's invisible
            // resize border so the client area stays on screen.
            if (wp == TRUE && IsZoomed(hwnd_)) {
                auto* p = reinterpret_cast<NCCALCSIZE_PARAMS*>(lp);
                int b = GetSystemMetrics(SM_CXSIZEFRAME) +
                        GetSystemMetrics(SM_CXPADDEDBORDER);
                p->rgrc[0].left += b;
                p->rgrc[0].right -= b;
                p->rgrc[0].top += b;
                p->rgrc[0].bottom -= b;
            }
            return 0;
        }
        case WM_NCHITTEST: {
            LRESULT hit = DefWindowProcW(hwnd_, msg, wp, lp);
            if (hit != HTCLIENT) return hit;
            POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd_, &pt);
            RECT rc{};
            GetClientRect(hwnd_, &rc);
            int edge = S(6);
            if (!IsZoomed(hwnd_)) {
                bool l = pt.x < edge, r = pt.x > rc.right - edge;
                bool t = pt.y < edge, b = pt.y > rc.bottom - edge;
                if (l && t) return HTTOPLEFT;
                if (r && t) return HTTOPRIGHT;
                if (l && b) return HTBOTTOMLEFT;
                if (r && b) return HTBOTTOMRIGHT;
                if (l) return HTLEFT;
                if (r) return HTRIGHT;
                if (t) return HTTOP;
                if (b) return HTBOTTOM;
            }
            int bw = S(46);
            if (pt.y >= 0 && pt.y < S(40) && pt.x >= 0 &&
                pt.x < rc.right - bw * 4)
                return HTCAPTION;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            int minW = S(1160), minH = S(860);
            // On a high-DPI / small display these scaled values can exceed
            // the work area. Restoring a minimized window would then grow it
            // beyond the screen and cover the status bar with the taskbar.
            RECT work{};
            if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) &&
                work.right > work.left && work.bottom > work.top) {
                minW = std::min(minW, static_cast<int>(work.right - work.left) - S(60));
                minH = std::min(minH, static_cast<int>(work.bottom - work.top) - S(60));
            }
            mm->ptMinTrackSize.x = minW;
            mm->ptMinTrackSize.y = minH;
            return 0;
        }
        case WM_MOVE:
            // The toast is an owned top-most popup, not a child: Windows moves
            // the owner but leaves the popup where it was. Re-anchor it on
            // every move (including the moves made by the modal title-bar
            // drag loop) so the notification follows the window.
            if (toastVisible_) positionToast();
            return 0;
        case WM_SIZE:
            btnMax_->icon = IsZoomed(hwnd_) ? Ic::Restore : Ic::Max;
            btnMax_->invalidate();
            if (wp != SIZE_MINIMIZED) layout();
            return 0;
        case WM_DPICHANGED: {
            theme().rebuild(hwnd_, theme().dark);
            for (auto& w : widgets_) w->onThemeChanged();
            applyTheme();
            auto* suggested = reinterpret_cast<RECT*>(lp);
            if (suggested)
                SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            layout();
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (themeMode_ == 2 && lp &&
                wcsstr(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet"))
                applyTheme();
            return 0;
        case WM_TIMER:
            if (wp == kTimer) {
                if (running_) {
                    progress_->phase += 5;
                    progress_->invalidate();
                }
                if (toastVisible_ && !toastSticky_ &&
                    GetTickCount64() >= toastUntil_) hideToast();
            }
            return 0;
        case WM_DROPFILES:
            onDrop(reinterpret_cast<HDROP>(wp));
            return 0;
        case WM_COMMAND:
            if (LOWORD(wp) == kIdOpen) { browsePackage(); return 0; }
            if (LOWORD(wp) == kIdRefresh) {
                if (!package_.empty()) loadPackage(package_);
                return 0;
            }
            break;
        case WM_APP_LOG: {
            std::unique_ptr<std::wstring> line(reinterpret_cast<std::wstring*>(lp));
            if (line) {
                log_->append(*line, classifyLog(wp != 0, *line));
                if (!status_.empty() && running_)
                    status_ = util::trim(util::replaceAll(*line, L"\t", L" "));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        }
        case WM_APP_TASK_DONE:
            onTaskDone(wp, lp);
            return 0;
        case WM_APP_ESC:
            hideToast();
            return 0;
        case WM_APP_REFRESH:
            if (!package_.empty()) loadPackage(package_);
            return 0;
        case WM_APP_OPEN:
            browsePackage();
            return 0;
        case WM_APP_FOCUS_NEXT:
            focusNext(reinterpret_cast<HWND>(wp), lp != 0);
            return 0;
        case WM_CLOSE:
            if (running_) {
                toast(L"任务进行中，请等待完成后再关闭", 1);
                return 0;
            }
            break;
        case WM_DESTROY:
            setLogTarget(nullptr);
            util::setLogSink(nullptr);
            releaseDropTarget();
            KillTimer(hwnd_, kTimer);
            if (accel_) DestroyAcceleratorTable(accel_);
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, msg, wp, lp);
    }
};

// ---------------------------------------------------------------------------
// IDropTarget glue. OLE calls these on our own (STA) thread from inside the
// message loop, so they can drive App directly without any synchronisation.
// ---------------------------------------------------------------------------
class AppDropTarget : public IDropTarget {
public:
    explicit AppDropTarget(App* app) : app_(app) {}
    virtual ~AppDropTarget() = default;   // for `delete this` in Release()

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_IDropTarget) {
            *ppv = static_cast<IDropTarget*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(++refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG left = static_cast<ULONG>(--refs_);
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* data, DWORD, POINTL pt,
                                        DWORD* effect) override {
        app_->dragEnter(data, pt, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt,
                                       DWORD* effect) override {
        app_->dragMove(pt, effect);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        app_->dragCancel();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* data, DWORD, POINTL pt,
                                   DWORD* effect) override {
        app_->dragDrop(data, pt, effect);
        return S_OK;
    }

private:
    App* app_;
    LONG refs_ = 1;   // ours; RegisterDragDrop() adds / RevokeDragDrop drops
};

App* g_app = nullptr;

void App::installDropTarget() {
    if (dropTarget_) return;
    auto* t = new AppDropTarget(this);
    if (FAILED(RegisterDragDrop(hwnd_, t))) { t->Release(); return; }
    dropTarget_ = t;
}

LRESULT CALLBACK App::proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    App* self = reinterpret_cast<App*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        self = static_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
        return DefWindowProcW(h, msg, wp, lp);
    }
    return self ? self->handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
int run(HINSTANCE instance, const std::wstring& initialFile) {
    enableDpiAwareness();
    prepareGuiProcess();
    OleInitialize(nullptr);
    Widget::registerClass(instance);

    // Resolve the DPI before creating the window so that the initial size is
    // already scaled (the fallback is the system DPI of a desktop DC).
    theme().rebuild(nullptr, systemPrefersDark());

    App app;
    g_app = &app;
    if (!app.create(instance, initialFile)) {
        OleUninitialize();
        return 1;
    }
    ShowWindow(app.hwnd_, SW_SHOWNORMAL);
    UpdateWindow(app.hwnd_);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!app.accel_ || !TranslateAcceleratorW(app.hwnd_, app.accel_, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    int rc = static_cast<int>(msg.wParam);
    theme().dispose();
    g_app = nullptr;
    OleUninitialize();
    return rc;
}

}  // namespace ui
