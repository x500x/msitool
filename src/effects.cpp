#include "effects.h"

#include "pack_util.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <map>
#include <set>
#include <vector>

namespace effects {
namespace {

// ---------------------------------------------------------------------------
// Small text helpers
// ---------------------------------------------------------------------------

enum class Enc { Utf8, Utf8Bom, Utf16LeBom };

std::wstring xmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  out += L"&amp;";  break;
        case L'<':  out += L"&lt;";   break;
        case L'>':  out += L"&gt;";   break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        case L'\r': out += L"&#13;"; break;
        case L'\n': out += L"&#10;"; break;
        case L'\t': out += L"&#9;";  break;
        default:    out += c;        break;
        }
    }
    return out;
}

void addAttr(std::wstring& line, const wchar_t* name, const std::wstring& value) {
    if (value.empty()) return;
    line += L" ";
    line += name;
    line += L"=\"";
    line += xmlEscape(value);
    line += L"\"";
}

std::wstring oneLine(std::wstring s) {
    s = util::replaceAll(s, L"\r", L" ");
    s = util::replaceAll(s, L"\n", L" ");
    return s;
}

std::wstring cmdQuote(const std::wstring& s) {
    return L"\"" + util::replaceAll(oneLine(s), L"\"", L"'") + L"\"";
}

std::wstring psQuote(const std::wstring& s) {
    return L"'" + util::replaceAll(oneLine(s), L"'", L"''") + L"'";
}

std::wstring regEscape(const std::wstring& s) {
    std::wstring out;
    for (wchar_t c : s) {
        if (c == L'\\' || c == L'"') out += L'\\';
        out += c;
    }
    return out;
}

bool hasFormatted(const std::wstring& s) {
    size_t a = s.find(L'[');
    return a != std::wstring::npos && s.find(L']', a + 1) != std::wstring::npos;
}

std::wstring toHexBytes(const std::wstring& text) {
    std::wstring out;
    for (wchar_t ch : text) {
        unsigned lo = static_cast<unsigned>(ch) & 0xffu;
        unsigned hi = (static_cast<unsigned>(ch) >> 8) & 0xffu;
        out += util::fmt(L"%02x,%02x,", lo, hi);
    }
    return out;
}

std::wstring normalizeHex(const std::wstring& raw) {
    std::wstring digits;
    for (wchar_t c : raw)
        if (iswxdigit(c)) digits += static_cast<wchar_t>(towlower(c));
    std::wstring out;
    for (size_t i = 0; i + 1 < digits.size(); i += 2) {
        if (!out.empty()) out += L",";
        out += digits.substr(i, 2);
    }
    return out.empty() ? L"" : out;
}

bool looksAbsolute(const std::wstring& s) {
    if (s.size() >= 2 && s[1] == L':') return true;
    return s.size() >= 2 && (s[0] == L'\\' || s[0] == L'/');
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'\\' || a.back() == L'/') return a + b;
    return a + L"\\" + b;
}

// ---------------------------------------------------------------------------
// Table access
// ---------------------------------------------------------------------------

struct Table {
    std::wstring                           name;
    std::vector<std::wstring>              cols;
    std::vector<std::vector<std::wstring>> rows;

    int col(const wchar_t* wanted) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (util::iequals(cols[i], wanted)) return static_cast<int>(i);
        return -1;
    }
    std::wstring get(size_t r, const wchar_t* wanted) const {
        int c = col(wanted);
        if (c < 0 || r >= rows.size() || static_cast<size_t>(c) >= rows[r].size())
            return L"";
        return rows[r][c];
    }
    int getInt(size_t r, const wchar_t* wanted, int deflt = 0) const {
        std::wstring s = util::trim(get(r, wanted));
        return s.empty() ? deflt : _wtoi(s.c_str());
    }
    size_t size() const { return rows.size(); }
};

Table readTable(msi::Db& db, const wchar_t* name) {
    Table t;
    t.name = name;
    bool exists = false;
    try {
        exists = db.tableExists(name);
    } catch (const util::Error&) {
        exists = false;
    }
    if (!exists) return t;
    try {
        msi::QueryResult r = db.query(L"SELECT * FROM " + msi::quoteIdent(name));
        t.cols = r.columns;
        t.rows = r.rows;
    } catch (const util::Error& e) {
        util::warn(L"cannot read table " + std::wstring(name) + L": " + e.message());
    }
    return t;
}

