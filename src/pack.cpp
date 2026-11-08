#include "pack.h"

#include "msidb.h"
#include "pack_ui.h"
#include "pack_util.h"
#include "pack_xml.h"

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

std::wstring absOf(const std::wstring& inDir, const std::wstring& spec) {
    return util::path::full(absolutePath(spec) ? spec
                                               : util::path::join(inDir, spec));
}

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

std::wstring parentRel(const std::wstring& rel) {
    std::wstring r = normRel(rel);
    size_t pos = r.find_last_of(L"\\/");
    return pos == std::wstring::npos ? L"" : normRel(r.substr(0, pos));
}

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
        for (std::wstring d = e.relDir; !d.empty(); d = parentRel(d))
            if (seenDir.insert(util::toLower(d)).second) dirs.push_back(d);
    }
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

class LayoutBuilder {
public:
    std::map<std::wstring, DirInfo> dirs;  // lower(relDir) -> info
    std::wstring rootKey = L"ProgramFilesFolder";
    std::wstring installLeaf;

    std::wstring ensure(const std::wstring& rel) {
        std::wstring k = util::toLower(rel);
        auto it = dirs.find(k);
        if (it != dirs.end()) return it->second.key;

        DirInfo d;
        if (rel.empty()) {
            d.key = L"INSTALLFOLDER";
            d.parentKey = rootKey;
            d.leaf = installLeaf == L"." ? L"." : sanitizeName(installLeaf);
        } else {
            size_t pos = rel.rfind(L'\\');
            std::wstring parent = pos == std::wstring::npos ? L"" : rel.substr(0, pos);
            std::wstring leaf = pos == std::wstring::npos ? rel : rel.substr(pos + 1);
            std::wstring parentKey = ensure(parent);
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

// ---------------------------------------------------------------------------
// Install location (--install-dir)
// ---------------------------------------------------------------------------

struct DirRow {
    std::wstring key;
    std::wstring parent;
    std::wstring leaf;
};

struct InstallTarget {
    std::vector<DirRow> chain;
    std::wstring leaf;
    std::wstring pinned;
    std::wstring display;
};

void checkInstallPart(const std::wstring& part, const std::wstring& spec) {
    if (part.empty() || part == L"." || part == L"..")
        util::fail(L"--install-dir needs a plain directory path "
                   L"(no empty, \".\" or \"..\" parts): " + spec);
    for (wchar_t c : part)
        if (c < 0x20 || wcschr(L"*?\"<>|:", c) != nullptr)
            util::fail(L"--install-dir contains a character that is not valid in a "
                       L"directory name: " + spec);
}

InstallTarget parseInstallTarget(const std::wstring& raw,
                                 const std::wstring& programFilesKey,
                                 const std::wstring& productName) {
    InstallTarget t;
    std::wstring spec = util::trim(raw);
    auto addProgramFiles = [&] {
        t.chain.push_back({programFilesKey, L"TARGETDIR", L"."});
        return programFilesKey;
    };

    if (spec.empty()) {
        addProgramFiles();
        t.leaf = productName;
        t.display = L"[" + programFilesKey + L"]\\" + productName;
        return t;
    }
    for (wchar_t& c : spec)
        if (c == L'/') c = L'\\';

    const bool absolute =
        (spec.size() >= 2 && spec[1] == L':') ||
        (spec.size() >= 2 && spec[0] == L'\\' && spec[1] == L'\\');
    if (absolute) {
        while (spec.size() > 3 && spec.back() == L'\\') spec.pop_back();
        if (spec.size() == 2 && spec[1] == L':') spec.push_back(L'\\');
        for (wchar_t c : spec)
            if (c < 0x20 || wcschr(L"*?\"<>|;[]", c) != nullptr)
                util::fail(L"--install-dir contains a character that is not valid "
                           L"in a Windows Installer property: " + spec);
        size_t slash = spec.find_last_of(L"\\/");
        std::wstring leaf = slash == std::wstring::npos ? spec
                                                        : spec.substr(slash + 1);
        addProgramFiles();
        t.leaf = leaf.empty() ? productName : leaf;
        t.pinned = spec;
        t.display = spec + L" (INSTALLFOLDER property)";
        return t;
    }
    while (spec.size() > 1 && spec.back() == L'\\') spec.pop_back();

    std::vector<std::wstring> parts;
    size_t pos = 0;
    while (true) {
        size_t sep = spec.find(L'\\', pos);
        std::wstring part = spec.substr(
            pos, sep == std::wstring::npos ? std::wstring::npos : sep - pos);
        checkInstallPart(part, spec);
        parts.push_back(part);
        if (sep == std::wstring::npos) break;
        pos = sep + 1;
    }

    std::wstring parent;
    size_t next = 0;
    if (const StandardDir* std = findStandardDir(parts[0])) {
        t.chain.push_back({std->key, std->parent, std->def});
        parent = std->key;
        next = 1;
    } else {
        parent = addProgramFiles();
    }
    t.display = L"[" + parent + L"]";
    for (size_t i = next; i < parts.size(); ++i) t.display += L"\\" + parts[i];
    for (size_t i = next; i + 1 < parts.size(); ++i) {
        std::wstring key = L"ID" + std::to_wstring(t.chain.size());
        t.chain.push_back({key, parent, parts[i]});
        parent = key;
    }
    t.leaf = parts.size() > next ? parts.back() : L".";
    return t;
}

unsigned long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// ---------------------------------------------------------------------------
// Schema
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
    // Metadata table used by ICE validation tools. msiexec itself does not
    // need it, but the validators read the column types from here.
    L"CREATE TABLE `_Validation` (`Table` CHAR(32) NOT NULL, `Column` CHAR(32) NOT NULL, "
    L"`Nullable` CHAR(4) NOT NULL, `MinValue` LONG, `MaxValue` LONG, `KeyTable` CHAR(255), "
    L"`KeyColumn` SHORT, `Category` CHAR(32), `Set` CHAR(255), `Description` CHAR(255) "
    L"PRIMARY KEY `Table`, `Column`)",
};

struct ExtraTable {
    const wchar_t* name;
    const wchar_t* ddl;
};

const ExtraTable kExtraTables[] = {
    {L"CreateFolder",
     L"CREATE TABLE `CreateFolder` (`Directory_` CHAR(72) NOT NULL, "
     L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `Directory_`, `Component_`)"},
    {L"Shortcut",
     L"CREATE TABLE `Shortcut` (`Shortcut` CHAR(72) NOT NULL, `Directory_` CHAR(72) NOT NULL, "
     L"`Name` CHAR(128) NOT NULL LOCALIZABLE, `Component_` CHAR(72) NOT NULL, "
     L"`Target` CHAR(72) NOT NULL, `Arguments` CHAR(255) LOCALIZABLE, "
     L"`Description` CHAR(255) LOCALIZABLE, `Hotkey` SHORT, `Icon_` CHAR(72), "
     L"`IconIndex` SHORT, `ShowCmd` SHORT, `WkDir` CHAR(72), "
     L"`DisplayResourceDLL` CHAR(255), `DisplayResourceId` SHORT, "
     L"`DescriptionResourceDLL` CHAR(255), `DescriptionResourceId` SHORT "
     L"PRIMARY KEY `Shortcut`)"},
    {L"Icon",
     L"CREATE TABLE `Icon` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL "
     L"PRIMARY KEY `Name`)"},
    {L"Registry",
     L"CREATE TABLE `Registry` (`Registry` CHAR(72) NOT NULL, `Root` SHORT NOT NULL, "
     L"`Key` CHAR(255) NOT NULL LOCALIZABLE, `Name` CHAR(255) LOCALIZABLE, "
     L"`Value` LONGCHAR LOCALIZABLE, `Component_` CHAR(72) NOT NULL "
     L"PRIMARY KEY `Registry`)"},
    {L"ServiceInstall",
     L"CREATE TABLE `ServiceInstall` (`ServiceInstall` CHAR(72) NOT NULL, "
     L"`Name` CHAR(255) NOT NULL, `DisplayName` CHAR(255) LOCALIZABLE, "
     L"`ServiceType` LONG NOT NULL, `StartType` LONG NOT NULL, `ErrorControl` LONG NOT NULL, "
     L"`LoadOrderGroup` CHAR(255), `Dependencies` CHAR(255), `StartName` CHAR(255), "
     L"`Password` CHAR(255), `Arguments` CHAR(255), `Component_` CHAR(72) NOT NULL, "
     L"`Description` LONGCHAR LOCALIZABLE PRIMARY KEY `ServiceInstall`)"},
    {L"ServiceControl",
     L"CREATE TABLE `ServiceControl` (`ServiceControl` CHAR(72) NOT NULL, "
     L"`Name` CHAR(255) NOT NULL LOCALIZABLE, `Event` SHORT NOT NULL, "
     L"`Arguments` CHAR(255) LOCALIZABLE, `Wait` SHORT, "
     L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `ServiceControl`)"},
    {L"Environment",
     L"CREATE TABLE `Environment` (`Environment` CHAR(72) NOT NULL, "
     L"`Name` CHAR(255) NOT NULL LOCALIZABLE, `Value` LONGCHAR LOCALIZABLE, "
     L"`Component_` CHAR(72) NOT NULL PRIMARY KEY `Environment`)"},
    {L"MsiShortcutProperty",
     L"CREATE TABLE `MsiShortcutProperty` (`MsiShortcutProperty` CHAR(72) NOT NULL, "
     L"`Shortcut_` CHAR(72) NOT NULL, `PropertyKey` CHAR(0) NOT NULL, "
     L"`PropVariantValue` CHAR(0) NOT NULL PRIMARY KEY `MsiShortcutProperty`)"},
    {L"RemoveFile",
     L"CREATE TABLE `RemoveFile` (`FileKey` CHAR(72) NOT NULL, `Component_` CHAR(72) NOT NULL, "
     L"`FileName` CHAR(255), `DirProperty` CHAR(72), `InstallMode` SHORT NOT NULL "
     L"PRIMARY KEY `FileKey`)"},
    {L"CustomAction",
     L"CREATE TABLE `CustomAction` (`Action` CHAR(72) NOT NULL, `Type` SHORT NOT NULL, "
     L"`Source` CHAR(72), `Target` CHAR(255) PRIMARY KEY `Action`)"},
    {L"InstallUISequence",
     L"CREATE TABLE `InstallUISequence` (`Action` CHAR(72) NOT NULL, "
     L"`Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)"},
    {L"AdminUISequence",
     L"CREATE TABLE `AdminUISequence` (`Action` CHAR(72) NOT NULL, "
     L"`Condition` CHAR(255), `Sequence` SHORT PRIMARY KEY `Action`)"},
    {L"Dialog",
     L"CREATE TABLE `Dialog` (`Dialog` CHAR(72) NOT NULL, `HCentering` SHORT NOT NULL, "
     L"`VCentering` SHORT NOT NULL, `Width` SHORT NOT NULL, `Height` SHORT NOT NULL, "
     L"`Attributes` LONG, `Title` CHAR(128) LOCALIZABLE, `Control_First` CHAR(50) NOT NULL, "
     L"`Control_Default` CHAR(50), `Control_Cancel` CHAR(50) PRIMARY KEY `Dialog`)"},
    {L"Control",
     L"CREATE TABLE `Control` (`Dialog_` CHAR(72) NOT NULL, `Control` CHAR(50) NOT NULL, "
     L"`Type` CHAR(20) NOT NULL, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, "
     L"`Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, "
     L"`Property` CHAR(72), `Text` LONGCHAR LOCALIZABLE, `Control_Next` CHAR(50), "
     L"`Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Dialog_`, `Control`)"},
    {L"ControlEvent",
     L"CREATE TABLE `ControlEvent` (`Dialog_` CHAR(72) NOT NULL, `Control_` CHAR(50) NOT NULL, "
     L"`Event` CHAR(50) NOT NULL, `Argument` CHAR(255) NOT NULL, `Condition` CHAR(255), "
     L"`Ordering` SHORT PRIMARY KEY `Dialog_`, `Control_`, `Event`, `Argument`, `Condition`)"},
    {L"ControlCondition",
     L"CREATE TABLE `ControlCondition` (`Dialog_` CHAR(72) NOT NULL, "
     L"`Control_` CHAR(50) NOT NULL, `Action` CHAR(64) NOT NULL, "
     L"`Condition` CHAR(255) NOT NULL PRIMARY KEY `Dialog_`, `Control_`, `Action`, `Condition`)"},
    {L"TextStyle",
     L"CREATE TABLE `TextStyle` (`TextStyle` CHAR(72) NOT NULL, `FaceName` CHAR(32) NOT NULL, "
     L"`Size` SHORT NOT NULL, `Color` LONG, `StyleBits` SHORT PRIMARY KEY `TextStyle`)"},
    {L"Binary",
     L"CREATE TABLE `Binary` (`Name` CHAR(72) NOT NULL, `Data` OBJECT NOT NULL "
     L"PRIMARY KEY `Name`)"},
    {L"RadioButton",
     L"CREATE TABLE `RadioButton` (`Property` CHAR(72) NOT NULL, `Order` SHORT NOT NULL, "
     L"`Value` CHAR(64) LOCALIZABLE, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, "
     L"`Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Text` CHAR(64) LOCALIZABLE, "
     L"`Help` CHAR(50) LOCALIZABLE PRIMARY KEY `Property`, `Order`)"},
    {L"ComboBox",
     L"CREATE TABLE `ComboBox` (`Property` CHAR(72) NOT NULL, `Order` SHORT NOT NULL, "
     L"`Value` CHAR(64) LOCALIZABLE, `Text` CHAR(64) LOCALIZABLE "
     L"PRIMARY KEY `Property`, `Order`)"},
    {L"ListBox",
     L"CREATE TABLE `ListBox` (`Property` CHAR(72) NOT NULL, `Order` SHORT NOT NULL, "
     L"`Value` CHAR(64) LOCALIZABLE, `Text` CHAR(64) LOCALIZABLE "
     L"PRIMARY KEY `Property`, `Order`)"},
    {L"ListView",
     L"CREATE TABLE `ListView` (`Property` CHAR(72) NOT NULL, `Order` SHORT NOT NULL, "
     L"`Value` CHAR(64) LOCALIZABLE, `Text` CHAR(64) LOCALIZABLE "
     L"PRIMARY KEY `Property`, `Order`)"},
    {L"Error",
     L"CREATE TABLE `Error` (`Error` LONG NOT NULL, `Text` CHAR(255) LOCALIZABLE "
     L"PRIMARY KEY `Error`)"},
    {L"CheckBox",
     L"CREATE TABLE `CheckBox` (`Property` CHAR(72) NOT NULL, `Value` CHAR(64) "
     L"PRIMARY KEY `Property`)"},
    {L"Billboard",
     L"CREATE TABLE `Billboard` (`Billboard` CHAR(50) NOT NULL, `Feature_` CHAR(38) NOT NULL, "
     L"`Action` CHAR(50), `Ordering` SHORT PRIMARY KEY `Billboard`)"},
    {L"BBControl",
     L"CREATE TABLE `BBControl` (`Billboard_` CHAR(50) NOT NULL, `BBControl` CHAR(50) NOT NULL, "
     L"`Type` CHAR(50) NOT NULL, `X` SHORT NOT NULL, `Y` SHORT NOT NULL, "
     L"`Width` SHORT NOT NULL, `Height` SHORT NOT NULL, `Attributes` LONG, "
     L"`Text` CHAR(50) LOCALIZABLE PRIMARY KEY `Billboard_`, `BBControl`)"},
};

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

const SequenceAction kAdminSequence[] = {
    {L"CostInitialize",     800}, {L"FileCost",           900}, {L"CostFinalize",    1000},
    {L"InstallValidate",   1400}, {L"InstallInitialize", 1500},
    {L"InstallAdminPackage",3900},{L"InstallFiles",      4000},{L"InstallFinalize", 6600},
};

const SequenceAction kUiSequenceBasic[] = {
    {L"CostInitialize", 800}, {L"FileCost", 900}, {L"CostFinalize", 1000},
    {L"ExecuteAction", 1300},
};

// Fills _Validation with column metadata derived from the DDL we executed.
// Windows Installer itself does not need the table; ICE validators use it to
// learn column types before running the consistency evaluators.
void insertValidationRows(MSIHANDLE db, const std::vector<std::wstring>& ddls) {
    for (const std::wstring& ddl : ddls) {
        const std::wstring prefix = L"CREATE TABLE `";
        size_t p = ddl.find(prefix);
        if (p == std::wstring::npos) continue;
        size_t nameStart = p + prefix.size();
        size_t nameEnd = ddl.find(L'`', nameStart);
        if (nameEnd == std::wstring::npos) continue;
        std::wstring table = ddl.substr(nameStart, nameEnd - nameStart);

        size_t lp = ddl.find(L'(', nameEnd);
        size_t rp = ddl.rfind(L')');
        if (lp == std::wstring::npos || rp == std::wstring::npos || rp <= lp) continue;
        std::wstring body = ddl.substr(lp + 1, rp - lp - 1);
        size_t pk = body.find(L"PRIMARY KEY");
        std::wstring cols = pk == std::wstring::npos ? body : body.substr(0, pk);

        size_t pos = 0;
        while (pos <= cols.size()) {
            size_t comma = cols.find(L',', pos);
            std::wstring part = util::trim(cols.substr(
                pos, comma == std::wstring::npos ? std::wstring::npos : comma - pos));
            pos = comma == std::wstring::npos ? cols.size() + 1 : comma + 1;
            if (part.empty() || part.front() != L'`') continue;
            size_t close = part.find(L'`', 1);
            if (close == std::wstring::npos) continue;
            std::wstring column = part.substr(1, close - 1);
            std::wstring rest = part.substr(close + 1);
            bool nullable = rest.find(L"NOT NULL") == std::wstring::npos;
            insertRow(db, L"_Validation",
                      {L"Table", L"Column", L"Nullable", L"MinValue", L"MaxValue",
                       L"KeyTable", L"KeyColumn", L"Category", L"Set", L"Description"},
                      {S(table), S(column), S(nullable ? L"Y" : L"N"), Null(), Null(),
                       Null(), Null(), Null(), Null(), Null()});
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

// ---------------------------------------------------------------------------
// Manifest / UI preparation
// ---------------------------------------------------------------------------

void applyManifest(Options& o, const Manifest& m) {
    if (!o.productNameSet && o.productName.empty() && !m.pack.name.empty())
        o.productName = m.pack.name;
    if (!o.versionSet && !m.pack.version.empty()) o.version = m.pack.version;
    if (!o.manufacturerSet && !m.pack.manufacturer.empty())
        o.manufacturer = m.pack.manufacturer;
    if (!o.productCodeSet && !m.pack.productCode.empty())
        o.productCode = m.pack.productCode;
    if (!o.upgradeCodeSet && !m.pack.upgradeCode.empty())
        o.upgradeCode = m.pack.upgradeCode;
    if (!o.platformSet && !m.pack.platform.empty()) o.platform = m.pack.platform;
    if (!o.scopeSet && !m.pack.scope.empty()) o.scope = m.pack.scope;
    if (!o.languageSet && m.pack.hasLanguage) o.language = m.pack.language;
    if (!o.installDirSet && o.installDir.empty() && !m.pack.installDir.empty())
        o.installDir = m.pack.installDir;
    if (!o.uiSet && !m.pack.ui.empty()) o.ui = m.pack.ui;
    if (!o.cabSizeSet && o.cabSize == 0 && m.pack.hasCabSize) o.cabSize = m.pack.cabSize;
    if (!o.largeFilesSet && !m.pack.largeFiles.empty()) o.largeFiles = m.pack.largeFiles;
    if (!m.pack.keepEmptyDirs) o.keepEmptyDirs = false;
    if (!o.signCertSet && o.signCert.empty() && m.sign.present) {
        o.signCert = m.sign.cert;
        o.signPassword = m.sign.password;
        o.signTimestamp = m.sign.timestamp;
        o.signTool = m.sign.tool;
    }
    if (!o.uiTemplateSet && o.uiTemplate.empty() && m.ui.hasTemplate)
        o.uiTemplate = m.ui.templateMsi;
}

// Builds the final UI model: base (minimal / wizard / template) + XML delta.
UiSpec buildUi(const Options& o, const Manifest& m, const std::wstring& tempDir,
               bool& uiEnabled) {
    UiSpec delta = m.ui;
    UiSpec base;
    uiEnabled = o.ui != L"none";

    const std::wstring locale =
        o.uiLocale.empty() ? localeForLanguage(o.language) : o.uiLocale;

    if (!o.uiTemplate.empty()) {
        base = loadUiTemplate(util::path::full(o.uiTemplate));
        uiEnabled = true;
    } else if (o.ui == L"wizard") {
        base = builtinWizardUi(locale, tempDir);
        uiEnabled = true;
    } else if (o.ui == L"minimal") {
        base = builtinMinimalUi();
        uiEnabled = true;
    } else if (o.ui == L"custom") {
        uiEnabled = true;
        if (util::iequals(delta.base, L"wizard"))
            base = builtinWizardUi(locale, tempDir);
        else if (util::iequals(delta.base, L"minimal"))
            base = builtinMinimalUi();
    } else if (o.ui == L"basic") {
        if (!delta.empty())
            util::fail(L"--ui basic cannot be combined with a <ui> section");
        uiEnabled = false;
    } else if (o.ui == L"none") {
        if (!delta.empty())
            util::fail(L"a <ui> section needs --ui wizard, custom, minimal or "
                       L"--ui-template");
        uiEnabled = false;
    }

    UiSpec spec = base;
    mergeUi(spec, delta);

    if (uiEnabled && spec.dialogs.empty())
        util::fail(L"UI mode '" + o.ui + L"' produced no dialogs");
    if (!uiEnabled) return UiSpec{};

    if (!spec.hasSequence)
        util::fail(L"the installer UI needs an InstallUISequence (add <uisequence> "
                   L"or a dialog with sequence=...)");

    std::vector<UiIssue> issues;
    std::vector<std::wstring> actionIds;
    for (const auto& a : m.customActions) actionIds.push_back(a.id);
    for (const auto& a : spec.customActions) actionIds.push_back(a.id);
    validateUi(spec, actionIds, issues);
    for (const auto& i : issues) {
        if (i.error) util::errLine(L"[ui] " + i.message);
        else util::errLine(L"[ui][warn] " + i.message);
    }
    for (const auto& i : issues)
        if (i.error) util::fail(L"installer UI validation failed");
    return spec;
}

}  // namespace

// ---------------------------------------------------------------------------
// pack
// ---------------------------------------------------------------------------

Stats pack(const Options& opt) {
    Stats st;
    if (!opt.dumpUiDir.empty()) {
        std::wstring error;
        if (!dumpBuiltinUi(opt.dumpUiDir, error)) util::fail(error);
        util::outLine(L"Built-in UI resources written to " + util::path::full(opt.dumpUiDir));
        return st;
    }

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
    if (!util::trim(o.configFile).empty()) {
        std::wstring cfgPath = util::path::full(o.configFile);
        if (util::iequals(util::path::ext(cfgPath), L".ini"))
            util::fail(L"INI manifests are no longer supported; use XML "
                       L"(see examples/pack.xml): " + cfgPath);
        cfgData = loadManifestXml(cfgPath);
        applyManifest(o, cfgData);
    }

    o.ui = util::toLower(o.ui);
    if (o.ui != L"none" && o.ui != L"basic" && o.ui != L"minimal" &&
        o.ui != L"wizard" && o.ui != L"custom")
        util::fail(L"--ui must be none, basic, minimal, wizard or custom: " + o.ui);
    o.largeFiles = util::toLower(o.largeFiles);
    if (o.largeFiles != L"layout" && o.largeFiles != L"error")
        util::fail(L"--large-files must be layout or error: " + o.largeFiles);

    util::TempDir temp(L"msitool_pack");
    bool uiEnabled = false;
    UiSpec uiSpec = buildUi(o, cfgData, temp.path(), uiEnabled);
    std::vector<CustomActionDef> allCustomActions = cfgData.customActions;
    for (const auto& a : uiSpec.customActions) allCustomActions.push_back(a);

    std::wstring productName = o.productName.empty()
                                   ? util::path::fileName(inDir)
                                   : o.productName;
    if (productName.empty()) productName = L"Application";
    std::wstring productCode = normalizeGuid(o.productCode);
    std::wstring upgradeCode = normalizeGuid(o.upgradeCode);
    std::wstring platform = o.platform;
    if (!util::iequals(platform, L"x64") && !util::iequals(platform, L"Intel"))
        util::fail(L"--platform must be x64 or Intel: " + platform);
    std::wstring scope = util::toLower(util::trim(o.scope));
    if (scope.empty()) scope = L"permachine";
    if (scope != L"permachine" && scope != L"peruser")
        util::fail(L"scope must be perMachine or perUser: " + o.scope);
    const bool perUser = scope == L"peruser";
    const std::wstring rootDirKey =
        perUser ? std::wstring(L"LocalAppDataFolder")
                : (util::iequals(platform, L"x64") ? L"ProgramFiles64Folder"
                                                    : L"ProgramFilesFolder");
    const InstallTarget installTarget =
        parseInstallTarget(o.installDir, rootDirKey, productName);
    if (installTarget.chain[0].key == L"ProgramFiles64Folder" &&
        !util::iequals(platform, L"x64"))
        util::errLine(L"[warn] --install-dir uses ProgramFiles64Folder while "
                      L"--platform is Intel");
    if (installTarget.chain[0].key == L"ProgramFilesFolder" &&
        util::iequals(platform, L"x64"))
        util::errLine(L"[warn] --install-dir uses ProgramFilesFolder while "
                      L"--platform is x64");

    std::vector<Entry> files;
    std::vector<std::wstring> allDirs;
    const bool selecting = !o.inputFiles.empty();
    if (selecting) {
        allDirs.push_back(L"");
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

    // > 2 GiB files cannot go through FCI / the 32-bit FileSize column.
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

    for (size_t i = 0; i < cabFiles.size(); ++i)
        cabFiles[i].fileKey = L"F" + std::to_wstring(i + 1);
    for (size_t i = 0; i < externalFiles.size(); ++i)
        externalFiles[i].fileKey = L"F" + std::to_wstring(cabFiles.size() + i + 1);
    std::vector<Entry> ordered = cabFiles;
    ordered.insert(ordered.end(), externalFiles.begin(), externalFiles.end());

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
    layout.rootKey = installTarget.chain.back().key;
    layout.installLeaf = installTarget.leaf;
    for (const auto& d : allDirs) layout.ensure(d);
    for (auto& kv : layout.dirs) {
        for (const auto& f : ordered) {
            if (util::iequals(f.relDir, kv.first)) {
                kv.second.keyPath = f.fileKey;
                break;
            }
        }
    }

    // GUI shortcuts / "create desktop shortcut" checkboxes.
    if (o.desktopShortcut || o.startMenuShortcut) {
        const Entry* main = nullptr;
        if (!o.mainExe.empty()) {
            std::wstring want = normRel(o.mainExe);
            for (const auto& f : ordered) {
                std::wstring rel = normRel((f.relDir.empty() ? L"" : f.relDir + L"\\") +
                                           f.fileName);
                if (util::iequals(rel, want) || util::iequals(f.fileKey, want)) {
                    main = &f;
                    break;
                }
            }
            if (!main) util::fail(L"main program not found in the payload: " + o.mainExe);
        } else {
            // Prefer a root-level .exe, otherwise the largest .exe.
            for (const auto& f : ordered) {
                if (!util::iequals(util::path::ext(f.fileName), L".exe")) continue;
                if (f.relDir.empty()) { main = &f; break; }
                if (!main || f.size > main->size) main = &f;
            }
            if (!main)
                util::errLine(L"[warn] no .exe found; desktop / start menu shortcuts "
                              L"were not created");
        }
        if (main) {
            std::wstring rel = normRel((main->relDir.empty() ? L"" : main->relDir + L"\\") +
                                       main->fileName);
            auto add = [&](const wchar_t* dir, const wchar_t* suffix) {
                ShortcutDef d;
                d.id = toIdentifier(productName + suffix);
                d.name = productName + L".lnk";
                d.dir = dir;
                d.target = rel;
                d.icon = rel;
                d.description = productName;
                o.shortcuts.push_back(std::move(d));
            };
            if (o.desktopShortcut) add(L"DesktopFolder", L"Desktop");
            if (o.startMenuShortcut) add(L"ProgramMenuFolder", L"StartMenu");
        }
    }
    for (const auto& d : o.shortcuts) cfgData.shortcuts.push_back(d);

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
    st.customActions = allCustomActions.size();
    st.uiPages = uiSpec.dialogs.size();
    for (const auto& kv : layout.dirs)
        if (kv.second.keyPath.empty()) ++st.emptyDirs;

    util::outLine(util::fmt(L"Packing %llu file(s) in %llu directory(ies) -> %s",
                            static_cast<unsigned long long>(st.files),
                            static_cast<unsigned long long>(st.directories),
                            outMsi.c_str()));
    util::outLine(L"Product   : " + productName + L" " + o.version);
    util::outLine(L"Install   : " + installTarget.display);
    util::outLine(L"Codes     : ProductCode=" + productCode + L" UpgradeCode=" + upgradeCode);
    util::outLine(L"Platform  : " + platform + L";" + std::to_wstring(o.language) +
                  (perUser ? L";per-user" : L";per-machine"));
    util::outLine(util::fmt(L"Cabinets  : %llu embedded CAB(s), %llu feature(s), "
                            L"%llu empty dir(s), %llu >2 GiB layout file(s)",
                            static_cast<unsigned long long>(st.cabinets),
                            static_cast<unsigned long long>(st.features),
                            static_cast<unsigned long long>(st.emptyDirs),
                            static_cast<unsigned long long>(st.externalFiles)));
    if (st.customActions)
        util::outLine(util::fmt(L"Actions   : %llu custom action(s)",
                                static_cast<unsigned long long>(st.customActions)));
    if (uiEnabled) {
        std::wstring uiMode = o.uiTemplate.empty() ? o.ui : L"template";
        util::outLine(util::fmt(L"UI        : %s (%llu dialog(s), locale %s)",
                                uiMode.c_str(),
                                static_cast<unsigned long long>(st.uiPages),
                                (o.uiLocale.empty() ? localeForLanguage(o.language)
                                                    : o.uiLocale).c_str()));
    }
    if (o.dryRun) {
        util::outLine(L"[dry-run] No cabinet or database was written.");
        return st;
    }

    // --- 1. Build the embedded cabinet(s) ----------------------------------
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
    const bool needCustomActions = !allCustomActions.empty();
    const UiTables uiTables = uiTablesFor(uiSpec);

    UINT verifyRc = ERROR_SUCCESS;
    {
        msi::Handle db;
        util::checkMsi(MsiOpenDatabaseW(outMsi.c_str(), MSIDBOPEN_CREATE, db.out()),
                       L"MsiOpenDatabaseW(create) " + outMsi);
        MSIHANDLE h = db.get();

        std::vector<std::wstring> executedDdl;
        for (const wchar_t* ddl : kSchema) {
            execSql(h, ddl);
            executedDdl.push_back(ddl);
        }
        std::unordered_set<std::wstring> created;
        auto create = [&](const wchar_t* table) {
            if (!created.insert(util::toLower(table)).second) return;
            for (const ExtraTable& t : kExtraTables) {
                if (util::iequals(t.name, table)) {
                    execSql(h, t.ddl);
                    executedDdl.push_back(t.ddl);
                    return;
                }
            }
            util::fail(L"internal error: no schema for table " + std::wstring(table));
        };
        if (needCreateFolders) create(L"CreateFolder");
        if (needShortcuts) {
            create(L"Shortcut");
            create(L"Icon");
            create(L"MsiShortcutProperty");
            create(L"RemoveFile");
        }
        if (needRegistry) create(L"Registry");
        if (needServices) { create(L"ServiceInstall"); create(L"ServiceControl"); }
        if (needEnvironment) create(L"Environment");
        if (needCustomActions) {
            create(L"CustomAction");
            for (const auto& a : allCustomActions)
                if (a.hasBinary) { create(L"Binary"); break; }
        }
        if (uiTables.installSequence || o.ui == L"basic") create(L"InstallUISequence");
        if (uiTables.adminSequence) create(L"AdminUISequence");
        if (uiTables.dialog) create(L"Dialog");
        if (uiTables.control) create(L"Control");
        if (uiTables.controlEvent) create(L"ControlEvent");
        if (uiTables.controlCondition) create(L"ControlCondition");
        if (uiTables.textStyle) create(L"TextStyle");
        if (uiTables.binary) create(L"Binary");
        if (uiTables.radioButton) create(L"RadioButton");
        if (uiTables.comboBox) create(L"ComboBox");
        if (uiTables.listBox) create(L"ListBox");
        if (uiTables.listView) create(L"ListView");
        if (uiTables.error) create(L"Error");
        if (uiTables.checkBox) create(L"CheckBox");
        if (uiTables.billboard) { create(L"Billboard"); create(L"BBControl"); }

        insertValidationRows(h, executedDdl);

        // Directory table.
        std::unordered_set<std::wstring> knownDirs;
        auto ensureDirRow = [&](const std::wstring& key, const std::wstring& parent,
                                const std::wstring& leaf) {
            if (!knownDirs.insert(util::toLower(key)).second) return;
            insertRow(h, L"Directory", {L"Directory", L"Directory_Parent", L"DefaultDir"},
                      {S(key), parent.empty() ? Null() : S(parent), S(leaf)});
        };
        ensureDirRow(L"TARGETDIR", L"", L"SourceDir");
        for (const DirRow& r : installTarget.chain)
            ensureDirRow(r.key, r.parent, r.leaf);
        for (const auto& kv : layout.dirs)
            ensureDirRow(kv.second.key, kv.second.parentKey, kv.second.leaf);

        // Components: one per payload directory, keyed by its first file.
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

        // File rows.
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

        // Media rows.
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

        // Features.
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
        struct TargetInfo {
            std::wstring target;
            std::wstring comp;
            std::wstring wkdir;
            std::wstring relDir;
        };
        auto targetFor = [&](const std::wstring& raw) {
            TargetInfo t;
            std::wstring want = normRel(raw);
            auto it = fileByRel.find(util::toLower(want));
            if (it != fileByRel.end()) {
                const Entry& f = ordered[it->second];
                const DirInfo& d = layout.dirs[util::toLower(f.relDir)];
                t.comp = d.componentKey;
                t.wkdir = d.key;
                t.relDir = f.relDir;
                t.target = L"[#" + f.fileKey + L"]";
                return t;
            }
            if (!want.empty() && (want.front() == L'[' || want.front() == L'#')) {
                t.target = want;
                return t;
            }
            t.target = L"[INSTALLFOLDER]" + want;
            return t;
        };

        // Shortcut directories: standard folders (and aliases), sub-paths
        // below them, existing layout directories, then INSTALLFOLDER.
        int shortcutDirNo = 0;
        int shortcutCompNo = 0;
        std::map<std::wstring, std::wstring> shortcutDirComponents;
        auto ensureShortcutDirComponent = [&](const std::wstring& dirKey,
                                              const std::wstring& feature) {
            std::wstring low = util::toLower(dirKey);
            auto it = shortcutDirComponents.find(low);
            if (it != shortcutDirComponents.end()) return it->second;
            if (!created.count(L"registry")) create(L"Registry");
            std::wstring comp = L"CS" + std::to_wstring(++shortcutCompNo);
            std::wstring reg = L"RegShortcutDir" + std::to_wstring(shortcutCompNo);
            insertRow(h, L"Component",
                      {L"Component", L"ComponentId", L"Directory_", L"Attributes",
                       L"Condition", L"KeyPath"},
                      {S(comp), S(newGuid()), S(dirKey), I(compAttrs | 4), Null(), S(reg)});
            insertRow(h, L"Registry",
                      {L"Registry", L"Root", L"Key", L"Name", L"Value", L"Component_"},
                      {S(reg), I(perUser ? 1 : 2), S(L"Software\\msitool\\ShortcutFolders"),
                       S(reg), S(L"1"), S(comp)});
            insertRow(h, L"FeatureComponents", {L"Feature_", L"Component_"},
                      {S(feature), S(comp)});
            insertRow(h, L"RemoveFile",
                      {L"FileKey", L"Component_", L"FileName", L"DirProperty",
                       L"InstallMode"},
                      {S(L"RF" + comp), S(comp), Null(), S(dirKey), I(2)});
            shortcutDirComponents[low] = comp;
            return comp;
        };
        auto resolveShortcutDir = [&](const std::wstring& raw,
                                      const std::wstring& feature,
                                      std::wstring& outComponent) {
            std::wstring spec = normRel(raw);
            if (spec.empty()) return std::wstring(L"INSTALLFOLDER");
            std::wstring first = spec;
            size_t slash = spec.find(L'\\');
            if (slash != std::wstring::npos) first = spec.substr(0, slash);
            std::wstring alias = util::toLower(first);
            std::wstring standard;
            if (alias == L"desktop") standard = L"DesktopFolder";
            else if (alias == L"startmenu") standard = L"ProgramMenuFolder";
            else if (alias == L"startup") standard = L"StartupFolder";
            else if (alias == L"sendto") standard = L"SendToFolder";
            else if (findStandardDir(first)) standard = first;

            if (!standard.empty()) {
                const StandardDir* sd = findStandardDir(standard);
                ensureDirRow(sd->key, sd->parent, sd->def);
                std::wstring parent = sd->key;
                size_t pos = slash == std::wstring::npos ? std::wstring::npos : slash + 1;
                while (pos != std::wstring::npos && pos < spec.size()) {
                    size_t next = spec.find(L'\\', pos);
                    std::wstring leaf = sanitizeName(
                        spec.substr(pos, next == std::wstring::npos
                                             ? std::wstring::npos : next - pos));
                    std::wstring key = L"SD" + std::to_wstring(++shortcutDirNo);
                    ensureDirRow(key, parent, leaf);
                    outComponent = ensureShortcutDirComponent(key, feature);
                    parent = key;
                    pos = next == std::wstring::npos ? std::wstring::npos : next + 1;
                }
                return parent;
            }
            for (const auto& kv : layout.dirs)
                if (util::iequals(spec, kv.second.key) ||
                    util::iequals(spec, kv.first))
                    return kv.second.key;
            // Relative path that is not part of the payload: create it below
            // INSTALLFOLDER.
            std::wstring parent = L"INSTALLFOLDER";
            size_t pos = 0;
            while (pos <= spec.size()) {
                size_t next = spec.find(L'\\', pos);
                std::wstring leaf = sanitizeName(
                    spec.substr(pos, next == std::wstring::npos
                                         ? std::wstring::npos : next - pos));
                if (!leaf.empty()) {
                    std::wstring key = L"SD" + std::to_wstring(++shortcutDirNo);
                    ensureDirRow(key, parent, leaf);
                    outComponent = ensureShortcutDirComponent(key, feature);
                    parent = key;
                }
                if (next == std::wstring::npos) break;
                pos = next + 1;
            }
            return parent;
        };

        std::map<std::wstring, std::wstring> iconKeys;
        for (const auto& d : cfgData.shortcuts) {
            TargetInfo ti = targetFor(d.target);
            std::wstring comp = ti.comp;
            if (comp.empty()) comp = componentFor(d.component, d.target);
            std::wstring wkdir = ti.wkdir;
            if (!d.wkdir.empty()) {
                for (const auto& kv : layout.dirs)
                    if (util::iequals(d.wkdir, kv.first) ||
                        util::iequals(d.wkdir, kv.second.key)) wkdir = kv.second.key;
            }
            std::wstring feature = featureOf(ti.relDir.empty() ? L"" : ti.relDir);
            if (!d.component.empty()) {
                // An explicit component can belong to a custom feature; keep
                // the shortcut in the same feature as the payload file.
            }
            std::wstring dirComp;
            std::wstring dirKey = resolveShortcutDir(d.dir, feature, dirComp);
            if (!dirComp.empty()) comp = dirComp;

            std::wstring target = ti.target;
            if (d.advertise) {
                if (d.target.empty())
                    util::fail(L"advertised shortcut " + d.id + L" needs a target file "
                              L"to determine its feature");
                // An advertised shortcut must belong to a component whose
                // keypath is a file (ICE19); the synthetic folder component
                // exists only to create the directory.
                comp = ti.comp;
                target = feature;
            } else if (d.target.empty()) {
                util::fail(L"shortcut " + d.id + L" needs a target (or advertise=\"true\")");
            }

            std::wstring iconKey;
            if (!d.icon.empty()) {
                std::wstring iconPath = absolutePath(d.icon)
                                            ? util::path::full(d.icon)
                                            : util::path::join(inDir, d.icon);
                if (!util::path::isDirectory(iconPath) && !util::path::exists(iconPath)) {
                    std::wstring alt = util::path::full(d.icon);
                    if (util::path::exists(alt) || util::path::isDirectory(alt)) iconPath = alt;
                }
                if (iconPath.empty() || (!util::path::isDirectory(iconPath) &&
                                         !util::path::exists(iconPath))) {
                    util::errLine(L"[warn] shortcut icon not found: " + d.icon);
                } else {
                    auto it = iconKeys.find(util::toLower(iconPath));
                    if (it != iconKeys.end()) {
                        iconKey = it->second;
                    } else {
                        iconKey = toIdentifier(d.id + L"Icon");
                        insertStreamRow(h, L"Icon", L"Name", L"Data", iconKey, iconPath);
                        iconKeys[util::toLower(iconPath)] = iconKey;
                    }
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
                       iconKey.empty() ? Null() : S(iconKey),
                       iconKey.empty() ? Null() : I(d.iconIndex),
                       Null(), wkdir.empty() ? Null() : S(wkdir), Null(), Null(), Null(),
                       Null()});
            for (const auto& p : d.properties) {
                std::wstring id = toIdentifier(d.id + L"Prop" + p.key);
                insertRow(h, L"MsiShortcutProperty",
                          {L"MsiShortcutProperty", L"Shortcut_", L"PropertyKey",
                           L"PropVariantValue"},
                          {S(id), S(d.id), S(p.key), S(p.value)});
            }
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
            TargetInfo ti = d.image.empty() ? TargetInfo{} : targetFor(d.image);
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
            (void)ti;
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

        // Custom actions (manifest + UI-template imports).
        std::set<std::wstring> embeddedBinary;
        for (const auto& d : allCustomActions) {
            const int srcKind = d.type & 0x30;
            std::wstring source = d.source;
            if (d.hasBinary && !d.binaryFile.empty()) {
                if (embeddedBinary.insert(util::toLower(d.source)).second)
                    insertStreamRow(h, L"Binary", L"Name", L"Data", d.source, d.binaryFile);
            } else if (srcKind == 0x10) {
                std::wstring want = normRel(source);
                auto it = fileByRel.find(util::toLower(want));
                if (it != fileByRel.end()) source = ordered[it->second].fileKey;
            } else if (srcKind == 0x20) {
                std::wstring want = normRel(source);
                if (const StandardDir* sd = findStandardDir(util::trim(source))) {
                    ensureDirRow(sd->key, sd->parent, sd->def);
                    source = sd->key;
                } else {
                    for (const auto& kv : layout.dirs)
                        if (util::iequals(want, kv.second.key) ||
                            util::iequals(want, kv.first)) {
                            source = kv.second.key;
                            break;
                        }
                }
            }
            insertRow(h, L"CustomAction",
                      {L"Action", L"Type", L"Source", L"Target"},
                      {S(d.id), I(d.type),
                       source.empty() ? Null() : S(source),
                       d.target.empty() ? Null() : S(d.target)});
        }

        // Properties.
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
        // Per-machine installs set ALLUSERS=1; per-user installs leave it
        // unset so msiexec installs into the user profile.
        if (!perUser) prop(L"ALLUSERS", L"1");
        if (!installTarget.pinned.empty()) prop(L"INSTALLFOLDER", installTarget.pinned);

        // Execute sequences.
        for (const auto& a : kSequence)
            insertRow(h, L"InstallExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});
        for (const auto& a : kAdminSequence)
            insertRow(h, L"AdminExecuteSequence", {L"Action", L"Condition", L"Sequence"},
                      {S(a.name), Null(), I(a.seq)});

        // Extra execute-sequence rows (usually custom actions).
        if (cfgData.hasExecuteSequence) {
            std::set<std::wstring> used;
            for (const auto& a : kSequence) used.insert(util::toLower(a.name));
            for (const auto& e : cfgData.executeSequence) {
                if (!used.insert(util::toLower(e.action)).second)
                    util::fail(L"executeSequence: duplicate action " + e.action);
                insertRow(h, L"InstallExecuteSequence",
                          {L"Action", L"Condition", L"Sequence"},
                          {S(e.action),
                           e.hasCondition && !e.condition.empty() ? S(e.condition) : Null(),
                           I(e.sequence)});
            }
        }
        if (cfgData.hasAdminExecuteSequence) {
            std::set<std::wstring> used;
            for (const auto& a : kAdminSequence) used.insert(util::toLower(a.name));
            for (const auto& e : cfgData.adminExecuteSequence) {
                if (!used.insert(util::toLower(e.action)).second)
                    util::fail(L"adminExecuteSequence: duplicate action " + e.action);
                insertRow(h, L"AdminExecuteSequence",
                          {L"Action", L"Condition", L"Sequence"},
                          {S(e.action),
                           e.hasCondition && !e.condition.empty() ? S(e.condition) : Null(),
                           I(e.sequence)});
            }
        }

        // UI: basic progress only, or the full dialog set.
        if (o.ui == L"basic") {
            for (const auto& a : kUiSequenceBasic)
                insertRow(h, L"InstallUISequence", {L"Action", L"Condition", L"Sequence"},
                          {S(a.name), Null(), I(a.seq)});
        } else if (uiEnabled) {
            applyUi(h, uiSpec, temp.path(), productName);
        }

        // Embedded cabinets as _Streams rows.
        for (size_t g = 0; g < cabPaths.size(); ++g)
            insertStreamRow(h, L"_Streams", L"Name", L"Data",
                            L"cab" + std::to_wstring(g + 1) + L".cab", cabPaths[g]);

        // SummaryInformation.
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
        setStr(2, productName);
        setStr(3, productName);
        setStr(4, o.manufacturer);
        setStr(7, platform + L";" + std::to_wstring(o.language));
        setStr(9, productCode);
        setInt(14, 200);
        setInt(15, 2);
        setStr(18, L"msitool");
        setInt(19, 2);
        util::checkMsi(MsiSummaryInfoPersist(sum.get()), L"MsiSummaryInfoPersist");

        util::checkMsi(MsiDatabaseCommit(h), L"MsiDatabaseCommit");
    }

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
