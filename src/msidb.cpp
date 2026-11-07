#include "msidb.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace msi {

namespace {

constexpr UINT kStreamChunk = 64 * 1024;

std::wstring recordString(MSIHANDLE rec, unsigned field) {
    DWORD len = 512;
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 2, L'\0');
    UINT rc = MsiRecordGetStringW(rec, field, buf.data(), &len);
    if (rc == ERROR_MORE_DATA) {
        buf.assign(static_cast<size_t>(len) + 2, L'\0');
        rc = MsiRecordGetStringW(rec, field, buf.data(), &len);
    }
    if (rc != ERROR_SUCCESS) return L"";
    return std::wstring(buf.data());
}

std::wstring renderField(MSIHANDLE rec, unsigned field, const std::wstring& type) {
    if (MsiRecordIsNull(rec, field)) return L"";

    const wchar_t t = type.empty()
                          ? L'?'
                          : static_cast<wchar_t>(towlower(static_cast<wint_t>(type[0])));
    if (t == L'i') {
        int v = MsiRecordGetInteger(rec, field);
        return v == static_cast<int>(MSI_NULL_INTEGER) ? L"" : std::to_wstring(v);
    }
    if (t == L'v') {
        UINT size = MsiRecordDataSize(rec, field);
        return util::fmt(L"<stream: %u bytes>", size);
    }

    // Strings first; MSI returns an error for integer fields, so fall back.
    DWORD len = 512;
    std::vector<wchar_t> buf(static_cast<size_t>(len) + 2, L'\0');
    UINT rc = MsiRecordGetStringW(rec, field, buf.data(), &len);
    if (rc == ERROR_MORE_DATA) {
        buf.assign(static_cast<size_t>(len) + 2, L'\0');
        rc = MsiRecordGetStringW(rec, field, buf.data(), &len);
    }
    if (rc == ERROR_SUCCESS) return std::wstring(buf.data());

    int v = MsiRecordGetInteger(rec, field);
    if (v != static_cast<int>(MSI_NULL_INTEGER)) return std::to_wstring(v);
    return L"";
}

Handle makeParams(const std::vector<std::wstring>& params) {
    if (params.empty()) return Handle();
    Handle rec(MsiCreateRecord(static_cast<UINT>(params.size())));
    if (!rec) util::fail(L"MsiCreateRecord failed");
    for (size_t i = 0; i < params.size(); ++i) {
        if (params[i].empty()) continue;  // leave the field NULL
        util::checkMsi(MsiRecordSetStringW(rec.get(), static_cast<UINT>(i + 1),
                                          params[i].c_str()),
                       L"MsiRecordSetStringW");
    }
    return rec;
}

std::wstring fileTimeText(const FILETIME& ft) {
    SYSTEMTIME st{};
    if (!FileTimeToSystemTime(&ft, &st)) return L"";
    return util::fmt(L"%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear, st.wMonth, st.wDay,
                     st.wHour, st.wMinute, st.wSecond);
}

struct PropDef {
    UINT        id;
    const wchar_t* name;
};

// Property IDs of the OLE SummaryInformation stream as used by Windows Installer.
const PropDef kSummaryProps[] = {
    {2,  L"Title"},
    {3,  L"Subject"},
    {4,  L"Author"},
    {5,  L"Keywords"},
    {6,  L"Comments"},
    {7,  L"Template (platform;language)"},
    {8,  L"Last saved by"},
    {9,  L"Revision number (PackageCode)"},
    {10, L"Editing time"},
    {11, L"Last printed"},
    {12, L"Create time"},
    {13, L"Last save time"},
    {14, L"Page count (min. installer version)"},
    {15, L"Word count (package flags)"},
    {16, L"Character count"},
    {18, L"Creating application"},
    {19, L"Security (read-only level)"},
};

}  // namespace

std::wstring quoteIdent(const std::wstring& name) {
    return L"`" + util::replaceAll(name, L"`", L"``") + L"`";
}