std::wstring dumpTable(const Table& t) {
    if (t.cols.empty()) return L"# " + t.name + L"\r\n(table absent or empty)\r\n";
    std::wstring out = L"# " + t.name + L"\r\n";
    for (size_t i = 0; i < t.cols.size(); ++i) {
        if (i) out += L"\t";
        out += t.cols[i];
    }
    out += L"\r\n";
    for (const auto& row : t.rows) {
        for (size_t i = 0; i < t.cols.size(); ++i) {
            if (i) out += L"\t";
            std::wstring cell = i < row.size() ? row[i] : L"";
            out += util::replaceAll(util::replaceAll(util::replaceAll(cell,
                       L"\t", L"\\t"), L"\r", L"\\r"), L"\n", L"\\n");
        }
        out += L"\r\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// In-memory model
// ---------------------------------------------------------------------------

using Props = std::map<std::wstring, std::wstring>;   // lower(name) -> value

struct RegRow {
    std::wstring id, key, name, value, component;
    int          root = 2;
};
struct SvcRow {
    std::wstring id, name, display, group, deps, account, password, args,
                 component, description;
    int          type = 0x10, start = 3, error = 1;
};
struct CtlRow {
    std::wstring id, name, args, component;
    int          event = 0, wait = 1;
};
struct EnvRow {
    std::wstring id, name, value, component;
};
struct ShortcutRow {
    std::wstring id, dir, name, component, target, args, description,
                 icon, wkdir;
    int          iconIndex = 0;
};
struct CaRow {
    std::wstring action, source, target;
    int          type = 0;
};
struct SeqRow {
    std::wstring action, condition;
    int          sequence = 0;
};
struct DirRow {
    std::wstring key, parent, target;
};

struct Model {
    Props                     props;       // verbatim Property table
    Props                     scriptProps; // props + a resolved INSTALLFOLDER for .cmd/.reg
    std::vector<RegRow>       registry;
    std::vector<SvcRow>       services;
    std::vector<CtlRow>       controls;
    std::vector<EnvRow>       env;
    std::vector<ShortcutRow>  shortcuts;
    std::vector<CaRow>        customActions;
    std::vector<SeqRow>       sequence;
    std::vector<std::pair<std::wstring, std::pair<std::wstring, std::wstring>>>
                              shortcutProps;  // Shortcut_, key, value
    std::map<std::wstring, DirRow> dirs;      // lower(key) -> row
    Table                     tRegistry, tServices, tControls, tEnvironment,
                              tShortcuts, tCustomActions, tSequence, tProperty,
                              tComponent, tFile, tDirectory, tFeature, tIcon;

    std::wstring prop(const wchar_t* name) const {
        auto it = props.find(util::toLower(std::wstring(name)));
        return it == props.end() ? L"" : it->second;
    }
};

std::wstring resolve(const std::wstring& in, const Props& props) {
    std::wstring out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size();) {
        if (in[i] == L'[') {
            size_t j = in.find(L']', i + 1);
            if (j != std::wstring::npos) {
                std::wstring key = in.substr(i + 1, j - i - 1);
                auto it = props.find(util::toLower(key));
                if (it != props.end() && !it->second.empty() &&
                    it->second.find(L'[') == std::wstring::npos &&
                    it->second.find(L']') == std::wstring::npos) {
                    out += it->second;
                    i = j + 1;
                    continue;
                }
            }
        }
        out += in[i++];
    }
    return out;
}

Model readModel(msi::Db& db) {
    Model m;
    m.tProperty       = readTable(db, L"Property");
    m.tRegistry       = readTable(db, L"Registry");
    m.tServices       = readTable(db, L"ServiceInstall");
    m.tControls       = readTable(db, L"ServiceControl");
    m.tEnvironment    = readTable(db, L"Environment");
    m.tShortcuts      = readTable(db, L"Shortcut");
    m.tCustomActions  = readTable(db, L"CustomAction");
    m.tSequence       = readTable(db, L"InstallExecuteSequence");
    m.tComponent      = readTable(db, L"Component");
    m.tFile           = readTable(db, L"File");
    m.tDirectory      = readTable(db, L"Directory");
    m.tFeature        = readTable(db, L"Feature");
    m.tIcon           = readTable(db, L"Icon");

    for (size_t i = 0; i < m.tProperty.size(); ++i) {
        std::wstring k = m.tProperty.get(i, L"Property");
        if (!k.empty()) m.props[util::toLower(k)] = m.tProperty.get(i, L"Value");
    }
    for (size_t i = 0; i < m.tDirectory.size(); ++i) {
        DirRow d;
        d.key = m.tDirectory.get(i, L"Directory");
        d.parent = m.tDirectory.get(i, L"Directory_Parent");
        std::wstring def = m.tDirectory.get(i, L"DefaultDir");
        size_t colon = def.find(L':');
        d.target = util::path::longName(colon == std::wstring::npos ? def
                                                                   : def.substr(0, colon));
        if (!d.key.empty()) m.dirs[util::toLower(d.key)] = d;
    }
    for (size_t i = 0; i < m.tRegistry.size(); ++i) {
        RegRow r;
        r.id        = m.tRegistry.get(i, L"Registry");
        r.root      = m.tRegistry.getInt(i, L"Root", 2);
        r.key       = m.tRegistry.get(i, L"Key");
        r.name      = m.tRegistry.get(i, L"Name");
        r.value     = m.tRegistry.get(i, L"Value");
        r.component = m.tRegistry.get(i, L"Component_");
        m.registry.push_back(std::move(r));
    }
    for (size_t i = 0; i < m.tServices.size(); ++i) {
        SvcRow s;
        s.id          = m.tServices.get(i, L"ServiceInstall");
        s.name        = m.tServices.get(i, L"Name");
        s.display     = m.tServices.get(i, L"DisplayName");
        s.type        = m.tServices.getInt(i, L"ServiceType", 0x10);
        s.start       = m.tServices.getInt(i, L"StartType", 3);
        s.error       = m.tServices.getInt(i, L"ErrorControl", 1);
        s.group       = m.tServices.get(i, L"LoadOrderGroup");
        s.deps        = m.tServices.get(i, L"Dependencies");
        s.account     = m.tServices.get(i, L"StartName");
        s.password    = m.tServices.get(i, L"Password");
        s.args        = m.tServices.get(i, L"Arguments");
        s.component   = m.tServices.get(i, L"Component_");
        s.description = m.tServices.get(i, L"Description");
        m.services.push_back(std::move(s));
    }
    for (size_t i = 0; i < m.tControls.size(); ++i) {
        CtlRow c;
        c.id        = m.tControls.get(i, L"ServiceControl");
        c.name      = m.tControls.get(i, L"Name");
        c.event     = m.tControls.getInt(i, L"Event", 0);
        c.args      = m.tControls.get(i, L"Arguments");
        c.wait      = m.tControls.getInt(i, L"Wait", 1);
        c.component = m.tControls.get(i, L"Component_");
        m.controls.push_back(std::move(c));
    }
    for (size_t i = 0; i < m.tEnvironment.size(); ++i) {
        EnvRow e;
        e.id        = m.tEnvironment.get(i, L"Environment");
        e.name      = m.tEnvironment.get(i, L"Name");
        e.value     = m.tEnvironment.get(i, L"Value");
        e.component = m.tEnvironment.get(i, L"Component_");
        m.env.push_back(std::move(e));
    }
    for (size_t i = 0; i < m.tShortcuts.size(); ++i) {
        ShortcutRow s;
        s.id          = m.tShortcuts.get(i, L"Shortcut");
        s.dir         = m.tShortcuts.get(i, L"Directory_");
        s.name        = util::path::longName(m.tShortcuts.get(i, L"Name"));
        s.component   = m.tShortcuts.get(i, L"Component_");
        s.target      = m.tShortcuts.get(i, L"Target");
        s.args        = m.tShortcuts.get(i, L"Arguments");
        s.description = m.tShortcuts.get(i, L"Description");
        s.icon        = m.tShortcuts.get(i, L"Icon_");
        s.iconIndex   = m.tShortcuts.getInt(i, L"IconIndex", 0);
        s.wkdir       = m.tShortcuts.get(i, L"WkDir");
        m.shortcuts.push_back(std::move(s));
    }
    for (size_t i = 0; i < m.tCustomActions.size(); ++i) {
        CaRow c;
        c.action = m.tCustomActions.get(i, L"Action");
        c.type   = m.tCustomActions.getInt(i, L"Type", 0);
        c.source = m.tCustomActions.get(i, L"Source");
        c.target = m.tCustomActions.get(i, L"Target");
        m.customActions.push_back(std::move(c));
    }
    for (size_t i = 0; i < m.tSequence.size(); ++i) {
        SeqRow s;
        s.action    = m.tSequence.get(i, L"Action");
        s.sequence  = m.tSequence.getInt(i, L"Sequence", 0);
        s.condition = m.tSequence.get(i, L"Condition");
        m.sequence.push_back(std::move(s));
    }
    Table sp = readTable(db, L"MsiShortcutProperty");
    for (size_t i = 0; i < sp.size(); ++i)
        m.shortcutProps.push_back({sp.get(i, L"Shortcut_"),
                                  {sp.get(i, L"PropertyKey"),
                                   sp.get(i, L"PropVariantValue")}});
    return m;
}

// ---------------------------------------------------------------------------
// Directory / file path reconstruction
// ---------------------------------------------------------------------------

bool isStandardFolder(const std::wstring& k) {
    static const std::set<std::wstring> names = {
        L"targetdir", L"programfilesfolder", L"programfiles64folder",
        L"commonfilesfolder", L"commonfiles64folder", L"programmenufolder",
        L"startmenufolder", L"startupfolder", L"desktopfolder",
        L"appdatafolder", L"localappdatafolder", L"commonappdatafolder",
        L"personalfolder", L"sendtofolder", L"templatesfolder",
        L"favoritesfolder", L"fontsfolder", L"mypicturesfolder",
        L"admintoolsfolder", L"tempfolder", L"windowsfolder",
        L"windowsvolume", L"systemfolder", L"system64folder"
    };
    return names.count(util::toLower(k)) != 0;
}

std::wstring standardCmd(const std::wstring& k) {
    std::wstring n = util::toLower(k);
    if (n == L"programfilesfolder" || n == L"programfiles64folder") return L"%ProgramFiles%";
    if (n == L"commonfilesfolder" || n == L"commonfiles64folder") return L"%CommonProgramFiles%";
    if (n == L"desktopfolder") return L"%USERPROFILE%\\Desktop";
    if (n == L"programmenufolder") return L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs";
    if (n == L"startmenufolder") return L"%APPDATA%\\Microsoft\\Windows\\Start Menu";
    if (n == L"startupfolder") return L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Startup";
    if (n == L"sendtofolder") return L"%APPDATA%\\Microsoft\\Windows\\SendTo";
    if (n == L"templatesfolder") return L"%APPDATA%\\Microsoft\\Windows\\Templates";
    if (n == L"favoritesfolder") return L"%USERPROFILE%\\Favorites";
    if (n == L"mypicturesfolder") return L"%USERPROFILE%\\Pictures";
    if (n == L"appdatafolder") return L"%APPDATA%";
    if (n == L"localappdatafolder") return L"%LOCALAPPDATA%";
    if (n == L"commonappdatafolder") return L"%ALLUSERSPROFILE%";
    if (n == L"personalfolder") return L"%USERPROFILE%\\Documents";
    if (n == L"admintoolsfolder") return L"%APPDATA%\\Microsoft\\Windows\\Start Menu\\Programs\\Administrative Tools";
    if (n == L"fontsfolder") return L"%WINDIR%\\Fonts";
    if (n == L"tempfolder") return L"%TEMP%";
    if (n == L"windowsfolder") return L"%WINDIR%";
    if (n == L"systemfolder" || n == L"system64folder") return L"%WINDIR%\\System32";
    if (n == L"windowsvolume") return L"%SystemDrive%";
    return L"%SystemDrive%";   // TARGETDIR
}

std::wstring standardPs(const std::wstring& k) {
    std::wstring n = util::toLower(k);
    if (n == L"programfilesfolder" || n == L"programfiles64folder") return L"$env:ProgramFiles";
    if (n == L"commonfilesfolder" || n == L"commonfiles64folder") return L"$env:CommonProgramFiles";
    if (n == L"desktopfolder") return L"[Environment]::GetFolderPath('Desktop')";
    if (n == L"programmenufolder") return L"[Environment]::GetFolderPath('Programs')";
    if (n == L"startmenufolder") return L"[Environment]::GetFolderPath('StartMenu')";
    if (n == L"startupfolder") return L"[Environment]::GetFolderPath('Startup')";
    if (n == L"sendtofolder") return L"[Environment]::GetFolderPath('SendTo')";
    if (n == L"templatesfolder") return L"[Environment]::GetFolderPath('Templates')";
    if (n == L"favoritesfolder") return L"[Environment]::GetFolderPath('Favorites')";
    if (n == L"mypicturesfolder") return L"[Environment]::GetFolderPath('MyPictures')";
    if (n == L"appdatafolder") return L"$env:APPDATA";
    if (n == L"localappdatafolder") return L"$env:LOCALAPPDATA";
    if (n == L"commonappdatafolder") return L"$env:ProgramData";
    if (n == L"personalfolder") return L"[Environment]::GetFolderPath('MyDocuments')";
    if (n == L"admintoolsfolder") return L"[Environment]::GetFolderPath('AdminTools')";
    if (n == L"fontsfolder") return L"(Join-Path $env:WINDIR 'Fonts')";
    if (n == L"tempfolder") return L"$env:TEMP";
    if (n == L"windowsfolder") return L"$env:WINDIR";
    if (n == L"systemfolder" || n == L"system64folder") return L"(Join-Path $env:WINDIR 'System32')";
    if (n == L"windowsvolume" || n == L"targetdir") return L"$env:SystemDrive";
    return L"$InstallFolder";
}

// A Directory key expressed as a standard-folder rooted path, which is what
// pack's <shortcut directory="..."> accepts (e.g. ProgramMenuFolder\Vendor\App).
std::wstring manifestDirExpr(const std::wstring& key, const Model& m) {
    std::vector<std::wstring> segs;
    std::wstring cur = key;
    for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
        if (isStandardFolder(cur)) {
            std::reverse(segs.begin(), segs.end());
            std::wstring out = cur;
            for (const auto& s : segs) out = joinPath(out, s);
            return out;
        }
        if (util::iequals(cur, L"INSTALLFOLDER")) {
            std::reverse(segs.begin(), segs.end());
            std::wstring out = L"INSTALLFOLDER";
            for (const auto& s : segs) out = joinPath(out, s);
            return out;
        }
        auto it = m.dirs.find(util::toLower(cur));
        if (it == m.dirs.end()) return key;
        if (!it->second.target.empty() && it->second.target != L".")
            segs.push_back(it->second.target);
        cur = it->second.parent;
    }
    return key;
}

