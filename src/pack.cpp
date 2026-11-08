#include "pack.h"
#include "msidb.h"

#include <fci.h>

#include <objbase.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pack {

namespace {

// ---------------------------------------------------------------------------
// FCI (cabinet.dll) callbacks - creates one MSZIP cabinet at a time.
// Path strings handed to FCI are UTF-8; our callbacks convert back to UTF-16,
// so source paths containing spaces / non-ASCII characters work.
// Each cabinet is built with its own FCI context; that keeps the CAB set
// independent and makes the File.Sequence -> Media mapping exact.
// ---------------------------------------------------------------------------

struct FciCtx {
    std::string cabDirUtf8;    // absolute, with trailing backslash
    std::string cabNameUtf8;   // "cab1.cab"
    bool        cabStarted = false;
    unsigned long tempNo = 0;
    bool        verbose = false;
};

FciCtx* g_fci = nullptr;

std::wstring widePath(const char* p) { return util::widen(p ? p : "", CP_UTF8); }

void copyChars(char* dst, size_t cap, const std::string& s) {
    size_t n = std::min(s.size(), cap - 1);
    std::memcpy(dst, s.data(), n);
    dst[n] = '\0';
}

FNFCIALLOC(fciAlloc) { return malloc(cb); }
FNFCIFREE(fciFree) { free(memory); }

FNFCIOPEN(fciOpen) {
    (void)pmode; (void)pv;
    if (err) *err = 0;
    std::wstring path = widePath(pszFile);
    if (path.empty()) return -1;

    DWORD access = GENERIC_READ;
    if ((oflag & _O_RDWR) == _O_RDWR) access = GENERIC_READ | GENERIC_WRITE;
    else if (oflag & _O_WRONLY) access = GENERIC_WRITE;
    DWORD creation = OPEN_EXISTING;
    if (oflag & _O_CREAT)
        creation = (oflag & _O_EXCL) ? CREATE_NEW
                                     : ((oflag & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS);
    else if (oflag & _O_TRUNC)
        creation = TRUNCATE_EXISTING;

    HANDLE h = CreateFileW(path.c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = static_cast<int>(GetLastError());
        return -1;
    }
    return reinterpret_cast<INT_PTR>(h);
}

FNFCIREAD(fciRead) {
    (void)pv;
    DWORD got = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(hf), memory, cb, &got, nullptr)) {
        if (err) *err = static_cast<int>(GetLastError());
        return static_cast<UINT>(-1);
    }
    return got;
}

FNFCIWRITE(fciWrite) {
    (void)pv;
    DWORD wrote = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(hf), memory, cb, &wrote, nullptr)) {
        if (err) *err = static_cast<int>(GetLastError());
        return static_cast<UINT>(-1);
    }
    return wrote;
}

FNFCICLOSE(fciClose) {
    (void)pv;
    HANDLE h = reinterpret_cast<HANDLE>(hf);
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    if (!CloseHandle(h)) {
        if (err) *err = static_cast<int>(GetLastError());
        return -1;
    }
    return 0;
}

FNFCISEEK(fciSeek) {
    (void)pv;
    LARGE_INTEGER distance{};
    distance.QuadPart = dist;
    LARGE_INTEGER pos{};
    DWORD method = seektype == SEEK_CUR ? FILE_CURRENT
                                        : (seektype == SEEK_END ? FILE_END : FILE_BEGIN);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(hf), distance, &pos, method)) {
        if (err) *err = static_cast<int>(GetLastError());
        return -1L;
    }
    return static_cast<long>(pos.QuadPart);
}

FNFCIDELETE(fciDelete) {
    (void)pv;
    if (err) *err = 0;
    std::wstring path = widePath(pszFile);
    if (path.empty() || !DeleteFileW(path.c_str())) {
        if (err) *err = static_cast<int>(GetLastError());
        return -1;
    }
    return 0;
}

FNFCIGETTEMPFILE(fciTempFile) {
    (void)pv;
    if (!g_fci || cbTempName <= 0) return FALSE;
    std::string name = "fci_tmp_" + std::to_string(++g_fci->tempNo) + ".bin";
    std::string full = g_fci->cabDirUtf8 + name;
    if (full.size() >= static_cast<size_t>(cbTempName)) return FALSE;
    copyChars(pszTempName, static_cast<size_t>(cbTempName), full);
    return TRUE;
}

FNFCIGETNEXTCABINET(fciNextCabinet) {
    (void)cbPrevCab; (void)pv;
    // One pass only: a single-cabinet FCI context. A cabinet set is produced
    // by running several contexts, one per volume.
    if (!g_fci || g_fci->cabStarted) return FALSE;
    g_fci->cabStarted = true;
    copyChars(pccab->szCab, sizeof(pccab->szCab), g_fci->cabNameUtf8);
    copyChars(pccab->szCabPath, sizeof(pccab->szCabPath), g_fci->cabDirUtf8);
    copyChars(pccab->szDisk, sizeof(pccab->szDisk), "");
    pccab->setID = 0x5A5A;
    pccab->iCab = 1;
    pccab->iDisk = 1;
    return TRUE;
}

FNFCIFILEPLACED(fciFilePlaced) {
    (void)pccab; (void)pszFile; (void)cbFile; (void)fContinuation; (void)pv;
    return 0;
}

FNFCISTATUS(fciStatus) {
    (void)typeStatus; (void)cb1; (void)pv;
    // statusCabinet expects the actual cabinet size back.
    return static_cast<long>(cb2);
}

FNFCIGETOPENINFO(fciGetOpenInfo) {
    (void)pv;
    if (err) *err = 0;
    std::wstring path = widePath(pszName);
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (err) *err = static_cast<int>(GetLastError());
        return -1;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileInformationByHandle(h, &info)) {
        FileTimeToDosDateTime(&info.ftLastWriteTime, pdate, ptime);
        *pattribs = static_cast<USHORT>(info.dwFileAttributes & 0x00FF);
    } else {
        *pdate = 0;
        *ptime = 0;
        *pattribs = 0;
    }
    return reinterpret_cast<INT_PTR>(h);
}

bool createCabinet(const std::wstring& cabPath, const std::vector<std::wstring>& sources,
                   const std::vector<std::wstring>& cabNames, bool verbose) {
    FciCtx ctx;
    ctx.cabDirUtf8 = util::narrow(util::path::dirName(cabPath));
    if (ctx.cabDirUtf8.empty()) ctx.cabDirUtf8 = ".\\";
    ctx.cabNameUtf8 = util::narrow(util::path::fileName(cabPath));
    ctx.verbose = verbose;
    g_fci = &ctx;

    CCAB ccab{};
    ccab.cb = 0x7FFFFFFF;             // one cabinet, no auto splitting
    ccab.cbFolderThresh = 0x7FFFFFFF;
    ccab.setID = 0x5A5A;
    ccab.iCab = 1;
    ccab.iDisk = 1;
    copyChars(ccab.szCab, sizeof(ccab.szCab), ctx.cabNameUtf8);
    copyChars(ccab.szCabPath, sizeof(ccab.szCabPath), ctx.cabDirUtf8);
    copyChars(ccab.szDisk, sizeof(ccab.szDisk), "");

    ERF erf{};
    HFCI hfci = FCICreate(&erf, fciFilePlaced, fciAlloc, fciFree, fciOpen, fciRead,
                          fciWrite, fciClose, fciSeek, fciDelete, fciTempFile,
                          &ccab, nullptr);
    if (!hfci) {
        g_fci = nullptr;
        util::fail(util::fmt(L"FCICreate failed (oper=%d, type=%d)", erf.erfOper,
                             erf.erfType));
    }

    bool ok = true;
    std::wstring failed;
    for (size_t i = 0; i < sources.size() && ok; ++i) {
        std::string src = util::narrow(sources[i]);
        std::string name = util::narrow(cabNames[i]);
        std::vector<char> srcBuf(src.begin(), src.end());
        std::vector<char> nameBuf(name.begin(), name.end());
        srcBuf.push_back('\0');
        nameBuf.push_back('\0');
        if (!FCIAddFile(hfci, srcBuf.data(), nameBuf.data(), FALSE, fciNextCabinet,
                        fciStatus, fciGetOpenInfo, tcompTYPE_MSZIP)) {
            ok = false;
            failed = sources[i];
        }
        util::verboseLine(verbose, cabNames[i] + L"  <-  " + sources[i]);
    }
    if (ok && !FCIFlushCabinet(hfci, FALSE, fciNextCabinet, fciStatus)) ok = false;
    FCIDestroy(hfci);
    g_fci = nullptr;

    if (!ok)
        util::fail(L"Failed to build the cabinet" +
                   (failed.empty() ? L"" : L" while adding " + failed) +
                   util::fmt(L" (FCI oper=%d, type=%d)", erf.erfOper, erf.erfType));
    if (!util::path::exists(cabPath))
        util::fail(L"Cabinet was not created: " + cabPath);
    return true;
}

// ---------------------------------------------------------------------------
// MSI helpers
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

void execSql(MSIHANDLE db, const std::wstring& sql) {
    msi::Handle view;
    util::checkMsi(MsiDatabaseOpenViewW(db, sql.c_str(), view.out()), L"SQL: " + sql);
    util::checkMsi(MsiViewExecute(view.get(), 0), L"SQL: " + sql);
}