FileProbe probe(const std::wstring& path) {
    FileProbe p;
    p.path = util::path::full(path);
    p.isPatch = util::iequals(util::path::ext(p.path), L".msp");

    DWORD attrs = GetFileAttributesW(p.path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        p.error = L"File not found: " + p.path;
        return p;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        p.error = L"Path is a directory, not a package: " + p.path;
        p.signatureHex = L"(not a file)";
        return p;
    }

    HANDLE h = CreateFileW(p.path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        p.error = L"Cannot open " + p.path + L": " + util::winError(GetLastError());
        return p;
    }
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(h, &sz)) p.size = static_cast<unsigned long long>(sz.QuadPart);
    BYTE header[8] = {};
    DWORD got = 0;
    if (!ReadFile(h, header, sizeof(header), &got, nullptr)) got = 0;
    CloseHandle(h);

    for (DWORD i = 0; i < got; ++i)
        p.signatureHex += util::fmt(L"%02X ", static_cast<unsigned>(header[i]));
    p.signatureHex = util::trim(p.signatureHex);
    if (p.signatureHex.empty()) p.signatureHex = L"(empty)";

    // Windows Installer databases are OLE compound files (CFBF).
    static const BYTE kOleSignature[8] = {0xD0, 0xCF, 0x11, 0xE0,
                                          0xA1, 0xB1, 0x1A, 0xE1};
    p.signatureOk = got == sizeof(kOleSignature) &&
                    std::memcmp(header, kOleSignature, sizeof(kOleSignature)) == 0;
    if (p.size < 512) {
        p.error = L"File is too small to be a Windows Installer package (" +
                  std::to_wstring(p.size) + L" bytes): " + p.path;
    } else if (!p.signatureOk) {
        p.error = L"Not a Windows Installer package: missing the OLE compound-file "
                  L"signature D0 CF 11 E0 A1 B1 1A E1 (found " + p.signatureHex +
                  L"). Expected an .msi or .msp file; .mst transforms, EXE/ZIP/text "
                  L"files are not supported: " + p.path;
    }
    return p;
}

Db::Db(const std::wstring& path) : path_(util::path::full(path)) {
    // 1. Preflight: make sure the input really is an MSI/MSP storage at all.
    FileProbe p = probe(path_);
    if (!p.error.empty()) util::fail(p.error);

    // 2. Open. A patch package must be opened with the MSIDBOPEN_PATCHFILE
    //    pseudo-pointer; plain MSIDBOPEN_READONLY fails with ERROR_OPEN_FAILED.
    //    The SDK macro expands to 16 in a Unicode build (32/sizeof(wchar_t)),
    //    but msi.dll actually expects the byte value 32 - try both.
    if (p.isPatch) {
        const ULONG_PTR patchModes[] = {32,
                                        static_cast<ULONG_PTR>(MSIDBOPEN_PATCHFILE)};
        UINT rc = ERROR_OPEN_FAILED;
        for (ULONG_PTR m : patchModes) {
            rc = MsiOpenDatabaseW(path_.c_str(), reinterpret_cast<LPCWSTR>(m), h_.out());
            if (rc == ERROR_SUCCESS && h_) break;
        }
        if (rc != ERROR_SUCCESS || !h_)
            util::fail(L"Cannot open patch package " + path_ + L": " + util::msiError(rc));
    } else {
        UINT rc = MsiOpenDatabaseW(path_.c_str(), MSIDBOPEN_READONLY, h_.out());
        if (rc != ERROR_SUCCESS || !h_)
            util::fail(L"Not a valid Windows Installer database: " + path_ +
                       L" - " + util::msiError(rc));
    }

    // 3. Structural check: every Installer database exposes _Tables. This
    //    rejects OLE compound documents that are not Installer storages.
    Handle view;
    UINT rc = MsiDatabaseOpenViewW(h_.get(), L"SELECT `Name` FROM `_Tables`", view.out());
    if (rc != ERROR_SUCCESS || MsiViewExecute(view.get(), 0) != ERROR_SUCCESS)
        util::fail(L"Not a valid Windows Installer database (no _Tables table): " + path_);
}