// Returns a command-shell / PowerShell expression for a Directory key.
std::wstring dirExpr(const std::wstring& key, const Model& m, bool powershell) {
    std::vector<std::wstring> segs;
    std::wstring base, cur = key;
    for (int depth = 0; depth < 32 && !cur.empty(); ++depth) {
        std::wstring low = util::toLower(cur);
        if (low == L"targetdir") { base = powershell ? L"$env:SystemDrive" : L"%SystemDrive%"; break; }
        if (low == L"installfolder") {
            std::wstring pinned = resolve(m.prop(L"INSTALLFOLDER"), m.props);
            if (looksAbsolute(pinned)) { base = pinned; break; }
        }
        if (isStandardFolder(cur)) {
            base = powershell ? standardPs(cur) : standardCmd(cur);
            break;
        }
        auto it = m.dirs.find(low);
        if (it == m.dirs.end()) {
            base = powershell ? L"$InstallFolder" : L"%INSTALLFOLDER%";
            break;
        }
        if (!it->second.target.empty() && it->second.target != L".")
            segs.push_back(it->second.target);
        cur = it->second.parent;
        if (cur.empty()) {
            base = powershell ? L"$InstallFolder" : L"%INSTALLFOLDER%";
            break;
        }
    }
    if (base.empty()) base = powershell ? L"$InstallFolder" : L"%INSTALLFOLDER%";
    std::reverse(segs.begin(), segs.end());
    std::wstring tail;
    for (const auto& s : segs) tail = joinPath(tail, s);
    if (tail.empty()) return base;
    if (powershell) return L"(Join-Path " + base + L" " + psQuote(tail) + L")";
    return joinPath(base, tail);
}

// PowerShell expression for a Shortcut.Target. `[#file]` is resolved through
// the File / Component / Directory tables; everything else stays a literal.
std::wstring shortcutTargetExpr(const std::wstring& raw, const Model& m) {
    std::wstring t = util::trim(raw);
    if (t.size() > 3 && t.front() == L'[' && (t[1] == L'#' || t[1] == L'!') &&
        t.back() == L']') {
        std::wstring key = t.substr(2, t.size() - 3);
        for (size_t i = 0; i < m.tFile.size(); ++i) {
            if (!util::iequals(m.tFile.get(i, L"File"), key)) continue;
            std::wstring comp = m.tFile.get(i, L"Component_");
            std::wstring dir;
            for (size_t j = 0; j < m.tComponent.size(); ++j)
                if (util::iequals(m.tComponent.get(j, L"Component"), comp)) {
                    dir = m.tComponent.get(j, L"Directory_");
                    break;
                }
            return L"(Join-Path " + dirExpr(dir, m, true) + L" " +
                   psQuote(util::path::longName(m.tFile.get(i, L"FileName"))) + L")";
        }
        return psQuote(t);
    }
    return psQuote(resolve(t, m.props));
}