struct Cell {
    bool        isNull = true;
    bool        isInt = false;
    int         i = 0;
    std::wstring s;
};
Cell S(std::wstring v) { Cell c; c.isNull = false; c.s = std::move(v); return c; }
Cell I(int v)          { Cell c; c.isNull = false; c.isInt = true; c.i = v; return c; }
Cell Null()            { return Cell{}; }

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

struct SequenceAction {
    const wchar_t* name;
    int            seq;
};

const SequenceAction kSequence[] = {
    {L"CostInitialize",   800}, {L"FileCost",         900}, {L"CostFinalize",   1000},
    {L"InstallValidate", 1400}, {L"InstallInitialize",1500}, {L"ProcessComponents",1600},
    {L"UnpublishFeatures",1800},{L"StopServices",    1900}, {L"DeleteServices", 2000},
    {L"RemoveRegistryValues",2600}, {L"RemoveShortcuts", 3500}, {L"RemoveFiles", 3500},
    {L"CreateFolders",   3700}, {L"InstallFiles",    4000}, {L"CreateShortcuts", 4500},
    {L"WriteRegistryValues",5000}, {L"WriteEnvironmentStrings", 5200},
    {L"InstallServices", 5800}, {L"StartServices",   5900}, {L"RegisterProduct", 6100},
    {L"PublishFeatures", 6300}, {L"PublishProduct",  6400}, {L"InstallExecute",  6500},
    {L"InstallFinalize", 6600},
};

// Administrative install (msiexec /a or ACTION=ADMIN) unpacks the CAB
// without writing registry/ARP state.
const SequenceAction kAdminSequence[] = {
    {L"CostInitialize",     800}, {L"FileCost",           900}, {L"CostFinalize",    1000},
    {L"InstallValidate",   1400}, {L"InstallInitialize", 1500},
    {L"CreateFolders",     3700},
    {L"InstallAdminPackage",3900},{L"InstallFiles",      4000},{L"InstallFinalize", 6600},
};

const SequenceAction kUiSequenceBasic[] = {
    {L"CostInitialize", 800}, {L"FileCost", 900}, {L"CostFinalize", 1000},
    {L"ExecuteAction", 1300},
};

const SequenceAction kUiSequenceMinimal[] = {
    {L"CostInitialize",  800}, {L"FileCost",     900}, {L"CostFinalize", 1000},
    {L"WelcomeDlg",     1290}, {L"ProgressDlg", 1299}, {L"ExecuteAction", 1300},
    {L"FatalError",       -3}, {L"UserExit",      -2}, {L"ExitDialog",     -1},
};

// ---------------------------------------------------------------------------
// Optional manifest (INI-like UTF-8 / UTF-16 file)
// ---------------------------------------------------------------------------

struct Section {
    std::wstring type;
    std::wstring name;
    std::map<std::wstring, std::wstring> values;
};

std::wstring readConfigFile(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        util::fail(L"Cannot open config file: " + path + L": " +
                   util::winError(GetLastError()));
    DWORD hi = 0;
    DWORD size = GetFileSize(h, &hi);
    if (hi || size > 64 * 1024 * 1024) {
        CloseHandle(h);
        util::fail(L"Config file is too large: " + path);
    }
    std::vector<unsigned char> buf(size + 1, 0);
    DWORD got = 0;
    if (size && !ReadFile(h, buf.data(), size, &got, nullptr)) {
        CloseHandle(h);
        util::fail(L"Cannot read config file: " + path);
    }
    CloseHandle(h);
    buf.resize(got);

    if (buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) {
        return util::widen(std::string(buf.begin() + 3, buf.end()), CP_UTF8);
    }
    if (buf.size() >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        std::wstring s;
        for (size_t i = 2; i + 1 < buf.size(); i += 2)
            s.push_back(static_cast<wchar_t>(buf[i] | (buf[i + 1] << 8)));
        return s;
    }
    return util::widen(std::string(buf.begin(), buf.end()), CP_UTF8);
}

Section parseSectionHeader(const std::wstring& text) {
    Section s;
    std::wstring body = util::trim(text.substr(1, text.size() - 2));
    size_t colon = body.find(L':');
    if (colon == std::wstring::npos) {
        s.type = util::toLower(body);
    } else {
        s.type = util::toLower(util::trim(body.substr(0, colon)));
        s.name = util::trim(body.substr(colon + 1));
    }
    return s;
}

std::vector<Section> parseConfig(const std::wstring& text) {
    std::vector<Section> sections;
    Section* cur = nullptr;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find_first_of(L"\r\n", start);
        std::wstring line = text.substr(start, end == std::wstring::npos
                                                     ? std::wstring::npos : end - start);
        start = end == std::wstring::npos ? text.size() + 1 : end + 1;
        line = util::trim(line);
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            sections.push_back(parseSectionHeader(line));
            cur = &sections.back();
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || !cur) continue;
        std::wstring key = util::toLower(util::trim(line.substr(0, eq)));
        std::wstring val = util::trim(line.substr(eq + 1));
        cur->values[key] = val;
    }
    return sections;
}