QueryResult Db::query(const std::wstring& sql,
                      const std::vector<std::wstring>& params) const {
    Handle view;
    util::checkMsi(MsiDatabaseOpenViewW(h_.get(), sql.c_str(), view.out()),
                   L"Cannot prepare query: " + sql);
    Handle paramRec = makeParams(params);
    util::checkMsi(MsiViewExecute(view.get(), paramRec.get()),
                   L"Cannot execute query: " + sql);

    QueryResult result;
    Handle names, types;
    UINT rcNames = MsiViewGetColumnInfo(view.get(), MSICOLINFO_NAMES, names.out());
    UINT rcTypes = MsiViewGetColumnInfo(view.get(), MSICOLINFO_TYPES, types.out());
    if (rcNames == ERROR_SUCCESS && names) {
        unsigned n = MsiRecordGetFieldCount(names.get());
        for (unsigned i = 1; i <= n; ++i)
            result.columns.push_back(recordString(names.get(), i));
    }
    if (rcTypes == ERROR_SUCCESS && types) {
        unsigned n = MsiRecordGetFieldCount(types.get());
        for (unsigned i = 1; i <= n; ++i)
            result.types.push_back(recordString(types.get(), i));
    }
    const size_t colCount = std::max(result.columns.size(), result.types.size());

    for (;;) {
        Handle rec;
        UINT rc = MsiViewFetch(view.get(), rec.out());
        if (rc == ERROR_NO_MORE_ITEMS) break;
        util::checkMsi(rc, L"Cannot fetch row of query: " + sql);
        unsigned n = MsiRecordGetFieldCount(rec.get());
        std::vector<std::wstring> row(std::max<size_t>(colCount, n), L"");
        for (unsigned i = 1; i <= n; ++i) {
            std::wstring type = (i - 1 < result.types.size()) ? result.types[i - 1] : L"";
            row[i - 1] = renderField(rec.get(), i, type);
        }
        result.rows.push_back(std::move(row));
    }
    return result;
}

std::wstring Db::scalar(const std::wstring& sql,
                        const std::vector<std::wstring>& params) const {
    QueryResult r = query(sql, params);
    if (r.rows.empty() || r.rows[0].empty()) return L"";
    return r.rows[0][0];
}

size_t Db::rowCount(const std::wstring& table) const {
    Handle view;
    if (MsiDatabaseOpenViewW(
            h_.get(), (L"SELECT * FROM " + quoteIdent(table)).c_str(), view.out()) !=
        ERROR_SUCCESS)
        return static_cast<size_t>(-1);
    if (MsiViewExecute(view.get(), 0) != ERROR_SUCCESS) return static_cast<size_t>(-1);
    size_t n = 0;
    for (;;) {
        Handle rec;
        UINT rc = MsiViewFetch(view.get(), rec.out());
        if (rc == ERROR_NO_MORE_ITEMS) return n;
        if (rc != ERROR_SUCCESS) return static_cast<size_t>(-1);
        ++n;
    }
}

QueryResult Db::columnsOf(const std::wstring& table) const {
    QueryResult out;
    Handle view;
    util::checkMsi(MsiDatabaseOpenViewW(
                       h_.get(), (L"SELECT * FROM " + quoteIdent(table)).c_str(), view.out()),
                   L"Cannot open table " + table);
    util::checkMsi(MsiViewExecute(view.get(), 0), L"Cannot open table " + table);
    Handle names, types;
    if (MsiViewGetColumnInfo(view.get(), MSICOLINFO_NAMES, names.out()) == ERROR_SUCCESS &&
        names) {
        unsigned n = MsiRecordGetFieldCount(names.get());
        for (unsigned i = 1; i <= n; ++i)
            out.columns.push_back(recordString(names.get(), i));
    }
    if (MsiViewGetColumnInfo(view.get(), MSICOLINFO_TYPES, types.out()) == ERROR_SUCCESS &&
        types) {
        unsigned n = MsiRecordGetFieldCount(types.get());
        for (unsigned i = 1; i <= n; ++i)
            out.types.push_back(recordString(types.get(), i));
    }
    return out;
}

std::vector<std::wstring> Db::tableNames() const {
    std::vector<std::wstring> names;
    try {
        QueryResult r = query(L"SELECT `Name` FROM `_Tables` ORDER BY `Name`");
        for (const auto& row : r.rows)
            if (!row.empty() && !row[0].empty()) names.push_back(row[0]);
    } catch (const util::Error&) {
        // A few odd packages do not expose _Tables; nothing else we can do.
    }
    return names;
}

std::wstring Db::resolveTable(const std::wstring& name) const {
    auto tables = tableNames();
    for (const auto& t : tables)
        if (t == name) return t;
    for (const auto& t : tables)
        if (util::iequals(t, name)) return t;
    return L"";
}

bool Db::tableExists(const std::wstring& name) const {
    return !resolveTable(name).empty();
}