// The service executable is the key file of ServiceInstall.Component_.
std::wstring serviceImage(const std::wstring& component, const Model& m) {
    std::wstring keyPath, dir;
    for (size_t i = 0; i < m.tComponent.size(); ++i)
        if (util::iequals(m.tComponent.get(i, L"Component"), component)) {
            keyPath = m.tComponent.get(i, L"KeyPath");
            dir = m.tComponent.get(i, L"Directory_");
            break;
        }
    std::wstring fileKey, fileName;
    for (size_t i = 0; i < m.tFile.size(); ++i) {
        const std::wstring fk = m.tFile.get(i, L"File");
        if (!keyPath.empty() && util::iequals(fk, keyPath)) {
            fileKey = fk;
            fileName = util::path::longName(m.tFile.get(i, L"FileName"));
            break;
        }
        if (keyPath.empty() && util::iequals(m.tFile.get(i, L"Component_"), component)) {
            fileKey = fk;
            fileName = util::path::longName(m.tFile.get(i, L"FileName"));
            break;
        }
    }
    if (fileName.empty()) return L"[#" + component + L"]";
    (void)fileKey;
    return resolve(joinPath(dirExpr(dir, m, false), fileName), m.scriptProps);
}

// ---------------------------------------------------------------------------
// Script generation
// ---------------------------------------------------------------------------

std::wstring hiveFor(int root) {
    switch (root) {
    case 0:  return L"HKEY_CLASSES_ROOT";
    case 1:  return L"HKEY_CURRENT_USER";
    case 3:  return L"HKEY_USERS";
    case -1: return L"HKEY_LOCAL_MACHINE";   // ALLUSERS-dependent
    default: return L"HKEY_LOCAL_MACHINE";
    }
}

std::wstring regValueLine(const std::wstring& name, const std::wstring& raw,
                          const Props& props) {
    std::wstring lhs = name.empty() ? L"@"
                                    : L"\"" + regEscape(name) + L"\"";
    if (raw.size() >= 2 && raw[0] == L'#' && (raw[1] == L'x' || raw[1] == L'X'))
        return lhs + L"=hex:" + normalizeHex(raw.substr(2));
    if (raw.size() >= 2 && raw[0] == L'#' && raw[1] == L'%')
        return lhs + L"=hex(2):" + toHexBytes(resolve(raw.substr(2), props)) + L"00,00";
    if (!raw.empty() && raw[0] == L'#')
        return lhs + L"=dword:" +
               util::fmt(L"%08x", static_cast<unsigned>(_wtoi(raw.substr(1).c_str())));
    std::wstring value = resolve(raw, props);
    if (raw.find(L"[~]") != std::wstring::npos) {
        std::wstring body = resolve(util::replaceAll(raw, L"[~]", L"\u0001"), props);
        std::wstring hex;
        size_t start = 0;
        while (true) {
            size_t p = body.find(L'\u0001', start);
            std::wstring part = body.substr(start, p == std::wstring::npos
                                                  ? std::wstring::npos : p - start);
            hex += toHexBytes(part) + L"00,00,";
            if (p == std::wstring::npos) break;
            start = p + 1;
        }
        return lhs + L"=hex(7):" + hex + L"00,00";
    }
    if (value.find(L'\r') != std::wstring::npos ||
        value.find(L'\n') != std::wstring::npos)
        return lhs + L"=hex(1):" + toHexBytes(value) + L"00,00";
    return lhs + L"=\"" + regEscape(value) + L"\"";
}

std::wstring buildRegFile(const std::vector<RegRow>& rows, const Props& props,
                          bool uninstall) {
    std::wstring out = L"Windows Registry Editor Version 5.00\r\n"
                       L"; Generated by msitool effects from the Registry table.\r\n"
                       L"; UTF-16 LE. Review formatted values before importing.\r\n\r\n";
    for (const auto& r : rows) {
        std::wstring full = joinPath(hiveFor(r.root), r.key);
        if (r.root == -1)
            out += L"; Root=-1: MSI decides HKLM / HKCU from ALLUSERS at install time.\r\n";
        if (hasFormatted(r.value))
            out += L"; MSI formatted text (resolved from Property table where possible): " +
                   oneLine(r.value) + L"\r\n";
        // MSI: a Name of "+/-/*" operates on the key itself; a value name
        // prefixed with '-' deletes that value on install.
        const bool deleteValue = r.name.size() > 1 && r.name[0] == L'-';
        if (uninstall) {
            if (r.name == L"*") out += L"[-" + full + L"]\r\n\r\n";
            else if (r.name == L"+" || r.name == L"-" || deleteValue) continue;
            else {
                out += L"[" + full + L"]\r\n";
                out += (r.name.empty() ? L"@" : L"\"" + regEscape(r.name) + L"\"") +
                       L"=-\r\n\r\n";
            }
            continue;
        }
        if (r.name == L"+")      { out += L"[" + full + L"]\r\n\r\n"; continue; }
        if (r.name == L"-")      { out += L"[-" + full + L"]\r\n\r\n"; continue; }
        if (r.name == L"*")      { out += L"[" + full + L"]\r\n\r\n"; continue; }
        if (deleteValue) {
            out += L"[" + full + L"]\r\n";
            out += L"\"" + regEscape(r.name.substr(1)) + L"\"=-\r\n\r\n";
            continue;
        }
        out += L"[" + full + L"]\r\n";
        out += regValueLine(r.name, r.value, props) + L"\r\n\r\n";
    }
    return out;
}

std::wstring svcType(int t) {
    if (t & 0x1) return L"kernel";
    if (t & 0x2) return L"filesys";
    if (t & 0x100) return L"interact";
    if (t & 0x20) return L"share";
    return L"own";
}
std::wstring svcStart(int s) {
    switch (s) {
    case 0: return L"boot";
    case 1: return L"system";
    case 2: return L"auto";
    case 4: return L"disabled";
    default: return L"demand";
    }
}
std::wstring svcError(int e) {
    switch (e) {
    case 0: return L"ignore";
    case 2: return L"severe";
    case 3: return L"critical";
    default: return L"normal";
    }
}

std::wstring buildServicesScript(const Model& m, bool uninstall) {
    std::wstring out = L"@echo off\r\nchcp 65001 >nul\r\n"
        L"rem Generated by msitool effects from ServiceInstall / ServiceControl.\r\n"
        L"rem Review placeholders and run from an elevated shell.\r\n\r\n";
    if (!uninstall) {
        for (const auto& s : m.services) {
            std::wstring image = serviceImage(s.component, m);
            std::wstring binPath = image;
            if (!s.args.empty()) binPath += L" " + resolve(s.args, m.scriptProps);
            out += L"rem ServiceInstall row: " + oneLine(s.id) + L"\r\n";
            if (hasFormatted(image) || hasFormatted(s.args))
                out += L"rem MSI formatted text kept verbatim: " + oneLine(image) +
                       L" " + oneLine(s.args) + L"\r\n";
            std::wstring line = L"sc create " + cmdQuote(s.name) +
                L" binPath= " + cmdQuote(binPath) +
                L" type= " + svcType(s.type) +
                L" start= " + svcStart(s.start) +
                L" error= " + svcError(s.error);
            if (!s.display.empty()) line += L" DisplayName= " + cmdQuote(resolve(s.display, m.scriptProps));
            if (!s.group.empty()) line += L" group= " + cmdQuote(resolve(s.group, m.scriptProps));
            if (!s.deps.empty()) line += L" depend= " + cmdQuote(resolve(s.deps, m.scriptProps));
            if (!s.account.empty()) line += L" obj= " + cmdQuote(resolve(s.account, m.scriptProps));
            if (!s.password.empty()) line += L" password= " + cmdQuote(s.password);
            out += line + L"\r\n";
            if (!s.description.empty())
                out += L"sc description " + cmdQuote(s.name) + L" " +
                       cmdQuote(resolve(s.description, m.scriptProps)) + L"\r\n";
            out += L"\r\n";
        }
    }
    // ServiceControl events: install-side bits 1/2/8, uninstall-side 16/32/128.
    for (const auto& c : m.controls) {
        int mask = uninstall ? (c.event & 0xB0) : (c.event & 0x0B);
        if (!mask) continue;
        out += L"rem ServiceControl row: " + oneLine(c.id) +
               L" (Event=" + std::to_wstring(c.event) +
               L", Wait=" + std::to_wstring(c.wait) + L")\r\n";
        if (!c.args.empty())
            out += L"rem Arguments: " + oneLine(c.args) + L"\r\n";
        if (mask & 0x02) out += L"sc stop " + cmdQuote(c.name) + L"\r\n";
        if (mask & 0x20) out += L"sc stop " + cmdQuote(c.name) + L"\r\n";
        if (mask & 0x01) out += L"sc start " + cmdQuote(c.name) + L"\r\n";
        if (mask & 0x10) out += L"sc start " + cmdQuote(c.name) + L"\r\n";
        if (mask & 0x08) out += L"sc delete " + cmdQuote(c.name) + L"\r\n";
        if (mask & 0x80) out += L"sc delete " + cmdQuote(c.name) + L"\r\n";
        out += L"\r\n";
    }
    return out;
}