std::wstring cfg(const Section& s, const wchar_t* key, const std::wstring& dflt = L"") {
    auto it = s.values.find(key);
    return it == s.values.end() ? dflt : it->second;
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

std::wstring normRel(std::wstring s) {
    s = util::trim(s);
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (!s.empty() && s.front() == L'\\') s.erase(s.begin());
    while (!s.empty() && s.back() == L'\\') s.pop_back();
    return s;
}

std::wstring toIdentifier(std::wstring s, size_t maxLen = 72) {
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

struct FeatureDef {
    std::wstring id, title, description, dirs;
    int level = 1, display = 0, attributes = 0;
};
struct ShortcutDef {
    std::wstring id, name, dir, target, args, description, icon, wkdir, component;
};
struct RegistryDef {
    std::wstring id, root, key, name, value, type, component;
};
struct ServiceDef {
    std::wstring id, name, displayName, image, type, start, error, account,
                 password, args, dependencies, description, component;
};
struct EnvDef {
    std::wstring id, name, value, action, separator, system, component;
};

struct Manifest {
    std::wstring ui, largeFiles, signCert, signPassword, signTimestamp, signTool;
    unsigned long long cabSize = 0;
    bool keepEmptyDirs = true;
    std::vector<FeatureDef> features;
    std::vector<ShortcutDef> shortcuts;
    std::vector<RegistryDef> registry;
    std::vector<ServiceDef> services;
    std::vector<EnvDef> environment;
};

Manifest loadManifest(const std::wstring& path) {
    Manifest m;
    std::vector<Section> sections = parseConfig(readConfigFile(path));
    for (const Section& s : sections) {
        if (s.type == L"pack") {
            m.ui = cfg(s, L"ui", m.ui);
            m.largeFiles = cfg(s, L"large-files", m.largeFiles);
            m.signCert = cfg(s, L"sign-cert", m.signCert);
            m.signPassword = cfg(s, L"sign-password", m.signPassword);
            m.signTimestamp = cfg(s, L"sign-timestamp", m.signTimestamp);
            m.signTool = cfg(s, L"sign-tool", m.signTool);
            m.keepEmptyDirs = parseBool(cfg(s, L"keep-empty-dirs"), m.keepEmptyDirs);
            unsigned long long mib = static_cast<unsigned long long>(
                parseInt(cfg(s, L"cab-size"), 0));
            m.cabSize = mib * 1024ULL * 1024ULL;
        } else if (s.type == L"feature") {
            FeatureDef f;
            f.id = toIdentifier(s.name.empty() ? L"Feature" : s.name, 38);
            f.title = cfg(s, L"title", f.id);
            f.description = cfg(s, L"description", f.title);
            f.dirs = cfg(s, L"dirs");
            f.level = parseInt(cfg(s, L"level"), 1);
            f.display = parseInt(cfg(s, L"display"), 0);
            f.attributes = parseInt(cfg(s, L"attributes"), 0);
            m.features.push_back(std::move(f));
        } else if (s.type == L"shortcut") {
            ShortcutDef d;
            d.id = toIdentifier(s.name.empty() ? L"Shortcut" : s.name);
            d.name = cfg(s, L"name", d.id + L".lnk");
            d.dir = cfg(s, L"dir", L"ProgramMenuFolder");
            d.target = cfg(s, L"target");
            d.args = cfg(s, L"args");
            d.description = cfg(s, L"description");
            d.icon = cfg(s, L"icon");
            d.wkdir = cfg(s, L"wkdir");
            d.component = cfg(s, L"component");
            m.shortcuts.push_back(std::move(d));
        } else if (s.type == L"registry") {
            RegistryDef d;
            d.id = toIdentifier(s.name.empty() ? L"Reg" : s.name);
            d.root = cfg(s, L"root", L"auto");
            d.key = cfg(s, L"key");
            d.name = cfg(s, L"name");
            d.value = cfg(s, L"value");
            d.type = util::toLower(cfg(s, L"type", L"string"));
            d.component = cfg(s, L"component");
            m.registry.push_back(std::move(d));
        } else if (s.type == L"service") {
            ServiceDef d;
            d.id = toIdentifier(s.name.empty() ? L"Service" : s.name);
            d.name = cfg(s, L"name", d.id);
            d.displayName = cfg(s, L"displayname", d.name);
            d.image = cfg(s, L"image");
            d.type = util::toLower(cfg(s, L"type", L"own"));
            d.start = util::toLower(cfg(s, L"start", L"demand"));
            d.error = util::toLower(cfg(s, L"error", L"normal"));
            d.account = cfg(s, L"account");
            d.password = cfg(s, L"password");
            d.args = cfg(s, L"args");
            d.dependencies = cfg(s, L"dependencies");
            d.description = cfg(s, L"description");
            d.component = cfg(s, L"component");
            m.services.push_back(std::move(d));
        } else if (s.type == L"environment") {
            EnvDef d;
            d.id = toIdentifier(s.name.empty() ? L"Env" : s.name);
            d.name = cfg(s, L"name", d.id);
            d.value = cfg(s, L"value");
            d.action = util::toLower(cfg(s, L"action", L"set"));
            d.separator = cfg(s, L"separator", L";");
            d.system = cfg(s, L"system", L"no");
            d.component = cfg(s, L"component");
            m.environment.push_back(std::move(d));
        } else {
            util::fail(L"Unknown section in config: [" + s.type + L"]");
        }
    }
    // De-duplicate generated identifiers.
    std::set<std::wstring> seen;
    auto unique = [&seen](std::wstring id) {
        std::wstring base = id;
        for (int n = 2; !seen.insert(id).second; ++n)
            id = base + L"_" + std::to_wstring(n);
        return id;
    };
    for (auto& f : m.features) f.id = unique(f.id);
    for (auto& d : m.shortcuts) d.id = unique(d.id);
    for (auto& d : m.registry) d.id = unique(d.id);
    for (auto& d : m.services) d.id = unique(d.id);
    for (auto& d : m.environment) d.id = unique(d.id);
    return m;
}

// ---------------------------------------------------------------------------
// Directory enumeration / layout
// ---------------------------------------------------------------------------

struct Entry {
    std::wstring relDir;      // relative directory, "" for the package root
    std::wstring fileName;
    std::wstring fullPath;
    unsigned long long size = 0;
    std::wstring fileKey;     // File table key == name inside the CAB
    bool        external = false;  // > 2 GiB: uncompressed companion file
};

void walk(const std::wstring& dir, const std::wstring& rel,
          std::vector<Entry>& out, std::vector<std::wstring>& dirs, bool verbose) {
    dirs.push_back(rel);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(util::path::join(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE)
        util::fail(L"Cannot enumerate " + dir + L": " + util::winError(GetLastError()));
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        std::wstring full = util::path::join(dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            std::wstring sub = rel.empty() ? std::wstring(fd.cFileName)
                                           : rel + L"\\" + fd.cFileName;
            walk(full, sub, out, dirs, verbose);
        } else {
            Entry e;
            e.relDir = rel;
            e.fileName = fd.cFileName;
            e.fullPath = full;
            e.size = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) |
                     fd.nFileSizeLow;
            out.push_back(std::move(e));
            util::verboseLine(verbose, L"scan " + (rel.empty() ? L"" : rel + L"\\") +
                                           fd.cFileName +
                                           L" (" + std::to_wstring(e.size) + L" B)");
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// ---------------------------------------------------------------------------
// Explicit selection (CLI --files, GUI "only these files")
// ---------------------------------------------------------------------------

bool absolutePath(const std::wstring& p) {
    if (p.size() >= 3 && p[1] == L':' && (p[2] == L'\\' || p[2] == L'/')) return true;
    return !p.empty() && (p.front() == L'\\' || p.front() == L'/');
}

unsigned long long statSize(const std::wstring& p) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(p.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
}

// Resolve one selection entry: absolute paths are used as they are, relative
// ones are looked up below the package root (like the paths in a manifest).
std::wstring absOf(const std::wstring& inDir, const std::wstring& spec) {
    return util::path::full(absolutePath(spec) ? spec
                                               : util::path::join(inDir, spec));
}

// Path of `full` relative to the package root. A selection must live inside
// inputDir: the Directory keys, the Feature tree and the companion layout are
// all derived from that root, so an outside file has nowhere to install to.
std::wstring relToRoot(const std::wstring& rootIn, const std::wstring& full) {
    std::wstring root = rootIn;
    if (!root.empty() && root.back() != L'\\' && root.back() != L'/') root += L'\\';
    if (root.empty() || full.size() <= root.size() ||
        !util::iequals(full.substr(0, root.size()), root))
        util::fail(L"selected entry is outside the source directory: " + full);
    std::wstring rel = normRel(full.substr(root.size()));
    if (rel.empty()) util::fail(L"selected entry is empty: " + full);
    return rel;
}

// Parent of a relative directory path; "" is the package root itself.
std::wstring parentRel(const std::wstring& rel) {
    std::wstring r = normRel(rel);
    size_t pos = r.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : normRel(r.substr(0, pos));
}

// Collect exactly the selected entries. Files keep the path they have relative
// to the root (so a subset still installs into the same directory tree), and a
// directory entry brings its whole subtree by delegating to walk().
void selectFiles(const std::wstring& inDir, const std::vector<std::wstring>& specs,
                 std::vector<Entry>& out, std::vector<std::wstring>& dirs,
                 bool verbose) {
    std::set<std::wstring> seenFile, seenDir;
    for (const std::wstring& raw : specs) {
        std::wstring spec = util::trim(raw);
        if (spec.empty()) continue;
        std::wstring full = absOf(inDir, spec);
        if (!util::path::exists(full))
            util::fail(L"selected entry does not exist: " + full);
        if (util::path::isDirectory(full)) {
            std::wstring rel = util::iequals(normRel(full), normRel(inDir))
                                   ? L""
                                   : normRel(relToRoot(inDir, full));
            util::verboseLine(verbose, L"select " +
                                           (rel.empty() ? L"." : rel) +
                                           L"\\ (whole subtree)");
            walk(full, rel, out, dirs, verbose);
            continue;
        }
        if (!seenFile.insert(util::toLower(full)).second) {
            util::verboseLine(verbose, L"skip duplicate selection " + full);
            continue;
        }
        Entry e;
        std::wstring rel = relToRoot(inDir, full);
        e.relDir = parentRel(rel);
        e.fileName = util::path::fileName(rel);
        e.fullPath = full;
        e.size = statSize(full);
        out.push_back(e);
        util::verboseLine(verbose, L"select " +
                                       (e.relDir.empty() ? L"" : e.relDir + L"\\") +
                                       e.fileName + L" (" +
                                       std::to_wstring(e.size) + L" B)");
        // Every ancestor of a selected file has to exist in the tree, even
        // when nothing else was picked from it.
        for (std::wstring d = e.relDir; !d.empty(); d = parentRel(d))
            if (seenDir.insert(util::toLower(d)).second) dirs.push_back(d);
    }
    // A directory entry and a file listed inside it must not be packed twice.
    std::set<std::wstring> unique;
    out.erase(std::remove_if(out.begin(), out.end(),
                             [&unique](const Entry& e) {
                                 return !unique.insert(util::toLower(e.fullPath)).second;
                             }),
              out.end());
}

struct DirInfo {
    std::wstring key;
    std::wstring parentKey;
    std::wstring leaf;
    std::wstring componentKey;
    std::wstring keyPath;
};

std::wstring sanitizeName(std::wstring name) {
    // MSI identifiers for directory/file names must not contain path syntax
    // or the short|long separator.
    for (wchar_t& c : name) {
        if (c < 0x20 || wcschr(L"\\/:*?\"<>|", c) != nullptr) c = L'_';
    }
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    return name.empty() ? L"_" : name;
}

class LayoutBuilder {
public:
    std::map<std::wstring, DirInfo> dirs;  // lower(relDir) -> info
    std::wstring rootKey = L"ProgramFilesFolder";

    std::wstring ensure(const std::wstring& rel, const std::wstring& productName) {
        std::wstring k = util::toLower(rel);
        auto it = dirs.find(k);
        if (it != dirs.end()) return it->second.key;

        DirInfo d;
        if (rel.empty()) {
            d.key = L"INSTALLFOLDER";
            d.parentKey = rootKey;
            d.leaf = sanitizeName(productName);
        } else {
            size_t pos = rel.rfind(L'\\');
            std::wstring parent = pos == std::wstring::npos ? L"" : rel.substr(0, pos);
            std::wstring leaf = pos == std::wstring::npos ? rel : rel.substr(pos + 1);
            std::wstring parentKey = ensure(parent, productName);
            d.key = L"D" + std::to_wstring(dirs.size() + 1);
            d.parentKey = parentKey;
            d.leaf = sanitizeName(leaf);
        }
        d.componentKey = L"C" + std::to_wstring(dirs.size() + 1);
        std::wstring result = d.key;
        dirs[k] = std::move(d);
        return result;
    }
};

unsigned long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// ---------------------------------------------------------------------------
// Base schema + optional extra tables
// ---------------------------------------------------------------------------

const wchar_t* kSchema[] = {
    L"CREATE TABLE `Directory` (`Directory` CHAR(72) NOT NULL, `Directory_Parent` CHAR(72), "
    L"`DefaultDir` CHAR(255) NOT NULL LOCALIZABLE PRIMARY KEY `Directory`)",
    L"CREATE TABLE `Component` (`Component` CHAR(72) NOT NULL, `ComponentId` CHAR(38), "
    L"`Directory_` CHAR(72) NOT NULL, `Attributes` SHORT NOT NULL, `Condition` CHAR(255), "
    L"`KeyPath` CHAR(72) PRIMARY KEY `Component`)",
    L"CREATE TABLE `File` (`File` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, "
    L"`FileName` CHAR(255) NOT NULL LOCALIZABLE, `FileSize` LONG NOT NULL, "
    L"`Version` CHAR(72), `Language` CHAR(20), `Attributes` SHORT, "
    L"`Sequence` SHORT NOT NULL PRIMARY KEY `File`)",
    L"CREATE TABLE `Media` (`DiskId` SHORT NOT NULL, `LastSequence` LONG NOT NULL, "
    L"`DiskPrompt` CHAR(64) LOCALIZABLE, `Cabinet` CHAR(255), `VolumeLabel` CHAR(32), "
    L"`Source` CHAR(72) PRIMARY KEY `DiskId`)",
    L"CREATE TABLE `Feature` (`Feature` CHAR(38) NOT NULL, `Feature_Parent` CHAR(38), "
    L"`Title` CHAR(64) LOCALIZABLE, `Description` CHAR(255) LOCALIZABLE, `Display` SHORT, "
    L"`Level` SHORT NOT NULL, `Directory_` CHAR(72), `Attributes` SHORT NOT NULL "
    L"PRIMARY KEY `Feature`)",
    L"CREATE TABLE `FeatureComponents` (`Feature_` CHAR(38) NOT NULL, "
    L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `Feature_`, `Component_`)",
    L"CREATE TABLE `Property` (`Property` CHAR(72) NOT NULL, `Value` LONGCHAR "
    L"PRIMARY KEY `Property`)",
    L"CREATE TABLE `InstallExecuteSequence` (`Action` CHAR(72) NOT NULL, "
    L"`Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)",
    L"CREATE TABLE `AdminExecuteSequence` (`Action` CHAR(72) NOT NULL, "
    L"`Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)",
};

const wchar_t* kExtraSchema[] = {
    L"CREATE TABLE `CreateFolder` (`Directory_` CHAR(72) NOT NULL, "
    L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `Directory_`, `Component_`)",
    L"CREATE TABLE `Shortcut` (`Shortcut` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL, "
    L"`Name` CHAR(128) NOT NULL LOCALIZABLE, `Component_` CHAR(72) NOT NULL, "
    L"`Target` CHAR(72) NOT NULL, `Arguments` CHAR(255) LOCALIZABLE, "
    L"`Description` CHAR(255) LOCALIZABLE, `Hotkey` SHORT, `Icon_` CHAR(72), "
    L"`IconIndex` SHORT, `ShowCmd` SHORT, `WkDir` CHAR(72), "
    L"`DisplayResourceDLL` CHAR(255), `DisplayResourceId` SHORT, "
    L"`DescriptionResourceDLL` CHAR(255), `DescriptionResourceId` SHORT "
    L"PRIMARY KEY `Shortcut`)",
    L"CREATE TABLE `Icon` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL "
    L"PRIMARY KEY `Name`)",
    L"CREATE TABLE `Registry` (`Registry` CHAR(72) NOT NULL, `Root` SHORT NOT NULL, "
    L"`Key` CHAR(255) NOT NULL LOCALIZABLE, `Name` CHAR(255) LOCALIZABLE, "
    L"`Value` LONGCHAR LOCALIZABLE, `Component_` CHAR(72) NOT NULL "
    L"PRIMARY KEY `Registry`)",
    L"CREATE TABLE `ServiceInstall` (`ServiceInstall` CHAR(72) NOT NULL, "
    L"`Name` CHAR(255) NOT NULL, `DisplayName` CHAR(255) LOCALIZABLE, "
    L"`ServiceType` LONG NOT NULL, `StartType` LONG NOT NULL, `ErrorControl` LONG NOT NULL, "
    L"`LoadOrderGroup` CHAR(255), `Dependencies` CHAR(255), `StartName` CHAR(255), "
    L"`Password` CHAR(255), `Arguments` CHAR(255), `Component_` CHAR(72) NOT NULL, "
    L"`Description` LONGCHAR LOCALIZABLE PRIMARY KEY `ServiceInstall`)",
    L"CREATE TABLE `ServiceControl` (`ServiceControl` CHAR(72) NOT NULL, "
    L"`Name` CHAR(255) NOT NULL LOCALIZABLE, `Event` SHORT NOT NULL, "
    L"`Arguments` CHAR(255) LOCALIZABLE, `Wait` SHORT, "
    L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `ServiceControl`)",
    L"CREATE TABLE `Environment` (`Environment` CHAR(72) NOT NULL, "
    L"`Name` CHAR(255) NOT NULL LOCALIZABLE, `Value` LONGCHAR LOCALIZABLE, "
    L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `Environment`)",
    L"CREATE TABLE `InstallUISequence` (`Action` CHAR(72) NOT NULL, "
    L"`Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)",
    L"CREATE TABLE `Dialog` (`Dialog` CHAR(72) NOT NULL, `HCentering` SHORT NOT NULL, "
    L"`VCentering` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, "
    L"`Attributes` LONG, `Title` CHAR(128) LOCALIZABLE, `Control_First` CHAR(50) NOT NULL, "
    L"`Control_Default` CHAR(50), `Control_Cancel` CHAR(50) PRIMARY KEY `Dialog`)",
    L"CREATE TABLE `Control` (`Dialog_` CHAR(72) NOT NULL, `Control` CHAR(50) NOT NULL, "
    L"`Type` CHAR(20) NOT NULL, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, "
    L"`Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, "
    L"`Property` CHAR(72), `Text` LONGCHAR LOCALIZABLE, `Control_Next` CHAR(50), "
    L"`Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Dialog_`, `Control`)",
    L"CREATE TABLE `ControlEvent` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, "
    L"`Event` CHAR(50) NOT NULL, `Argument` CHAR(255) NOT NULL, `Condition` CHAR(255), "
    L"`Ordering` SHORT PRIMARY KEY `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`)",
    L"CREATE TABLE `TextStyle` (`TextStyle` CHAR(72) NOT NULL, `FaceName` CHAR(32) NOT NULL, "
    L"`Size` SHORT NOT NULL, `Color` LONG, `StyleBits` SHORT PRIMARY KEY `TextStyle`)",
};

