#include "pack_util.h"

#include "msidb.h"

#include <objbase.h>

#include <algorithm>
#include <cwchar>

namespace pack {

// ---------------------------------------------------------------------------
// MSI row helpers
// ---------------------------------------------------------------------------

Cell S(std::wstring v) { Cell c; c.isNull = false; c.s = std::move(v); return c; }
Cell I(int v)          { Cell c; c.isNull = false; c.isInt = true; c.i = v; return c; }
Cell Null()            { return Cell{}; }

void execSql(MSIHANDLE db, const std::wstring& sql) {
    msi::Handle view;
    util::checkMsi(MsiDatabaseOpenViewW(db, sql.c_str(), view.out()), L"SQL: " + sql);
    util::checkMsi(MsiViewExecute(view.get(), 0), L"SQL: " + sql);
}

void insertRow(MSIHANDLE db, const std::wstring& table,
               const std::vector<std::wstring>& cols,
               const std::vector<Cell>& cells) {
    if (cols.size() != cells.size()) util::fail(L"insertRow: column/value mismatch");
    std::wstring sql = L"INSERT INTO " + msi::quoteIdent(table) + L" (";
    for (size_t i = 0; i < cols.size(); ++i)
        sql += (i ? L", " : L"") + msi::quoteIdent(cols[i]);
    sql += L") VALUES (";
    for (size_t i = 0; i < cols.size(); ++i) sql += (i ? L", ?" : L"?");
    sql += L")";

    msi::Handle view, rec(MsiCreateRecord(static_cast<UINT>(cells.size())));
    if (!rec) util::fail(L"MsiCreateRecord failed");
    util::checkMsi(MsiDatabaseOpenViewW(db, sql.c_str(), view.out()),
                   L"INSERT into " + table);
    for (size_t i = 0; i < cells.size(); ++i) {
        if (cells[i].isNull) continue;
        UINT rc = cells[i].isInt
                      ? MsiRecordSetInteger(rec.get(), static_cast<UINT>(i + 1), cells[i].i)
                      : MsiRecordSetStringW(rec.get(), static_cast<UINT>(i + 1),
                                            cells[i].s.c_str());
        util::checkMsi(rc, L"setting value " + cols[i] + L" in " + table);
    }
    util::checkMsi(MsiViewExecute(view.get(), rec.get()),
                   L"INSERT into " + table + L" row " +
                       (cells.empty() ? L"" : cells[0].s));
}

void insertStreamRow(MSIHANDLE db, const std::wstring& table,
                     const std::wstring& nameCol, const std::wstring& dataCol,
                     const std::wstring& name, const std::wstring& file) {
    msi::Handle view, rec(MsiCreateRecord(2));
    if (!rec) util::fail(L"MsiCreateRecord failed");
    std::wstring sql = L"SELECT " + msi::quoteIdent(nameCol) + L", " +
                       msi::quoteIdent(dataCol) + L" FROM " + msi::quoteIdent(table);
    util::checkMsi(MsiDatabaseOpenViewW(db, sql.c_str(), view.out()),
                   L"open " + table + L" view");
    util::checkMsi(MsiViewExecute(view.get(), 0), L"execute " + table + L" view");
    util::checkMsi(MsiRecordSetStringW(rec.get(), 1, name.c_str()),
                   L"set " + table + L" name");
    util::checkMsi(MsiRecordSetStreamW(rec.get(), 2, file.c_str()),
                   L"set " + table + L" stream " + name);
    util::checkMsi(MsiViewModify(view.get(), MSIMODIFY_INSERT, rec.get()),
                   L"insert " + table + L" stream " + name);
}

// ---------------------------------------------------------------------------
// Text / identifier helpers
// ---------------------------------------------------------------------------

std::wstring newGuid() {
    GUID g{};
    if (FAILED(CoCreateGuid(&g))) util::fail(L"CoCreateGuid failed");
    wchar_t buf[64] = {};
    if (StringFromGUID2(g, buf, 64) <= 0) util::fail(L"StringFromGUID2 failed");
    return std::wstring(buf);
}

std::wstring normalizeGuid(std::wstring s) {
    s = util::trim(s);
    if (s.empty()) return newGuid();
    if (s.front() != L'{') s.insert(s.begin(), L'{');
    if (s.back() != L'}') s.push_back(L'}');
    return s;
}

std::wstring toIdentifier(std::wstring s, size_t maxLen) {
    for (wchar_t& c : s) {
        bool ok = (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
                  (c >= L'0' && c <= L'9') || c == L'_' || c == L'.';
        if (!ok) c = L'_';
    }
    if (!s.empty() && s[0] >= L'0' && s[0] <= L'9') s.insert(s.begin(), L'_');
    if (s.empty()) s = L"_";
    if (s.size() > maxLen) s.resize(maxLen);
    return s;
}

std::wstring sanitizeName(std::wstring name) {
    // MSI identifiers for directory/file names must not contain path syntax
    // or the short|long separator.
    for (wchar_t& c : name) {
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c) != nullptr) c = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name.empty() ? L"_" : name;
}

std::wstring normRel(std::wstring s) {
    s = util::trim(s);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (!s.empty() && s.front() == L'\\') s.erase(s.begin());
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

bool parseBool(const std::wstring& s, bool dflt) {
    std::wstring v = util::toLower(util::trim(s));
    if (v == L"yes" || v == L"true" || v == L"1" || v == L"on") return true;
    if (v == L"no" || v == L"false" || v == L"0" || v == L"off") return false;
    return dflt;
}

int parseInt(const std::wstring& s, int dflt) {
    std::wstring v = util::trim(s);
    if (v.empty()) return dflt;
    return _wtoi(v.c_str());
}

std::vector<std::wstring> splitList(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wstring cur;
    for (wchar_t c : s) {
        if (c == L';' || c == L',') {
            cur = util::trim(cur);
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    cur = util::trim(cur);
    if (!cur.empty()) out.push_back(cur);
    return out;
}

unsigned long long parseSize(const std::wstring& text) {
    std::wstring s = util::trim(text);
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

// ---------------------------------------------------------------------------
// Standard Windows Installer folder keys
// ---------------------------------------------------------------------------

const StandardDir kStandardDirs[] = {
    {L"ProgramFilesFolder",   L"TARGETDIR", L"."},
    {L"ProgramFiles64Folder", L"TARGETDIR", L"."},
    {L"CommonFilesFolder",    L"TARGETDIR", L"."},
    {L"CommonFiles64Folder",  L"TARGETDIR", L"."},
    {L"ProgramMenuFolder",    L"TARGETDIR", L"."},
    {L"StartMenuFolder",      L"TARGETDIR", L"."},
    {L"StartupFolder",        L"TARGETDIR", L"."},
    {L"DesktopFolder",        L"TARGETDIR", L"."},
    {L"AppDataFolder",        L"TARGETDIR", L"."},
    {L"LocalAppDataFolder",   L"TARGETDIR", L"."},
    {L"CommonAppDataFolder",  L"TARGETDIR", L"."},
    {L"PersonalFolder",       L"TARGETDIR", L"."},
    {L"SendToFolder",         L"TARGETDIR", L"."},
    {L"TemplatesFolder",      L"TARGETDIR", L"."},
    {L"FavoritesFolder",      L"TARGETDIR", L"."},
    {L"FontsFolder",          L"TARGETDIR", L"."},
    {L"MyPicturesFolder",     L"TARGETDIR", L"."},
    {L"AdminToolsFolder",     L"TARGETDIR", L"."},
    {L"TempFolder",           L"TARGETDIR", L"."},
    {L"WindowsFolder",        L"TARGETDIR", L"."},
    {L"WindowsVolume",        L"TARGETDIR", L"."},
    {L"SystemFolder",         L"TARGETDIR", L"."},
    {L"System64Folder",       L"TARGETDIR", L"."},
};

const StandardDir* findStandardDir(const std::wstring& name) {
    for (const StandardDir& d : kStandardDirs)
        if (util::iequals(name, d.key)) return &d;
    return nullptr;
}

}  // namespace pack