struct EnvParsed {
    std::wstring action;   // set / create / remove / append / prepend
    std::wstring name;
    std::wstring value;
    std::wstring separator = L";";
    bool         system = false;
};

EnvParsed parseEnv(const EnvRow& r) {
    EnvParsed p;
    std::wstring n = r.name;
    if (n.size() >= 2 && (n.compare(0, 2, L"=-") == 0 ||
                          n.compare(0, 2, L"+-") == 0 ||
                          n.compare(0, 2, L"!-") == 0)) {
        std::wstring prefix = n.substr(0, 2);
        n = n.substr(2);
        p.action = prefix == L"+-" ? L"create" : prefix == L"!-" ? L"remove" : L"set";
    }
    if (!n.empty() && n[0] == L'*') { p.system = true; n = n.substr(1); }
    p.name = n;
    std::wstring v = r.value;
    size_t tilde = v.find(L"[~]");
    if (tilde != std::wstring::npos) {
        if (tilde == 0) {
            p.action = L"append";
            std::wstring rest = v.substr(3);
            if (!rest.empty() && (rest[0] == L';' || rest[0] == L':')) {
                p.separator = rest.substr(0, 1);
                rest = rest.substr(1);
            }
            p.value = rest;
        } else {
            p.action = L"prepend";
            std::wstring head = v.substr(0, tilde);
            if (!head.empty() && (head.back() == L';' || head.back() == L':')) {
                p.separator = head.substr(head.size() - 1);
                head.pop_back();
            }
            p.value = head;
        }
    } else {
        p.value = v;
    }
    return p;
}

std::wstring buildEnvironmentScript(const Model& m) {
    std::wstring out = L"@echo off\r\nchcp 65001 >nul\r\n"
        L"rem Generated by msitool effects from the Environment table.\r\n"
        L"rem Install-time effects only: MSI removes these values on uninstall.\r\n\r\n";
    for (const auto& r : m.env) {
        EnvParsed p = parseEnv(r);
        if (p.name.empty()) continue;
        std::wstring val = resolve(p.value, m.scriptProps);
        out += L"rem Environment row: " + oneLine(r.id) + L" (" +
               p.action + (p.system ? L", machine" : L", user") + L")\r\n";
        if (hasFormatted(p.value))
            out += L"rem MSI formatted text kept verbatim: " + oneLine(p.value) + L"\r\n";
        const std::wstring flag = p.system ? L" /M" : L"";
        if (p.action == L"remove") {
            std::wstring hive = p.system
                ? L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Environment"
                : L"HKCU\\Environment";
            out += L"reg delete " + cmdQuote(hive) + L" /v " + cmdQuote(p.name) +
                   L" /f\r\n\r\n";
            continue;
        }
        std::wstring expanded;
        if (p.action == L"append")
            expanded = L"%" + p.name + L"%" + p.separator + val;
        else if (p.action == L"prepend")
            expanded = val + p.separator + L"%" + p.name + L"%";
        else
            expanded = val;
        if (p.action == L"create")
            out += L"rem MSI only creates this value when it does not exist yet.\r\n";
        out += L"setx " + cmdQuote(p.name) + L" " + cmdQuote(expanded) + flag + L"\r\n\r\n";
    }
    return out;
}

std::wstring buildShortcutsScript(const Model& m,
                                  const std::map<std::wstring, std::wstring>& iconFiles) {
    std::wstring out =
        L"# Generated by msitool effects from the Shortcut table.\r\n"
        L"# UTF-8 with BOM; run with PowerShell. Review every path first.\r\n"
        L"$InstallFolder = '<set INSTALLFOLDER here>'   # MSI install root\r\n"
        L"$ws = New-Object -ComObject WScript.Shell\r\n\r\n";
    for (const auto& s : m.shortcuts) {
        if (s.target.empty()) continue;
        bool advertised = true;
        for (wchar_t c : s.target) if (c == L'[' || c == L'#' || c == L'\\') advertised = false;
        if (advertised) {
            out += L"# " + oneLine(s.id) + L": advertised shortcut (Target is feature " +
                   oneLine(s.target) + L"); not reproducible with a .lnk script.\r\n\r\n";
            continue;
        }
        std::wstring dir = dirExpr(s.dir, m, true);
        std::wstring lnk = L"(Join-Path " + dir + L" " + psQuote(s.name) + L")";
        out += L"# " + oneLine(s.id) + L"  Directory_=" + oneLine(s.dir) + L"\r\n";
        out += L"$lnk = $ws.CreateShortcut(" + lnk + L")\r\n";
        out += L"$lnk.TargetPath = " + shortcutTargetExpr(s.target, m) + L"\r\n";
        if (!s.args.empty())
            out += L"$lnk.Arguments = " + psQuote(resolve(s.args, m.props)) + L"\r\n";
        if (!s.wkdir.empty())
            out += L"$lnk.WorkingDirectory = " + dirExpr(s.wkdir, m, true) + L"\r\n";
        if (!s.description.empty())
            out += L"$lnk.Description = " + psQuote(resolve(s.description, m.props)) + L"\r\n";
        auto it = iconFiles.find(s.icon);
        if (!s.icon.empty() && it != iconFiles.end())
            out += L"$lnk.IconLocation = " +
                   psQuote(it->second + L"," + std::to_wstring(s.iconIndex)) + L"\r\n";
        out += L"$lnk.Save()\r\n\r\n";
    }
    return out;
}

std::wstring buildPropertyDump(const Model& m) {
    return dumpTable(m.tProperty);
}

std::wstring buildCustomActionDump(const Model& m) {
    std::wstring out =
        L"# CustomAction and InstallExecuteSequence rows are shown for audit only.\r\n"
        L"# msitool never executes custom actions; the commands below are NOT run.\r\n\r\n";
    out += dumpTable(m.tCustomActions);
    if (!m.customActions.empty()) {
        out += L"\r\n# InstallExecuteSequence rows for custom actions\r\n";
        std::set<std::wstring> ids;
        for (const auto& c : m.customActions) ids.insert(util::toLower(c.action));
        for (const auto& s : m.sequence)
            if (ids.count(util::toLower(s.action)))
                out += s.action + L"\t" + std::to_wstring(s.sequence) + L"\t" +
                       oneLine(s.condition) + L"\r\n";
    }
    return out;
}

