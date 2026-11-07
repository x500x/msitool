#include "pack.h"
#include "msidb.h"

#include <fci.h>

#include <objbase.h>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <map>
#include <unordered_map>

namespace pack {

namespace {

// ---------------------------------------------------------------------------
// FCI (cabinet.dll) callbacks - creates one MSZIP cabinet.
// Path strings handed to FCI are UTF-8; our callbacks convert back to UTF-16,
// so source paths containing spaces / non-ASCII characters work.
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

FNFCIFILEPLACED(fciFilePlaced) {
    (void)pccab; (void)pszFile; (void)cbFile; (void)fContinuation; (void)pv;
    return 0;
}

FNFCIGETNEXTCABINET(fciNextCabinet) {
    (void)cbPrevCab; (void)pv;
    if (!g_fci || g_fci->cabStarted) return FALSE;  // only one cabinet
    g_fci->cabStarted = true;
    copyChars(pccab->szCab, sizeof(pccab->szCab), g_fci->cabNameUtf8);
    copyChars(pccab->szCabPath, sizeof(pccab->szCabPath), g_fci->cabDirUtf8);
    copyChars(pccab->szDisk, sizeof(pccab->szDisk), "");
    pccab->setID = 0x5A5A;
    pccab->iCab = 1;
    pccab->iDisk = 1;
    return TRUE;
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
    ccab.cb = 0x7FFFFFFF;             // one cabinet, no media splitting
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

struct SequenceAction {
    const wchar_t* name;
    int            seq;
};
const SequenceAction kSequence[] = {
    {L"CostInitialize",   800}, {L"FileCost",         900}, {L"CostFinalize",   1000},
    {L"InstallValidate", 1400}, {L"InstallInitialize",1500}, {L"ProcessComponents",1600},
    {L"UnpublishFeatures",1800},{L"RemoveFiles",     3500}, {L"CreateFolders",  3700},
    {L"InstallFiles",    4000}, {L"RegisterProduct", 6100}, {L"PublishFeatures", 6300},
    {L"PublishProduct",  6400}, {L"InstallExecute",  6500}, {L"InstallFinalize",6600},
};

// Administrative install (msiexec /a or ACTION=ADMIN) unpacks the CAB
// without writing registry/ARP state.
const SequenceAction kAdminSequence[] = {
    {L"CostInitialize",     800}, {L"FileCost",           900}, {L"CostFinalize",    1000},
    {L"InstallValidate",   1400}, {L"InstallInitialize", 1500},
    {L"InstallAdminPackage",3900},{L"InstallFiles",      4000},{L"InstallFinalize", 6600},
};

// ---------------------------------------------------------------------------
// Directory enumeration / layout
// ---------------------------------------------------------------------------

struct Entry {
    std::wstring relDir;      // relative directory, "" for the package root
    std::wstring fileName;
    std::wstring fullPath;
    unsigned long long size = 0;
    std::wstring fileKey;     // File table key == name inside the CAB
};

void walk(const std::wstring& dir, const std::wstring& rel,
          std::vector<Entry>& out, bool verbose) {
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
            walk(full, sub, out, verbose);
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

    std::wstring productName = opt.productName.empty()
                                   ? util::path::fileName(inDir)
                                   : opt.productName;
    if (productName.empty()) productName = L"Application";
    std::wstring productCode = normalizeGuid(opt.productCode);
    std::wstring upgradeCode = normalizeGuid(opt.upgradeCode);
    std::wstring platform = opt.platform;
    if (!util::iequals(platform, L"x64") && !util::iequals(platform, L"Intel"))
        util::fail(L"--platform must be x64 or Intel: " + platform);
    const std::wstring rootDirKey =
        util::iequals(platform, L"x64") ? L"ProgramFiles64Folder" : L"ProgramFilesFolder";

    std::vector<Entry> files;
    walk(inDir, L"", files, opt.verbose);
    if (files.empty()) util::fail(L"Input directory contains no files: " + inDir);
    std::sort(files.begin(), files.end(), [](const Entry& a, const Entry& b) {
        if (a.relDir != b.relDir) return a.relDir < b.relDir;
        return a.fileName < b.fileName;
    });
    for (size_t i = 0; i < files.size(); ++i) {
        if (files[i].size > 0x7FFFFFFFULL)
            util::fail(L"File exceeds the 2 GiB MSI FileSize limit: " + files[i].fullPath);
        files[i].fileKey = L"F" + std::to_wstring(i + 1);
    }

    LayoutBuilder layout;
    layout.rootKey = rootDirKey;
    for (const auto& f : files) layout.ensure(f.relDir, productName);
    for (auto& kv : layout.dirs) {
        for (const auto& f : files) {
            if (util::iequals(f.relDir, kv.first)) {
                kv.second.keyPath = f.fileKey;
                break;
            }
        }
    }

    st.files = files.size();
    st.directories = layout.dirs.size();
    st.components = layout.dirs.size();

    util::outLine(util::fmt(L"Packing %llu file(s) in %llu directory(ies) -> %s",
                            static_cast<unsigned long long>(st.files),
                            static_cast<unsigned long long>(st.directories),
                            outMsi.c_str()));
    util::outLine(L"Product   : " + productName + L" " + opt.version);
    util::outLine(L"Codes     : ProductCode=" + productCode + L" UpgradeCode=" + upgradeCode);
    util::outLine(L"Platform  : " + platform + L";" + std::to_wstring(opt.language));
    if (opt.dryRun) {
        util::outLine(L"[dry-run] No cabinet or database was written.");
        return st;
    }

    // --- 1. Build the embedded cabinet -------------------------------------
    util::TempDir temp(L"msitool_pack");
    const std::wstring cabPath = util::path::join(temp.path(), L"cab1.cab");
    std::vector<std::wstring> sources, cabNames;
    for (const auto& f : files) {
        sources.push_back(f.fullPath);
        cabNames.push_back(f.fileKey);
    }
    createCabinet(cabPath, sources, cabNames, opt.verbose);
    st.cabBytes = fileSize(cabPath);
    util::outLine(L"Cabinet   : " + util::fmt(L"%llu bytes", st.cabBytes) +
                  L" (MSZIP, embedded as #cab1.cab)");

    // --- 2. Build the MSI database ------------------------------------------
    if (util::path::exists(outMsi)) DeleteFileW(outMsi.c_str());
    std::wstring outParent = util::path::dirName(outMsi);
    if (!outParent.empty()) util::path::makeDirs(outParent);

    UINT verifyRc = ERROR_SUCCESS;
    {
        msi::Handle db;
        util::checkMsi(MsiOpenDatabaseW(outMsi.c_str(), MSIDBOPEN_CREATE, db.out()),
                       L"MsiOpenDatabaseW(create) " + outMsi);
        MSIHANDLE h = db.get();

        for (const wchar_t* ddl : kSchema) execSql(h, ddl);

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

        // File: CAB-internal name is the File table primary key.
        for (size_t i = 0; i < files.size(); ++i) {
            const Entry& f = files[i];
            auto it = layout.dirs.find(util::toLower(f.relDir));
            if (it == layout.dirs.end()) util::fail(L"internal layout error");
            insertRow(h, L"File",
                      {L"File", L"Component_", L"FileName", L"FileSize", L"Version",
                       L"Language", L"Attributes", L"Sequence"},
                      {S(f.fileKey), S(it->second.componentKey), S(sanitizeName(f.fileName)),
                       I(static_cast<int>(f.size)), Null(), Null(), I(0),
                       I(static_cast<int>(i + 1))});
        }

        insertRow(h, L"Media",
                  {L"DiskId", L"LastSequence", L"DiskPrompt", L"Cabinet",
                   L"VolumeLabel", L"Source"},
                  {I(1), I(static_cast<int>(files.size())), Null(), S(L"#cab1.cab"),
                   Null(), Null()});

        insertRow(h, L"Feature",
                  {L"Feature", L"Feature_Parent", L"Title", L"Description", L"Display",
                   L"Level", L"Directory_", L"Attributes"},
                  {S(L"MainFeature"), Null(), S(productName), S(productName), Null(),
                   I(1), S(L"INSTALLFOLDER"), I(0)});
        for (const auto& kv : layout.dirs)
            insertRow(h, L"FeatureComponents", {L"Feature_", L"Component_"},
                      {S(L"MainFeature"), S(kv.second.componentKey)});

        auto prop = [&](const wchar_t* name, const std::wstring& value) {
            insertRow(h, L"Property", {L"Property", L"Value"}, {S(name), S(value)});
        };
        prop(L"ProductName", productName);
        prop(L"ProductVersion", opt.version);
        prop(L"ProductCode", productCode);
        prop(L"UpgradeCode", upgradeCode);
        prop(L"Manufacturer", opt.manufacturer);
        prop(L"ProductLanguage", std::to_wstring(opt.language));
        prop(L"INSTALLLEVEL", L"1");
        prop(L"ALLUSERS", L"1");

        for (const auto& a : kSequence)
            insertRow(h, L"InstallExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});
        for (const auto& a : kAdminSequence)
            insertRow(h, L"AdminExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});

        // Embedded cabinet as a _Streams row.
        msi::Handle view, rec(MsiCreateRecord(2));
        util::checkMsi(MsiDatabaseOpenViewW(
                           h, L"SELECT `Name`, `Data` FROM `_Streams`", view.out()),
                       L"open _Streams view");
        util::checkMsi(MsiViewExecute(view.get(), 0), L"execute _Streams view");
        util::checkMsi(MsiRecordSetStringW(rec.get(), 1, L"cab1.cab"),
                       L"set stream name");
        util::checkMsi(MsiRecordSetStreamW(rec.get(), 2, cabPath.c_str()),
                       L"set cabinet stream");
        util::checkMsi(MsiViewModify(view.get(), MSIMODIFY_INSERT, rec.get()),
                       L"insert cabinet stream");

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
        setStr(4, opt.manufacturer);                              // Author
        setStr(7, platform + L";" + std::to_wstring(opt.language));  // Template
        setStr(9, productCode);                                   // Revision/PackageCode
        setInt(14, 200);                                          // min. installer 2.0
        setInt(15, 2);                                            // long names + compressed
        setStr(18, L"msitool");                                   // Creating application
        setInt(19, 2);                                            // read-only recommended
        util::checkMsi(MsiSummaryInfoPersist(sum.get()), L"MsiSummaryInfoPersist");

        util::checkMsi(MsiDatabaseCommit(h), L"MsiDatabaseCommit");
    }  // closes all MSI handles, flushing the file

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