bool Db::isPersistent(const std::wstring& table) const {
    return MsiDatabaseIsTablePersistentW(h_.get(), table.c_str()) == MSICONDITION_TRUE;
}

std::vector<std::wstring> Db::primaryKeys(const std::wstring& table) const {
    std::vector<std::wstring> keys;
    Handle rec;
    UINT rc = MsiDatabaseGetPrimaryKeysW(h_.get(), table.c_str(), rec.out());
    if (rc != ERROR_SUCCESS || !rec) return keys;
    unsigned n = MsiRecordGetFieldCount(rec.get());
    for (unsigned i = 1; i <= n; ++i) keys.push_back(recordString(rec.get(), i));
    return keys;
}

std::vector<std::wstring> Db::streamNames() const {
    std::vector<std::wstring> names;
    try {
        QueryResult r = query(L"SELECT `Name` FROM `_Streams` ORDER BY `Name`");
        for (const auto& row : r.rows)
            if (!row.empty()) names.push_back(row[0]);
    } catch (const util::Error&) {
    }
    return names;
}

std::vector<unsigned char> Db::readStream(const std::wstring& sql,
                                          const std::vector<std::wstring>& params,
                                          unsigned field) const {
    Handle view;
    util::checkMsi(MsiDatabaseOpenViewW(h_.get(), sql.c_str(), view.out()),
                   L"Cannot prepare stream query: " + sql);
    Handle paramRec = makeParams(params);
    util::checkMsi(MsiViewExecute(view.get(), paramRec.get()),
                   L"Cannot execute stream query: " + sql);

    Handle rec;
    UINT rc = MsiViewFetch(view.get(), rec.out());
    if (rc == ERROR_NO_MORE_ITEMS) return {};
    util::checkMsi(rc, L"Cannot fetch stream record");

    std::vector<unsigned char> data;
    std::vector<char> buf(kStreamChunk);
    for (;;) {
        DWORD want = kStreamChunk;
        UINT r = MsiRecordReadStream(rec.get(), field, buf.data(), &want);
        if (r != ERROR_SUCCESS)
            util::fail(L"MsiRecordReadStream: " + util::msiError(r));
        if (want) data.insert(data.end(), buf.begin(), buf.begin() + want);
        if (want < kStreamChunk) break;
    }
    return data;
}

UINT Db::exportTable(const std::wstring& table,
                     const std::wstring& folder,
                     const std::wstring& file) const {
    return MsiDatabaseExportW(h_.get(), table.c_str(), folder.c_str(), file.c_str());
}

std::wstring Db::property(const std::wstring& name) const {
    if (!tableExists(L"Property")) return L"";
    return scalar(L"SELECT `Value` FROM `Property` WHERE `Property`=?", {name});
}

std::vector<SummaryItem> Db::summary() const {
    std::vector<SummaryItem> items;
    Handle si;
    UINT rc = MsiGetSummaryInformationW(h_.get(), nullptr, 0, si.out());
    if (rc != ERROR_SUCCESS || !si) return items;

    for (const auto& def : kSummaryProps) {
        UINT type = 0;
        INT  iValue = 0;
        FILETIME ft{};
        wchar_t buf[2048] = {};
        DWORD len = _countof(buf);
        UINT r = MsiSummaryInfoGetPropertyW(si.get(), def.id, &type, &iValue, &ft, buf, &len);
        if (r == ERROR_MORE_DATA) {
            std::vector<wchar_t> big(static_cast<size_t>(len) + 2, L'\0');
            DWORD bigLen = static_cast<DWORD>(big.size());
            r = MsiSummaryInfoGetPropertyW(si.get(), def.id, &type, &iValue, &ft,
                                           big.data(), &bigLen);
            if (r == ERROR_SUCCESS) {
                items.push_back({def.id, def.name, std::wstring(big.data())});
                continue;
            }
        }
        if (r != ERROR_SUCCESS) continue;

        SummaryItem item{def.id, def.name, L""};
        if (type == 30 || type == 31)  // VT_LPSTR / VT_LPWSTR
            item.value = buf;
        else if (type == 64)  // VT_FILETIME
            item.value = fileTimeText(ft);
        else if (type == 3)  // VT_I4
            item.value = std::to_wstring(iValue);
        if (!item.value.empty()) items.push_back(std::move(item));
    }
    return items;
}

}  // namespace msi