// ---------------------------------------------------------------------------
// pack.xml (round-trip manifest)
// ---------------------------------------------------------------------------

std::wstring xmlTag(const std::wstring& tag,
                    const std::vector<std::pair<std::wstring, std::wstring>>& attrs,
                    const std::wstring& indent = L"  ", bool close = true) {
    std::wstring line = indent + L"<" + tag;
    for (const auto& a : attrs) addAttr(line, a.first.c_str(), a.second);
    return line + (close ? L"/>\r\n" : L">\r\n");
}

std::wstring regTypeAndValue(const RegRow& r, std::wstring& value) {
    value = r.value;
    if (r.name == L"+") return L"key";
    if (r.name == L"-") return L"keydelete";
    if (r.name == L"*") return L"keycreatedelete";
    if (value.size() >= 2 && value[0] == L'#' &&
        (value[1] == L'x' || value[1] == L'X')) { value = value.substr(2); return L"binary"; }
    if (value.size() >= 2 && value[0] == L'#' && value[1] == L'%') {
        value = value.substr(2); return L"expand";
    }
    if (!value.empty() && value[0] == L'#') { value = value.substr(1); return L"integer"; }
    if (value.find(L"[~]") != std::wstring::npos) return L"multi";
    return L"string";
}

std::wstring buildManifest(msi::Db& db, const Model& m,
                           const std::map<std::wstring, std::wstring>& iconFiles,
                           const std::map<std::wstring, std::wstring>& binaryRels,
                           std::vector<std::wstring>& warnings) {
    // SummaryInformation Template is "platform;language".
    std::wstring platform = L"Intel";
    for (const auto& s : db.summary())
        if (s.id == 7 && util::toLower(s.value).find(L"x64") != std::wstring::npos)
            platform = L"x64";
    std::wstring language = m.prop(L"ProductLanguage");
    if (language.empty()) language = L"1033";
    std::wstring scope = m.prop(L"ALLUSERS").empty() ? L"perUser" : L"perMachine";

    std::wstring out = L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
    out += L"<!-- Generated by `msitool effects`. Use this file as the pack\r\n";
    out += L"     config manifest: msitool pack <payload-dir> <out.msi> plus the\r\n";
    out += L"     config option pointing at this file (see effects.md).\r\n";
    out += L"     Component_ values refer to the source package; pack falls back to\r\n";
    out += L"     the root component when an id does not exist in the new tree.\r\n";
    out += L"     UI, features, files and transforms are intentionally not replayed. -->\r\n";
    out += L"<msitool xmlns=\"https://msitool.dev/schema/pack/v1\" minVersion=\"1\">\r\n";

    out += xmlTag(L"pack", {
        {L"name", m.prop(L"ProductName")},
        {L"version", m.prop(L"ProductVersion")},
        {L"manufacturer", m.prop(L"Manufacturer")},
        {L"productCode", m.prop(L"ProductCode")},
        {L"upgradeCode", m.prop(L"UpgradeCode")},
        {L"platform", platform},
        {L"language", language},
        {L"scope", scope},
        {L"ui", L"none"}});

    if (!m.registry.empty()) {
        out += L"\r\n  <registry>\r\n";
        for (const auto& r : m.registry) {
            // pack generates these keypath rows itself for every shortcut
            // subdirectory; re-emitting them would clash on the primary key.
            if (util::iequals(r.key, L"Software\\msitool\\ShortcutFolders") &&
                util::startsWith(util::toLower(r.name), L"regshortcutdir")) {
                warnings.push_back(L"internal shortcut-folder registry row '" + r.id +
                    L"' is regenerated by pack; skipped in pack.xml");
                continue;
            }
            std::wstring value, type = regTypeAndValue(r, value);
            std::wstring root = r.root == 0 ? L"HKCR" : r.root == 1 ? L"HKCU"
                              : r.root == 3 ? L"HKU" : r.root == -1 ? L"auto" : L"HKLM";
            std::wstring name = r.name;
            if (name == L"+" || name == L"-" || name == L"*") name.clear();
            std::wstring line = L"    <registryValue";
            addAttr(line, L"id", r.id);
            addAttr(line, L"root", root);
            addAttr(line, L"key", r.key);
            addAttr(line, L"name", name);
            addAttr(line, L"value", value);
            addAttr(line, L"type", type);
            addAttr(line, L"component", r.component);
            out += line + L"/>\r\n";
        }
        out += L"  </registry>\r\n";
    }
    if (!m.services.empty()) {
        out += L"\r\n  <services>\r\n";
        for (const auto& s : m.services) {
            std::wstring type = (s.type & 0x20) ? L"share"
                              : (s.type & 0x100) ? L"interactive" : L"own";
            std::wstring start = s.start == 2 ? L"auto" : s.start == 4 ? L"disabled"
                               : s.start == 1 ? L"system" : L"demand";
            std::wstring error = s.error == 0 ? L"ignore" : s.error == 2 ? L"severe"
                               : s.error == 3 ? L"critical" : L"normal";
            std::wstring line = L"    <service";
            addAttr(line, L"id", s.id);
            addAttr(line, L"name", s.name);
            addAttr(line, L"displayName", s.display);
            addAttr(line, L"type", type);
            addAttr(line, L"start", start);
            addAttr(line, L"error", error);
            addAttr(line, L"account", s.account);
            addAttr(line, L"password", s.password);
            addAttr(line, L"arguments", s.args);
            addAttr(line, L"dependencies", s.deps);
            addAttr(line, L"description", s.description);
            addAttr(line, L"component", s.component);
            out += line + L"/>\r\n";
        }
        out += L"  </services>\r\n";
        if (!m.controls.empty())
            out += L"  <!-- Source ServiceControl events (pack normalises them to Event=163): "
                   L"see services-install.cmd / services-uninstall.cmd. -->\r\n";
    }
    if (!m.env.empty()) {
        out += L"\r\n  <environment>\r\n";
        for (const auto& r : m.env) {
            EnvParsed p = parseEnv(r);
            std::wstring line = L"    <variable";
            addAttr(line, L"id", r.id);
            addAttr(line, L"name", p.name);
            addAttr(line, L"value", p.value);
            addAttr(line, L"action", p.action);
            addAttr(line, L"separator", p.separator);
            addAttr(line, L"system", p.system ? L"yes" : L"no");
            addAttr(line, L"component", r.component);
            out += line + L"/>\r\n";
        }
        out += L"  </environment>\r\n";
    }
    if (!m.shortcuts.empty()) {
        out += L"\r\n  <shortcuts>\r\n";
        for (const auto& s : m.shortcuts) {
            if (s.target.empty()) {
                warnings.push_back(L"shortcut '" + s.id + L"' has no Target; skipped in pack.xml");
                continue;
            }
            bool advertised = true;
            for (wchar_t c : s.target)
                if (c == L'[' || c == L'#' || c == L'\\') advertised = false;
            if (advertised) {
                warnings.push_back(L"advertised shortcut '" + s.id +
                    L"' has a feature as Target; skipped in pack.xml");
                continue;
            }
            std::wstring line = L"    <shortcut";
            addAttr(line, L"id", s.id);
            addAttr(line, L"name", s.name);
            addAttr(line, L"directory", manifestDirExpr(s.dir, m));
            addAttr(line, L"target", s.target);
            addAttr(line, L"arguments", s.args);
            addAttr(line, L"description", s.description);
            auto iconIt = iconFiles.find(s.icon);
            // Icons are exported next to the manifest; pack resolves absolute
            // paths as-is, so the reference stays usable.
            if (!s.icon.empty() && iconIt != iconFiles.end())
                addAttr(line, L"icon", iconIt->second);
            if (s.iconIndex) addAttr(line, L"iconIndex", std::to_wstring(s.iconIndex));
            addAttr(line, L"workingDirectory", s.wkdir);
            addAttr(line, L"component", s.component);
            bool hasProps = false;
            for (const auto& p : m.shortcutProps)
                if (util::iequals(p.first, s.id)) { hasProps = true; break; }
            if (hasProps) {
                out += line + L">\r\n";
                for (const auto& p : m.shortcutProps)
                    if (util::iequals(p.first, s.id)) {
                        std::wstring sub = L"      <msishortcutproperty";
                        addAttr(sub, L"key", p.second.first);
                        addAttr(sub, L"value", p.second.second);
                        out += sub + L"/>\r\n";
                    }
                out += L"    </shortcut>\r\n";
            } else {
                out += line + L"/>\r\n";
            }
        }
        out += L"  </shortcuts>\r\n";
    }
    if (!m.customActions.empty()) {
        out += L"\r\n  <customactions>\r\n";
        std::vector<std::wstring> kept;
        for (const auto& c : m.customActions) {
            if (c.type <= 0 || c.type > 0x7fff) {
                warnings.push_back(L"custom action '" + c.action +
                    L"' has an out-of-range type; skipped in pack.xml");
                continue;
            }
            const int srcKind = c.type & 0x30;
            const int execType = c.type & 0x0f;
            std::wstring line = L"    <customaction";
            addAttr(line, L"id", c.action);
            addAttr(line, L"type", std::to_wstring(c.type));
            if (srcKind == 0x00) {
                // Source is a Binary stream exported next to the manifest.
                auto it = binaryRels.find(c.source);
                if (it == binaryRels.end()) {
                    warnings.push_back(L"custom action '" + c.action +
                        L"' needs Binary '" + c.source + L"' which could not be exported; "
                        L"skipped in pack.xml");
                    continue;
                }
                addAttr(line, L"binary", it->second);
                if (execType == 1 && !c.target.empty()) addAttr(line, L"entry", c.target);
                else addAttr(line, L"target", c.target);
            } else if (srcKind == 0x10) {
                addAttr(line, L"file", c.source);
                addAttr(line, L"target", c.target);
            } else if (srcKind == 0x20) {
                addAttr(line, L"directory", c.source);
                if (execType == 3) addAttr(line, L"value", c.target);
                else addAttr(line, L"target", c.target);
            } else {
                addAttr(line, L"property", c.source);
                if (execType == 3) addAttr(line, L"value", c.target);
                else addAttr(line, L"target", c.target);
            }
            out += line + L"/>\r\n";
            kept.push_back(util::toLower(c.action));
        }
        out += L"  </customactions>\r\n";
        std::wstring seq;
        for (const auto& s : m.sequence) {
            if (std::find(kept.begin(), kept.end(), util::toLower(s.action)) == kept.end())
                continue;   // standard actions are already inserted by pack
            std::wstring line = L"    <action";
            addAttr(line, L"name", s.action);
            addAttr(line, L"value", std::to_wstring(s.sequence));
            addAttr(line, L"condition", s.condition);
            seq += line + L"/>\r\n";
        }
        if (!seq.empty()) out += L"\r\n  <executeSequence>\r\n" + seq +
                                L"  </executeSequence>\r\n";
    }
    out += L"</msitool>\r\n";
    return out;
}