struct UiDialog {
    const wchar_t* name;
    int            attributes;
    const wchar_t* title;
    const wchar_t* text;
    const wchar_t* first;
    const wchar_t* def;
    const wchar_t* cancel;
    int            seq;   // InstallUISequence sequence (0 = termination only)
};

// A compact, self-contained wizard: welcome -> (modeless progress) -> exit.
const UiDialog kMinimalDialogs[] = {
    {L"WelcomeDlg", 7, L"[ProductName] Setup",
     L"Welcome to the [ProductName] [ProductVersion] Setup. Click Install to continue or Cancel to exit.",
     L"Next", L"Next", L"Cancel", 1290},
    {L"ProgressDlg", 5, L"[ProductName] Setup",
     L"Please wait while Setup installs [ProductName]...",
     L"Cancel", L"Cancel", L"Cancel", 1299},
    {L"ExitDialog", 7, L"[ProductName] Setup",
     L"[ProductName] [ProductVersion] setup is complete.",
     L"Finish", L"Finish", L"Finish", -1},
    {L"UserExit", 7, L"[ProductName] Setup",
     L"[ProductName] setup was cancelled. Your system has not been modified.",
     L"Finish", L"Finish", L"Finish", -2},
    {L"FatalError", 7, L"[ProductName] Setup",
     L"[ProductName] setup ended prematurely because of an error.",
     L"Finish", L"Finish", L"Finish", -3},
};

void addUiControl(MSIHANDLE db, const std::wstring& dialog, const std::wstring& name,
                  const std::wstring& type, int x, int y, int w, int h, int attr,
                  const std::wstring& text, const std::wstring& next) {
    insertRow(db, L"Control",
              {L"Dialog_", L"Control", L"Type", L"X", L"Y", L"Width", L"Height",
               L"Attributes", L"Property", L"Text", L"Control_Next", L"Help"},
              {S(dialog), S(name), S(type), I(x), I(y), I(w), I(h), I(attr),
               Null(), text.empty() ? Null() : S(text),
               next.empty() ? Null() : S(next), Null()});
}

void addUiEvent(MSIHANDLE db, const std::wstring& dialog, const std::wstring& control,
                const std::wstring& event, const std::wstring& argument, int ordering) {
    insertRow(db, L"ControlEvent",
              {L"Dialog_", L"Control_", L"Event", L"Argument", L"Condition", L"Ordering"},
              {S(dialog), S(control), S(event), S(argument), S(L"1"), I(ordering)});
}