// ---------------------------------------------------------------------------
// Driver
// ---------------------------------------------------------------------------

std::wstring buildReport(msi::Db& db, const Model& m, const Stats& st,
                         const Options& opt, const std::wstring& reportPath,
                         const std::vector<std::wstring>& warnings) {
    const wchar_t* fmtName = opt.format == Format::All ? L"all"
                           : opt.format == Format::Scripts ? L"scripts" : L"manifest";
    std::wstring out = L"# 安装效果导出报告 (`msitool effects`)\r\n\r\n";
    out += L"- 包: `" + db.path() + L"`\r\n";
    out += L"- 产品: " + m.prop(L"ProductName") + L" " +
           m.prop(L"ProductVersion") + L" (" + m.prop(L"Manufacturer") + L")\r\n";
    out += L"- 导出格式: " + std::wstring(fmtName) + L"\r\n";
    out += L"- 统计: 注册表 " + std::to_wstring(st.registry) +
           L" / 服务 " + std::to_wstring(st.services) +
           L" / 服务控制 " + std::to_wstring(st.serviceControls) +
           L" / 环境变量 " + std::to_wstring(st.environment) +
           L" / 快捷方式 " + std::to_wstring(st.shortcuts) +
           L" / 自定义动作 " + std::to_wstring(st.customActions) +
           L" / 属性 " + std::to_wstring(st.properties) + L"\r\n\r\n";
    out += L"## 生成的文件\r\n\r\n";
    for (const auto& p : st.outputs) out += L"- `" + p + L"`\r\n";
    out += L"- `" + reportPath + L"`（本文件）\r\n\r\n";
    out += L"## 文件说明\r\n\r\n";
    out += L"| 文件 | 内容 |\r\n| --- | --- |\r\n";
    out += L"| `registry.reg` | 安装侧注册表写入，UTF-16 LE + BOM，可直接由 regedit 导入 |\r\n";
    out += L"| `registry-uninstall.reg` | 卸载侧删除（普通值写 `\"Name\"=-`，`*` 键写 `[-Key]`） |\r\n";
    out += L"| `services-install.cmd` / `services-uninstall.cmd` | `sc create` / `sc start` / `sc stop` / `sc delete` / `sc description` |\r\n";
    out += L"| `environment.cmd` | `setx` / `reg delete`，`[~]` 追加写成 `%NAME%;value` |\r\n";
    out += L"| `shortcuts.ps1` | PowerShell + WScript.Shell 创建 .lnk |\r\n";
    out += L"| `custom-actions.txt`、`tables.txt`、`properties.txt` | 原始表数据与属性，供人工核对 |\r\n";
    out += L"| `pack.xml` | 可回填的打包清单；`icons/` 与 `binaries/` 是它引用的资源 |\r\n\r\n";
    out += L"## 回填打包\r\n\r\n";
    out += L"```bat\r\n";
    out += L"msitool pack <payload-dir> <out.msi> --config \"" +
           util::path::join(util::path::dirName(reportPath), L"pack.xml") + L"\"\r\n";
    out += L"```\r\n\r\n";
    out += L"## 限制\r\n\r\n";
    out += L"- 工具只读 MSI 表，**不执行安装、不求值条件、不运行 CustomAction**。\r\n";
    out += L"- `[Property]` 只在 Property 表里有字面值时替换；`[#file]`、`[~]`、条件表达式等保持原样并写入注释。\r\n";
    out += L"- 服务的可执行路径由 `Component.KeyPath` / `File` / `Directory` 推导，找不到时写 `[#Component]` 占位符。\r\n";
    out += L"- `pack.xml` 不重放 UI、Feature、文件树、Transform 与签名；`ServiceControl` 事件在 pack 里归一化为 163，原始事件见两个 .cmd。\r\n";
    out += L"- `Component_` 是源包的组件键；新包里找不到时 pack 会回退到根组件。\r\n";
    out += L"- 脚本是**审计/复现辅助**，不是逐字节等价的安装器；导入前请人工审阅。\r\n";
    if (!warnings.empty()) {
        out += L"\r\n## 警告\r\n\r\n";
        for (const auto& x : warnings) out += L"- " + oneLine(x) + L"\r\n";
    }
    return out;
}

struct Writer {
    const Options& opt;
    Stats&         stats;
    std::wstring   dir;

    std::wstring text(const std::wstring& rel, const std::wstring& content, Enc enc) {
        std::wstring path = util::path::join(dir, rel);
        std::vector<unsigned char> bytes;
        if (enc == Enc::Utf16LeBom) {
            bytes.push_back(0xFF);
            bytes.push_back(0xFE);
            for (wchar_t ch : content) {
                bytes.push_back(static_cast<unsigned char>(ch & 0xff));
                bytes.push_back(static_cast<unsigned char>((ch >> 8) & 0xff));
            }
        } else {
            if (enc == Enc::Utf8Bom) { bytes.push_back(0xEF); bytes.push_back(0xBB); bytes.push_back(0xBF); }
            std::string s = util::narrow(content, CP_UTF8);
            bytes.insert(bytes.end(), s.begin(), s.end());
        }
        if (!util::path::writeFile(path, bytes))
            util::fail(L"Cannot write " + path);
        ++stats.files;
        stats.outputs.push_back(path);
        util::verboseLine(opt.verbose, L"wrote " + path);
        return path;
    }
};

}  // namespace

Format parseFormat(const std::wstring& text) {
    std::wstring t = util::toLower(util::trim(text));
    if (t.empty() || t == L"all" || t == L"both") return Format::All;
    if (t == L"scripts" || t == L"script" || t == L"cmd") return Format::Scripts;
    if (t == L"manifest" || t == L"xml" || t == L"config" || t == L"pack.xml")
        return Format::Manifest;
    util::fail(L"Unknown --format '" + text + L"' (use all / scripts / manifest)");
}

Stats exportEffects(msi::Db& db, const Options& opt) {
    const std::wstring outDir = util::path::full(opt.outDir);
    if (!util::path::makeDirs(outDir))
        util::fail(L"Cannot create output directory: " + outDir);

    Stats stats;
    Writer w{opt, stats, outDir};
    Model m = readModel(db);
    // .cmd / .reg scripts want a usable install root; the XML manifest keeps
    // the original formatted [INSTALLFOLDER] text for pack to evaluate.
    m.scriptProps = m.props;
    if (!m.scriptProps.count(L"installfolder")) {
        std::wstring root = dirExpr(L"INSTALLFOLDER", m, false);
        if (!root.empty() && root.back() != L'\\') root += L'\\';
        m.scriptProps[L"installfolder"] = root;
    }
    stats.registry        = m.registry.size();
    stats.services        = m.services.size();
    stats.serviceControls = m.controls.size();
    stats.environment     = m.env.size();
    stats.shortcuts       = m.shortcuts.size();
    stats.customActions   = m.customActions.size();
    stats.properties      = m.props.size();
    std::vector<std::wstring> warnings;

    // Icons and custom-action binaries are shared resources: the PowerShell
    // shortcut script and pack.xml both reference the files written here.
    std::map<std::wstring, std::wstring> iconFiles, binaryRels;
    std::set<std::wstring> seenIcons, seenBinaries;
    auto exportStream = [&](const wchar_t* table, const std::wstring& key,
                            const std::wstring& rel) {
        std::wstring path = util::path::join(outDir, rel);
        try {
            std::wstring sql = L"SELECT `Data` FROM " + msi::quoteIdent(table) +
                               L" WHERE `Name`=?";
            auto data = db.readStream(sql, {key}, 1);
            if (data.empty()) return false;
            if (!util::path::writeFile(path, data)) return false;
            ++stats.files;
            stats.outputs.push_back(path);
            util::verboseLine(opt.verbose, L"wrote " + path);
            return true;
        } catch (const util::Error&) {
            return false;
        }
    };
    for (const auto& s : m.shortcuts) {
        if (s.icon.empty() || !seenIcons.insert(s.icon).second) continue;
        std::wstring rel = joinPath(L"icons",
            util::path::sanitizeComponent(s.icon) + L".ico");
        if (exportStream(L"Icon", s.icon, rel))
            iconFiles[s.icon] = util::path::join(outDir, rel);
        else
            warnings.push_back(L"cannot export icon '" + s.icon + L"'");
    }
    for (const auto& c : m.customActions) {
        if ((c.type & 0x30) != 0x00 || c.source.empty() ||
            !seenBinaries.insert(c.source).second) continue;
        std::wstring rel = joinPath(L"binaries",
            util::path::sanitizeComponent(c.source) + L".bin");
        if (exportStream(L"Binary", c.source, rel))
            binaryRels[c.source] = rel;
        else
            warnings.push_back(L"cannot export custom action binary '" + c.source + L"'");
    }

    if (opt.format != Format::Manifest) {
        w.text(L"registry.reg", buildRegFile(m.registry, m.scriptProps, false),
               Enc::Utf16LeBom);
        w.text(L"registry-uninstall.reg", buildRegFile(m.registry, m.scriptProps, true),
               Enc::Utf16LeBom);
        w.text(L"services-install.cmd", buildServicesScript(m, false), Enc::Utf8);
        w.text(L"services-uninstall.cmd", buildServicesScript(m, true), Enc::Utf8);
        w.text(L"environment.cmd", buildEnvironmentScript(m), Enc::Utf8);
        w.text(L"shortcuts.ps1", buildShortcutsScript(m, iconFiles), Enc::Utf8Bom);
        w.text(L"custom-actions.txt", buildCustomActionDump(m), Enc::Utf8);
        w.text(L"properties.txt", buildPropertyDump(m), Enc::Utf8);
        std::wstring raw =
            dumpTable(m.tRegistry) + L"\r\n" + dumpTable(m.tServices) + L"\r\n" +
            dumpTable(m.tControls) + L"\r\n" + dumpTable(m.tEnvironment) + L"\r\n" +
            dumpTable(m.tShortcuts) + L"\r\n" + dumpTable(m.tCustomActions);
        w.text(L"tables.txt", raw, Enc::Utf8);
    }
    if (opt.format != Format::Scripts)
        w.text(L"pack.xml", buildManifest(db, m, iconFiles, binaryRels, warnings),
               Enc::Utf8Bom);

    const std::wstring reportPath = util::path::join(outDir, L"effects.md");
    w.text(L"effects.md", buildReport(db, m, stats, opt, reportPath, warnings),
           Enc::Utf8);

    for (const auto& x : warnings) util::errLine(L"[warn] " + x);
    util::outLine(util::fmt(
        L"Exported %llu registry value(s), %llu service(s), %llu service control(s), "
        L"%llu environment variable(s), %llu shortcut(s), %llu custom action(s) into %s",
        static_cast<unsigned long long>(stats.registry),
        static_cast<unsigned long long>(stats.services),
        static_cast<unsigned long long>(stats.serviceControls),
        static_cast<unsigned long long>(stats.environment),
        static_cast<unsigned long long>(stats.shortcuts),
        static_cast<unsigned long long>(stats.customActions),
        outDir.c_str()));
    for (const auto& p : stats.outputs) util::outLine(L"  " + p);
    util::outLine(L"Output: " + outDir);
    return stats;
}

}  // namespace effects