void buildMinimalUi(MSIHANDLE db, const std::wstring& productName) {
    (void)productName;
    insertRow(db, L"TextStyle",
              {L"TextStyle", L"FaceName", L"Size", L"Color", L"StyleBits"},
              {S(L"DlgFont8"), S(L"Tahoma"), I(8), Null(), Null()});

    for (const auto& d : kMinimalDialogs) {
        insertRow(db, L"Dialog",
                  {L"Dialog", L"HCentering", L"VCentering", L"Width", L"Height",
                   L"Attributes", L"Title", L"Control_First", L"Control_Default",
                   L"Control_Cancel"},
                  {S(d.name), I(50), I(50), I(370), I(270), I(d.attributes), S(d.title),
                   S(d.first), S(d.def), S(d.cancel)});
        const bool isWelcome = std::wstring(d.name) == L"WelcomeDlg";
        const bool isProgress = std::wstring(d.name) == L"ProgressDlg";
        // Windows Installer requires the Control_Next pointers of a dialog to
        // form a single cycle. Previously Description and Cancel both pointed
        // at Next, so the dialog refused to open with error 2810 and the
        // whole install aborted ("unexpected error installing this package").
        addUiControl(db, d.name, L"Title", L"Text", 20, 20, 330, 40, 196611,
                     L"{\\DlgFont8}" + std::wstring(d.title), L"Description");
        addUiControl(db, d.name, L"Description", L"Text", 20, 70, 330, 90, 196611,
                     std::wstring(d.text), isWelcome ? L"Cancel" : d.first);
        addUiControl(db, d.name, L"BottomLine", L"Line", 0, 234, 370, 0, 1, L"", L"");

        if (isProgress) {
            addUiControl(db, d.name, L"ActionText", L"Text", 20, 175, 330, 12, 3,
                         L"", L"");
            addUiControl(db, d.name, L"ProgressBar", L"ProgressBar", 20, 195, 330, 12,
                         65537, L"Progress done", L"");
            addUiControl(db, d.name, L"Cancel", L"PushButton", 304, 243, 56, 17, 3,
                         L"Cancel", L"Title");
            addUiEvent(db, d.name, L"Cancel", L"EndDialog", L"Exit", 1);
            // Without these subscriptions the modeless progress dialog shows
            // a frozen bar and an empty action line.
            addUiEvent(db, d.name, L"ActionText", L"ActionText", L"[ActionText]", 1);
            addUiEvent(db, d.name, L"ProgressBar", L"SetProgress", L"[Progress]", 1);
        } else if (isWelcome) {
            addUiControl(db, d.name, L"Next", L"PushButton", 236, 243, 56, 17, 3,
                         L"&Install", L"Title");
            addUiControl(db, d.name, L"Cancel", L"PushButton", 304, 243, 56, 17, 3,
                         L"Cancel", L"Next");
            addUiEvent(db, d.name, L"Next", L"EndDialog", L"Return", 1);
            addUiEvent(db, d.name, L"Cancel", L"EndDialog", L"Exit", 1);
        } else {
            addUiControl(db, d.name, L"Finish", L"PushButton", 236, 243, 56, 17, 3,
                         L"&Finish", L"Title");
            addUiEvent(db, d.name, L"Finish", L"EndDialog", L"Exit", 1);
        }
    }
}

// ---------------------------------------------------------------------------
// Signing (optional, uses signtool.exe from the Windows SDK)
// ---------------------------------------------------------------------------

std::wstring quoteArg(const std::wstring& s) {
    std::wstring q = L"\"";
    for (size_t i = 0; i < s.size(); ++i) {
        size_t bs = 0;
        while (i < s.size() && s[i] == L'\\') { ++bs; ++i; }
        if (i == s.size()) { q.append(bs * 2, L'\\'); break; }
        if (s[i] == L'"') { q.append(bs * 2 + 1, L'\\'); q += L'"'; }
        else { q.append(bs, L'\\'); q += s[i]; }
    }
    q += L'"';
    return q;
}

std::wstring findSignTool(const std::wstring& explicitPath) {
    if (!explicitPath.empty()) {
        std::wstring p = util::path::full(explicitPath);
        if (!util::path::exists(p))
            util::fail(L"signtool.exe not found: " + p);
        return p;
    }
    wchar_t found[MAX_PATH] = {};
    if (SearchPathW(nullptr, L"signtool.exe", nullptr, MAX_PATH, found, nullptr))
        return found;

    wchar_t pf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"ProgramFiles(x86)", pf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH)
        n = GetEnvironmentVariableW(L"ProgramFiles", pf, MAX_PATH);
    if (n && n < MAX_PATH) {
        std::wstring pattern = std::wstring(pf) + L"\\Windows Kits\\10\\bin\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                for (const wchar_t* arch : {L"x64", L"arm64", L"x86"}) {
                    std::wstring p = std::wstring(pf) + L"\\Windows Kits\\10\\bin\\" +
                                     fd.cFileName + L"\\" + arch + L"\\signtool.exe";
                    if (util::path::exists(p)) { FindClose(h); return p; }
                }
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    util::fail(L"Cannot find signtool.exe. Install the Windows SDK or pass --sign-tool <path>.");
    return L"";
}

bool hasDigitalSignature(const std::wstring& msiPath) {
    msi::Db db(msiPath);
    for (const std::wstring& s : db.streamNames())
        if (s == L"\u0005DigitalSignature") return true;
    return false;
}

void signPackage(const std::wstring& msiPath, const Options& opt) {
    std::wstring tool = findSignTool(opt.signTool);
    std::wstring cert = util::path::full(opt.signCert);
    if (!util::path::exists(cert)) util::fail(L"Signing certificate not found: " + cert);

    std::wstring cmd = quoteArg(tool) + L" sign /fd SHA256 /f " + quoteArg(cert);
    if (!opt.signPassword.empty()) cmd += L" /p " + quoteArg(opt.signPassword);
    if (!opt.signTimestamp.empty())
        cmd += L" /tr " + quoteArg(opt.signTimestamp) + L" /td SHA256";
    cmd += L" " + quoteArg(msiPath);

    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok)
        util::fail(L"Cannot run signtool.exe: " + util::winError(GetLastError()));
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 180000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    if (rc != 0)
        util::fail(util::fmt(L"signtool.exe failed (exit %lu)",
                             static_cast<unsigned long>(rc)));
    if (!hasDigitalSignature(msiPath))
        util::fail(L"signtool.exe finished but the package has no DigitalSignature stream");
}

}  // namespace

Stats pack(const Options& opt) {
    Stats st;
    std::wstring inDir = util::path::full(opt.inputDir);
    while (inDir.size() > 3 && (inDir.back() == L'\\' || inDir.back() == L'/'))
        inDir.pop_back();
    const std::wstring outMsi = util::path::full(opt.outputMsi);
    st.output = outMsi;

    if (!util::path::isDirectory(inDir))
        util::fail(L"Input directory not found: " + inDir);
    if (util::path::exists(outMsi) && !opt.force && !opt.dryRun)
        util::fail(L"Output already exists (use --force to overwrite): " + outMsi);
    if (util::iequals(util::path::ext(outMsi), L".msp"))
        util::fail(L"Output must be an .msi file: " + outMsi);

    // Manifest values fill in the options the command line did not set.
    Options o = opt;
    Manifest cfgData;
    if (!o.configFile.empty()) {
        cfgData = loadManifest(util::path::full(o.configFile));
        if (!o.uiSet && o.ui == L"none" && !cfgData.ui.empty()) o.ui = cfgData.ui;
        if (!o.cabSizeSet && o.cabSize == 0 && cfgData.cabSize != 0) o.cabSize = cfgData.cabSize;
        if (!o.largeFilesSet && !cfgData.largeFiles.empty())
            o.largeFiles = cfgData.largeFiles;
        if (!o.signCertSet && o.signCert.empty()) {
            o.signCert = cfgData.signCert;
            o.signPassword = cfgData.signPassword;
            o.signTimestamp = cfgData.signTimestamp;
            o.signTool = cfgData.signTool;
        }
        if (!cfgData.keepEmptyDirs) o.keepEmptyDirs = false;
    }
    o.ui = util::toLower(o.ui);
    if (o.ui != L"none" && o.ui != L"basic" && o.ui != L"minimal")
        util::fail(L"--ui must be none, basic or minimal: " + o.ui);
    o.largeFiles = util::toLower(o.largeFiles);
    if (o.largeFiles != L"layout" && o.largeFiles != L"error")
        util::fail(L"--large-files must be layout or error: " + o.largeFiles);
    std::wstring productName = o.productName.empty()
                                   ? util::path::fileName(inDir)
                                   : o.productName;
    if (productName.empty()) productName = L"Application";
    std::wstring productCode = normalizeGuid(o.productCode);
    std::wstring upgradeCode = normalizeGuid(o.upgradeCode);
    std::wstring platform = o.platform;
    if (!util::iequals(platform, L"x64") && !util::iequals(platform, L"Intel"))
        util::fail(L"--platform must be x64 or Intel: " + platform);
    const std::wstring rootDirKey =
        util::iequals(platform, L"x64") ? L"ProgramFiles64Folder" : L"ProgramFilesFolder";

    std::vector<Entry> files;
    std::vector<std::wstring> allDirs;
    const bool selecting = !o.inputFiles.empty();
    if (selecting) {
        allDirs.push_back(L"");        // the root, exactly like walk() does
        selectFiles(inDir, o.inputFiles, files, allDirs, o.verbose);
        util::outLine(L"selected " + std::to_wstring(files.size()) +
                      L" file(s) from " + std::to_wstring(o.inputFiles.size()) +
                      L" entr" + (o.inputFiles.size() == 1 ? L"y" : L"ies") +
                      L" of " + inDir);
    } else {
        walk(inDir, L"", files, allDirs, o.verbose);
    }
    if (files.empty() && !o.keepEmptyDirs)
        util::fail(selecting ? L"Selection contains no files: " + inDir
                             : L"Input directory contains no files: " + inDir);
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        if (a.relDir != b.relDir) return a.relDir < b.relDir;
        return a.fileName < b.fileName;
    });

    // Files above the 32-bit FileSize limit cannot go through FCI (its seek
    // callback takes a 32-bit offset) and cannot be represented in the File
    // table. Keep them as uncompressed companion files in a layout tree.
    std::vector<Entry> cabFiles, externalFiles;
    for (auto& f : files) {
        if (f.size > 0x7FFFFFFFULL) {
            if (o.largeFiles == L"error")
                util::fail(L"File exceeds the 2 GiB MSI FileSize limit: " + f.fullPath +
                           L" (use --large-files layout)");
            f.external = true;
            externalFiles.push_back(f);
        } else cabFiles.push_back(f);
    }
    if (!externalFiles.empty())
        util::errLine(L"[warn] " + std::to_wstring(externalFiles.size()) +
                      L" file(s) exceed 2 GiB: stored as uncompressed companion files; "
                      L"File.FileSize is 0 (MSI stores it as a 32-bit integer).");

    // File.Sequence: compressed files first (in CAB order), then layout files.
    for (size_t i = 0; i < cabFiles.size(); ++i)
        cabFiles[i].fileKey = L"F" + std::to_wstring(i + 1);
    for (size_t i = 0; i < externalFiles.size(); ++i)
        externalFiles[i].fileKey = L"F" + std::to_wstring(cabFiles.size() + i + 1);
    std::vector<Entry> ordered = cabFiles;
    ordered.insert(ordered.end(), externalFiles.begin(), externalFiles.end());

    // Split compressed files into cabinets of at most --cab-size bytes of
    // uncompressed input. An oversized single file always gets its own CAB.
    std::vector<std::vector<size_t>> groups;
    if (!cabFiles.empty()) {
        std::vector<size_t> cur;
        unsigned long long curBytes = 0;
        for (size_t i = 0; i < cabFiles.size(); ++i) {
            if (!cur.empty() && o.cabSize != 0 &&
                curBytes + cabFiles[i].size > o.cabSize) {
                groups.push_back(cur);
                cur.clear();
                curBytes = 0;
            }
            cur.push_back(i);
            curBytes += cabFiles[i].size;
        }
        if (!cur.empty()) groups.push_back(cur);
    }

    LayoutBuilder layout;
    layout.rootKey = rootDirKey;
    for (const auto& d : allDirs) layout.ensure(d, productName);
    for (auto& kv : layout.dirs) {
        for (const auto& f : ordered) {
            if (util::iequals(f.relDir, kv.first)) {
                kv.second.keyPath = f.fileKey;
                break;
            }
        }
    }

    st.files = ordered.size();
    st.directories = layout.dirs.size();
    st.components = layout.dirs.size();
    st.cabinets = groups.size();
    st.externalFiles = externalFiles.size();
    st.features = 1 + cfgData.features.size();
    st.shortcuts = cfgData.shortcuts.size();
    st.registry = cfgData.registry.size();
    st.services = cfgData.services.size();
    st.environment = cfgData.environment.size();
    for (const auto& kv : layout.dirs)
        if (kv.second.keyPath.empty()) ++st.emptyDirs;

    util::outLine(util::fmt(L"Packing %llu file(s) in %llu directory(ies) -> %s",
                            static_cast<unsigned long long>(st.files),
                            static_cast<unsigned long long>(st.directories),
                            outMsi.c_str()));
    util::outLine(L"Product   : " + productName + L" " + o.version);
    util::outLine(L"Codes     : ProductCode=" + productCode + L" UpgradeCode=" + upgradeCode);
    util::outLine(L"Platform  : " + platform + L";" + std::to_wstring(o.language));
    util::outLine(util::fmt(L"Cabinets  : %llu embedded CAB(s), %llu feature(s), "
                            L"%llu empty dir(s), %llu >2 GiB layout file(s)",
                            static_cast<unsigned long long>(st.cabinets),
                            static_cast<unsigned long long>(st.features),
                            static_cast<unsigned long long>(st.emptyDirs),
                            static_cast<unsigned long long>(st.externalFiles)));
    if (o.dryRun) {
        util::outLine(L"[dry-run] No cabinet or database was written.");
        return st;
    }

    // --- 1. Build the embedded cabinet(s) ----------------------------------
    util::TempDir temp(L"msitool_pack");
    std::vector<std::wstring> cabPaths(groups.size());
    for (size_t g = 0; g < groups.size(); ++g) {
        cabPaths[g] = util::path::join(temp.path(), L"cab" + std::to_wstring(g + 1) + L".cab");
        std::vector<std::wstring> sources, cabNames;
        for (size_t idx : groups[g]) {
            sources.push_back(cabFiles[idx].fullPath);
            cabNames.push_back(cabFiles[idx].fileKey);
        }
        createCabinet(cabPaths[g], sources, cabNames, o.verbose);
        st.cabBytes += fileSize(cabPaths[g]);
        util::verboseLine(o.verbose, L"built " + cabPaths[g]);
    }
    if (!groups.empty())
        util::outLine(L"Cabinet   : " + util::fmt(L"%llu bytes in %llu CAB(s) (MSZIP)",
                                                 st.cabBytes,
                                                 static_cast<unsigned long long>(st.cabinets)));

    // --- 2. Build the MSI database ------------------------------------------
    if (util::path::exists(outMsi)) DeleteFileW(outMsi.c_str());
    std::wstring outParent = util::path::dirName(outMsi);
    if (!outParent.empty()) util::path::makeDirs(outParent);

    const bool needCreateFolders = o.keepEmptyDirs && st.emptyDirs > 0;
    const bool needShortcuts = !cfgData.shortcuts.empty();
    const bool needRegistry = !cfgData.registry.empty();
    const bool needServices = !cfgData.services.empty();
    const bool needEnvironment = !cfgData.environment.empty();
    const bool needUi = o.ui != L"none";
    const bool needDialogs = o.ui == L"minimal";

    UINT verifyRc = ERROR_SUCCESS;
    {
        msi::Handle db;
        util::checkMsi(MsiOpenDatabaseW(outMsi.c_str(), MSIDBOPEN_CREATE, db.out()),
                       L"MsiOpenDatabaseW(create) " + outMsi);
        MSIHANDLE h = db.get();

        for (const wchar_t* ddl : kSchema) execSql(h, ddl);
        std::unordered_set<std::wstring> created;
        auto create = [&](const wchar_t* ddl, const std::wstring& table) {
            if (created.insert(table).second) execSql(h, ddl);
        };
        if (needCreateFolders) create(kExtraSchema[0], L"CreateFolder");
        if (needShortcuts) { create(kExtraSchema[1], L"Shortcut");
                             create(kExtraSchema[2], L"Icon"); }
        if (needRegistry)   create(kExtraSchema[3], L"Registry");
        if (needServices) { create(kExtraSchema[4], L"ServiceInstall");
                            create(kExtraSchema[5], L"ServiceControl"); }
        if (needEnvironment) create(kExtraSchema[6], L"Environment");
        if (needUi)          create(kExtraSchema[7], L"InstallUISequence");
        if (needDialogs) {
            create(kExtraSchema[8], L"Dialog");
            create(kExtraSchema[9], L"Control");
            create(kExtraSchema[10], L"ControlEvent");
            create(kExtraSchema[11], L"TextStyle");
        }

        // Directory table: standard roots + one row per source directory.
        insertRow(h, L"Directory", {L"Directory", L"Directory_Parent", L"DefaultDir"},
                  {S(L"TARGETDIR"), Null(), S(L"SourceDir")});
        insertRow(h, L"Directory", {L"Directory", L"Directory_Parent", L"DefaultDir"},
                  {S(rootDirKey), S(L"TARGETDIR"), S(L".")});
        for (const auto& kv : layout.dirs) {
            const DirInfo& d = kv.second;
            insertRow(h, L"Directory", {L"Directory", L"Directory_Parent", L"DefaultDir"},
                      {S(d.key), S(d.parentKey), S(d.leaf)});
        }

        // Extra directories for shortcut folders (standard Windows Installer
        // folders resolve by their well-known Directory key).
        std::unordered_set<std::wstring> knownDirs;
        knownDirs.insert(L"TARGETDIR");
        knownDirs.insert(util::toLower(rootDirKey));
        for (const auto& kv : layout.dirs) knownDirs.insert(util::toLower(kv.second.key));
        int customDirNo = 0;
        auto ensureExtraDir = [&](const std::wstring& key,
                                  const std::wstring& parent,
                                  const std::wstring& leaf) {
            if (knownDirs.insert(util::toLower(key)).second)
                insertRow(h, L"Directory",
                          {L"Directory", L"Directory_Parent", L"DefaultDir"},
                          {S(key), parent.empty() ? Null() : S(parent), S(leaf)});
        };
        static const wchar_t* kStandardFolders[] = {
            L"ProgramMenuFolder", L"StartMenuFolder", L"DesktopFolder",
            L"StartupFolder", L"AppDataFolder", L"LocalAppDataFolder",
            L"CommonAppDataFolder", L"PersonalFolder", L"SendToFolder",
            L"TemplatesFolder", L"FavoritesFolder", L"FontsFolder",
            L"MyPicturesFolder", L"AdminToolsFolder", L"TempFolder",
        };
        auto isStandardFolder = [&](const std::wstring& s) {
            for (const wchar_t* f : kStandardFolders)
                if (util::iequals(s, f)) return true;
            return false;
        };
        auto shortcutDirKey = [&](const std::wstring& raw) {
            if (raw.empty()) return std::wstring(L"INSTALLFOLDER");
            if (isStandardFolder(raw)) {
                ensureExtraDir(raw, L"TARGETDIR", L".");
                return raw;
            }
            for (const auto& kv : layout.dirs)
                if (util::iequals(raw, kv.second.key) || util::iequals(raw, kv.first))
                    return kv.second.key;
            std::wstring key = L"SD" + std::to_wstring(++customDirNo);
            ensureExtraDir(key, L"INSTALLFOLDER", sanitizeName(raw));
            return key;
        };

        // Component: one per directory, keyed by the first file it contains.
        const int compAttrs = util::iequals(platform, L"x64") ? 256 : 0;
        for (const auto& kv : layout.dirs) {
            const DirInfo& d = kv.second;
            insertRow(h, L"Component",
                      {L"Component", L"ComponentId", L"Directory_", L"Attributes",
                       L"Condition", L"KeyPath"},
                      {S(d.componentKey), S(newGuid()), S(d.key), I(compAttrs), Null(),
                       d.keyPath.empty() ? Null() : S(d.keyPath)});
        }
        if (needCreateFolders) {
            for (const auto& kv : layout.dirs) {
                if (!kv.second.keyPath.empty()) continue;
                insertRow(h, L"CreateFolder", {L"Directory_", L"Component_"},
                          {S(kv.second.key), S(kv.second.componentKey)});
            }
        }

        // File: CAB-internal name is the File table primary key.
        for (size_t i = 0; i < ordered.size(); ++i) {
            const Entry& f = ordered[i];
            auto it = layout.dirs.find(util::toLower(f.relDir));
            if (it == layout.dirs.end()) util::fail(L"internal layout error");
            insertRow(h, L"File",
                      {L"File", L"Component_", L"FileName", L"FileSize", L"Version",
                       L"Language", L"Attributes", L"Sequence"},
                      {S(f.fileKey), S(it->second.componentKey), S(sanitizeName(f.fileName)),
                       f.external ? I(0) : I(static_cast<int>(f.size)), Null(), Null(),
                       f.external ? I(8192) : I(0),
                       I(static_cast<int>(i + 1))});
        }

        // Media: one row per embedded cabinet + one uncompressed row for the
        // > 2 GiB companion files (kept last so sequence ranges stay contiguous).
        for (size_t g = 0; g < groups.size(); ++g) {
            int lastSeq = static_cast<int>(groups[g].back() + 1);
            insertRow(h, L"Media",
                      {L"DiskId", L"LastSequence", L"DiskPrompt", L"Cabinet",
                       L"VolumeLabel", L"Source"},
                      {I(static_cast<int>(g + 1)), I(lastSeq), Null(),
                       S(L"#cab" + std::to_wstring(g + 1) + L".cab"), Null(), Null()});
        }
        if (!externalFiles.empty()) {
            insertRow(h, L"Media",
                      {L"DiskId", L"LastSequence", L"DiskPrompt", L"Cabinet",
                       L"VolumeLabel", L"Source"},
                      {I(static_cast<int>(groups.size() + 1)),
                       I(static_cast<int>(ordered.size())), Null(), Null(), Null(), Null()});
        }

        // Features: a default MainFeature plus any manifest-defined features.
        std::vector<std::wstring> featureIds;
        featureIds.push_back(L"MainFeature");
        for (const auto& f : cfgData.features) featureIds.push_back(f.id);
        auto featureOf = [&](const std::wstring& rel) {
            std::wstring best;
            size_t bestLen = 0;
            for (const auto& f : cfgData.features) {
                for (const std::wstring& d : splitList(f.dirs)) {
                    std::wstring p = util::toLower(normRel(d));
                    std::wstring r = util::toLower(normRel(rel));
                    if (p.empty() || r == p ||
                        (r.size() > p.size() && r.compare(0, p.size(), p) == 0 &&
                         r[p.size()] == L'\\')) {
                        if (p.size() >= bestLen) { best = f.id; bestLen = p.size(); }
                    }
                }
            }
            return best.empty() ? std::wstring(L"MainFeature") : best;
        };
        insertRow(h, L"Feature",
                  {L"Feature", L"Feature_Parent", L"Title", L"Description", L"Display",
                   L"Level", L"Directory_", L"Attributes"},
                  {S(L"MainFeature"), Null(), S(productName), S(productName), Null(),
                   I(1), S(L"INSTALLFOLDER"), I(0)});
        for (const auto& f : cfgData.features) {
            insertRow(h, L"Feature",
                      {L"Feature", L"Feature_Parent", L"Title", L"Description", L"Display",
                       L"Level", L"Directory_", L"Attributes"},
                      {S(f.id), Null(), S(f.title), S(f.description),
                       f.display ? I(f.display) : Null(), I(f.level),
                       S(L"INSTALLFOLDER"), I(f.attributes)});
        }
        std::set<std::pair<std::wstring, std::wstring>> fc;
        for (const auto& kv : layout.dirs)
            fc.insert({featureOf(kv.first), kv.second.componentKey});
        for (const auto& p : fc)
            insertRow(h, L"FeatureComponents", {L"Feature_", L"Component_"},
                      {S(p.first), S(p.second)});

        // Resolve a manifest "component" (a rel path, a File key, a Component
        // key or a Directory key) to a Component table key.
        auto componentFor = [&](const std::wstring& spec,
                                const std::wstring& fallbackFile) {
            std::wstring want = normRel(spec);
            if (!want.empty()) {
                for (const auto& kv : layout.dirs) {
                    if (util::iequals(want, kv.first) ||
                        util::iequals(want, kv.second.key) ||
                        util::iequals(want, kv.second.componentKey))
                        return kv.second.componentKey;
                }
                for (const auto& f : ordered)
                    if (util::iequals(want, f.fileKey) ||
                        util::iequals(want, normRel((f.relDir.empty() ? L"" : f.relDir + L"\\") +
                                                    f.fileName)))
                        return layout.dirs[util::toLower(f.relDir)].componentKey;
            }
            if (!fallbackFile.empty()) {
                std::wstring wantFile = normRel(fallbackFile);
                for (const auto& f : ordered)
                    if (util::iequals(wantFile, f.fileKey) ||
                        util::iequals(wantFile, normRel((f.relDir.empty() ? L"" : f.relDir + L"\\") +
                                                        f.fileName)))
                        return layout.dirs[util::toLower(f.relDir)].componentKey;
            }
            return layout.dirs[L""].componentKey;
        };

        // File lookup used by shortcuts / services.
        std::unordered_map<std::wstring, size_t> fileByRel;
        for (size_t i = 0; i < ordered.size(); ++i) {
            std::wstring rel = normRel((ordered[i].relDir.empty()
                                            ? L"" : ordered[i].relDir + L"\\") +
                                       ordered[i].fileName);
            fileByRel[util::toLower(rel)] = i;
            fileByRel[util::toLower(ordered[i].fileKey)] = i;
        }
        auto targetFor = [&](const std::wstring& raw, std::wstring& comp,
                             std::wstring& wkdir) {
            std::wstring t = normRel(raw);
            auto it = fileByRel.find(util::toLower(t));
            if (it != fileByRel.end()) {
                const Entry& f = ordered[it->second];
                comp = layout.dirs[util::toLower(f.relDir)].componentKey;
                wkdir = layout.dirs[util::toLower(f.relDir)].key;
                return L"[#" + f.fileKey + L"]";
            }
            if (!t.empty() && (t.front() == L'[' || t.front() == L'#'))
                return t;
            return L"[INSTALLFOLDER]" + t;
        };

        // Shortcuts + icons.
        for (const auto& d : cfgData.shortcuts) {
            std::wstring comp, wkdir;
            std::wstring target = targetFor(d.target, comp, wkdir);
            if (comp.empty()) comp = componentFor(d.component, d.target);
            if (!d.wkdir.empty()) {
                for (const auto& kv : layout.dirs)
                    if (util::iequals(d.wkdir, kv.first) ||
                        util::iequals(d.wkdir, kv.second.key)) wkdir = kv.second.key;
            }
            std::wstring dirKey = shortcutDirKey(d.dir);
            std::wstring iconKey;
            if (!d.icon.empty()) {
                std::wstring iconPath = util::path::isDirectory(d.icon)
                                            ? d.icon : util::path::full(d.icon);
                if (!util::path::exists(iconPath))
                    iconPath = util::path::join(inDir, d.icon);
                if (!util::path::exists(iconPath)) {
                    util::errLine(L"[warn] shortcut icon not found: " + d.icon);
                } else {
                    iconKey = toIdentifier(d.id + L"Icon");
                    insertStreamRow(h, L"Icon", L"Name", L"Data", iconKey, iconPath);
                }
            }
            insertRow(h, L"Shortcut",
                      {L"Shortcut", L"Directory_", L"Name", L"Component_", L"Target",
                       L"Arguments", L"Description", L"Hotkey", L"Icon_", L"IconIndex",
                       L"ShowCmd", L"WkDir", L"DisplayResourceDLL", L"DisplayResourceId",
                       L"DescriptionResourceDLL", L"DescriptionResourceId"},
                      {S(d.id), S(dirKey), S(sanitizeName(d.name)), S(comp), S(target),
                       d.args.empty() ? Null() : S(d.args),
                       d.description.empty() ? Null() : S(d.description), Null(),
                       iconKey.empty() ? Null() : S(iconKey), iconKey.empty() ? Null() : I(0),
                       Null(), wkdir.empty() ? Null() : S(wkdir), Null(), Null(), Null(),
                       Null()});
        }

        // Registry values.
        for (const auto& d : cfgData.registry) {
            int root = -1;
            std::wstring r = util::toLower(d.root);
            if (r == L"hklm" || r == L"localmachine") root = 2;
            else if (r == L"hkcu" || r == L"currentuser") root = 1;
            else if (r == L"hkcr" || r == L"classesroot") root = 0;
            else if (r == L"hku" || r == L"users") root = 3;
            else if (r != L"auto") root = _wtoi(d.root.c_str());
            std::wstring value = d.value;
            std::wstring name = d.name;
            if (d.type == L"integer") value = L"#" + value;
            else if (d.type == L"binary") value = L"#x" + value;
            else if (d.type == L"expand") value = L"#%" + value;
            else if (d.type == L"multi") value = d.value;
            else if (d.type == L"key") { name = L"+"; value.clear(); }
            else if (d.type == L"keydelete") { name = L"-"; value.clear(); }
            else if (d.type == L"keycreatedelete") { name = L"*"; value.clear(); }
            insertRow(h, L"Registry",
                      {L"Registry", L"Root", L"Key", L"Name", L"Value", L"Component_"},
                      {S(d.id), I(root), S(d.key), name.empty() ? Null() : S(name),
                       value.empty() ? Null() : S(value),
                       S(componentFor(d.component, L""))});
        }

        // Services.
        for (const auto& d : cfgData.services) {
            int stype = 0x10, start = 3, error = 1;
            if (d.type == L"share") stype = 0x20;
            if (d.type == L"interactive") stype = 0x110;
            if (d.start == L"auto") start = 2;
            else if (d.start == L"disabled") start = 4;
            else if (d.start == L"system") start = 1;
            if (d.error == L"ignore") error = 0;
            else if (d.error == L"severe") error = 2;
            else if (d.error == L"critical") error = 3;
            std::wstring comp, wkdir;
            std::wstring image = d.image.empty() ? L"" : targetFor(d.image, comp, wkdir);
            std::wstring compKey = componentFor(d.component, d.image);
            insertRow(h, L"ServiceInstall",
                      {L"ServiceInstall", L"Name", L"DisplayName", L"ServiceType",
                       L"StartType", L"ErrorControl", L"LoadOrderGroup", L"Dependencies",
                       L"StartName", L"Password", L"Arguments", L"Component_", L"Description"},
                      {S(d.id), S(d.name),
                       d.displayName.empty() ? Null() : S(d.displayName), I(stype),
                       I(start), I(error), Null(),
                       d.dependencies.empty() ? Null() : S(d.dependencies),
                       d.account.empty() ? Null() : S(d.account),
                       d.password.empty() ? Null() : S(d.password),
                       d.args.empty() ? Null() : S(d.args), S(compKey),
                       d.description.empty() ? Null() : S(d.description)});
            (void)image;
            insertRow(h, L"ServiceControl",
                      {L"ServiceControl", L"Name", L"Event", L"Arguments", L"Wait",
                       L"Component_"},
                      {S(toIdentifier(d.id + L"Control")), S(d.name), I(163), Null(), I(1),
                       S(compKey)});
        }

        // Environment variables.
        for (const auto& d : cfgData.environment) {
            std::wstring prefix;
            if (d.action == L"create") prefix = L"+-";
            else if (d.action == L"remove" || d.action == L"delete") prefix = L"!-";
            else prefix = L"=-";
            if (parseBool(d.system, false)) prefix += L"*";
            std::wstring value = d.value;
            if (d.action == L"append") value = L"[~]" + d.separator + value;
            else if (d.action == L"prepend") value = value + d.separator + L"[~]";
            insertRow(h, L"Environment",
                      {L"Environment", L"Name", L"Value", L"Component_"},
                      {S(d.id), S(prefix + d.name), value.empty() ? Null() : S(value),
                       S(componentFor(d.component, L""))});
        }

        auto prop = [&](const wchar_t* name, const std::wstring& value) {
            insertRow(h, L"Property", {L"Property", L"Value"}, {S(name), S(value)});
        };
        prop(L"ProductName", productName);
        prop(L"ProductVersion", o.version);
        prop(L"ProductCode", productCode);
        prop(L"UpgradeCode", upgradeCode);
        prop(L"Manufacturer", o.manufacturer);
        prop(L"ProductLanguage", std::to_wstring(o.language));
        prop(L"INSTALLLEVEL", L"1");
        prop(L"ALLUSERS", L"1");
        if (needDialogs) prop(L"DefaultUIFont", L"DlgFont8");

        for (const auto& a : kSequence)
            insertRow(h, L"InstallExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});
        for (const auto& a : kAdminSequence)
            insertRow(h, L"AdminExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});
        if (needUi) {
            const SequenceAction* seq = needDialogs ? kUiSequenceMinimal : kUiSequenceBasic;
            size_t count = needDialogs ? sizeof(kUiSequenceMinimal) / sizeof(SequenceAction)
                                       : sizeof(kUiSequenceBasic) / sizeof(SequenceAction);
            for (size_t i = 0; i < count; ++i)
                insertRow(h, L"InstallUISequence", {L"Action", L"Condition", L"Sequence"},
                          {S(seq[i].name), Null(), I(seq[i].seq)});
        }
        if (needDialogs) buildMinimalUi(h, productName);

        // Embedded cabinets as _Streams rows.
        for (size_t g = 0; g < cabPaths.size(); ++g)
            insertStreamRow(h, L"_Streams", L"Name", L"Data",
                            L"cab" + std::to_wstring(g + 1) + L".cab", cabPaths[g]);

        // SummaryInformation (must be persisted before commit).
        msi::Handle sum;
        util::checkMsi(MsiGetSummaryInformationW(h, nullptr, 20, sum.out()),
                       L"MsiGetSummaryInformationW");
        auto setStr = [&](UINT id, const std::wstring& v) {
            util::checkMsi(MsiSummaryInfoSetPropertyW(sum.get(), id, 30, 0, nullptr,
                                                      v.c_str()),
                           L"summary string " + std::to_wstring(id));
        };
        auto setInt = [&](UINT id, int v) {
            util::checkMsi(MsiSummaryInfoSetPropertyW(sum.get(), id, 3, v, nullptr, nullptr),
                           L"summary integer " + std::to_wstring(id));
        };
        setStr(2, productName);                                   // Title
        setStr(3, productName);                                   // Subject
        setStr(4, o.manufacturer);                                // Author
        setStr(7, platform + L";" + std::to_wstring(o.language));  // Template
        setStr(9, productCode);                                   // Revision/PackageCode
        setInt(14, 200);                                          // min. installer 2.0
        setInt(15, 2);                                            // long names + compressed
        setStr(18, L"msitool");                                   // Creating application
        setInt(19, 2);                                            // read-only recommended
        util::checkMsi(MsiSummaryInfoPersist(sum.get()), L"MsiSummaryInfoPersist");

        util::checkMsi(MsiDatabaseCommit(h), L"MsiDatabaseCommit");
    }  // closes all MSI handles, flushing the file

    // --- 3. Companion layout tree for > 2 GiB files ------------------------
    if (!externalFiles.empty()) {
        std::wstring root = util::path::join(outParent, sanitizeName(productName));
        for (const auto& f : externalFiles) {
            std::wstring dst = f.relDir.empty()
                                   ? util::path::join(root, f.fileName)
                                   : util::path::join(util::path::join(root, f.relDir),
                                                      f.fileName);
            std::wstring dstDir = util::path::dirName(dst);
            if (!dstDir.empty()) util::path::makeDirs(dstDir);
            util::checkWin(util::path::copyFile(f.fullPath, dst, true),
                           L"copy large file to layout " + dst);
            util::verboseLine(o.verbose, L"layout " + dst);
        }
        util::outLine(L"Layout    : " + root + L" (ship this folder with the MSI)");
    }

    // --- 4. Optional Authenticode signature --------------------------------
    if (!o.signCert.empty()) {
        signPackage(outMsi, o);
        st.signedPackage = true;
        util::outLine(L"Signature : OK (\\x05DigitalSignature stream written)");
    }

    st.msiBytes = fileSize(outMsi);
    verifyRc = MsiVerifyPackageW(outMsi.c_str());
    util::outLine(L"MSI       : " + util::fmt(L"%llu bytes", st.msiBytes));
    if (verifyRc == ERROR_SUCCESS) {
        util::outLine(L"MsiVerifyPackage: OK (Windows Installer accepts this package)");
    } else {
        util::errLine(L"[warn] MsiVerifyPackage: " + util::msiError(verifyRc));
    }
    util::outLine(L"Done      : " + outMsi);
    return st;
}

}  // namespace pack
