#include "cab.h"

#include <fdi.h>

#include <fcntl.h>
#include <cstdlib>
#include <unordered_map>

namespace cab {

namespace {

struct Context {
    const std::unordered_map<std::wstring, std::wstring>* map = nullptr;
    std::wstring fallbackDir;
    std::wstring tempDir;
    bool         dryRun = false;
    bool         skipExisting = false;
    bool         verbose = false;
    bool         spanning = false;
    Result*      result = nullptr;

    // handle -> lowercased cab-internal file name (fdintCOPY_FILE -> CLOSE_FILE_INFO)
    std::unordered_map<INT_PTR, std::wstring> handleNames;
    std::wstring spillPath;
    INT_PTR      spillHandle = -1;
};

Context* g_ctx = nullptr;

std::wstring fdiErrorText(const ERF& erf) {
    switch (erf.erfOper) {
        case FDIERROR_NONE:                    return L"no error";
        case FDIERROR_CABINET_NOT_FOUND:       return L"cabinet not found";
        case FDIERROR_NOT_A_CABINET:           return L"not a cabinet file";
        case FDIERROR_UNKNOWN_CABINET_VERSION: return L"unknown cabinet version";
        case FDIERROR_CORRUPT_CABINET:         return L"corrupt cabinet";
        case FDIERROR_ALLOC_FAIL:              return L"memory allocation failed";
        case FDIERROR_BAD_COMPR_TYPE:          return L"unknown compression type";
        case FDIERROR_MDI_FAIL:                return L"decompressor failure";
        case FDIERROR_TARGET_FILE:             return L"cannot write target file";
        case FDIERROR_RESERVE_MISMATCH:        return L"cabinet reserve mismatch";
        case FDIERROR_WRONG_CABINET:           return L"wrong cabinet in set";
        case FDIERROR_USER_ABORT:              return L"aborted";
        default: break;
    }
    return util::fmt(L"FDI error %d (type=%d, fError=%d)", erf.erfOper, erf.erfType,
                     erf.fError);
}

// --- FDI callbacks ----------------------------------------------------------

FNALLOC(fdiAlloc) { return malloc(cb); }
FNFREE(fdiFree) { free(pv); }

FNOPEN(fdiOpen) {
    (void)pmode;
    Context* ctx = g_ctx;
    if (!ctx || !pszFile) return -1;

    std::wstring path;
    HANDLE       h = INVALID_HANDLE_VALUE;

    if (pszFile[0] == '*' && pszFile[1] == '\0') {
        // Quantum decompression spill file request.
        static unsigned long spillNo = 0;
        path = util::path::join(ctx->tempDir,
                                util::fmt(L"spill_%lu.bin", ++spillNo));
        h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            ctx->spillPath = path;
            ctx->spillHandle = reinterpret_cast<INT_PTR>(h);
            auto* spill = reinterpret_cast<PFDISPILLFILE>(pszFile);
            if (spill->cbFile > 0) {
                LARGE_INTEGER size{};
                size.QuadPart = spill->cbFile;
                if (SetFilePointerEx(h, size, nullptr, FILE_BEGIN))
                    SetEndOfFile(h);
                LARGE_INTEGER zero{};
                SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
            }
        }
        return reinterpret_cast<INT_PTR>(h);
    }

    // We pass UTF-8 encoded paths down to FDI; convert back to UTF-16 here.
    path = util::widen(pszFile, CP_UTF8);
    if (path.empty()) return -1;

    DWORD access = GENERIC_READ;
    if ((oflag & _O_RDWR) == _O_RDWR)
        access = GENERIC_READ | GENERIC_WRITE;
    else if (oflag & _O_WRONLY)
        access = GENERIC_WRITE;

    DWORD creation = OPEN_EXISTING;
    if (oflag & _O_CREAT)
        creation = (oflag & _O_EXCL) ? CREATE_NEW
                                     : ((oflag & _O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS);
    else if (oflag & _O_TRUNC)
        creation = TRUNCATE_EXISTING;

    h = CreateFileW(path.c_str(), access,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                    creation, FILE_ATTRIBUTE_NORMAL, nullptr);
    return reinterpret_cast<INT_PTR>(h);
}

FNREAD(fdiRead) {
    DWORD got = 0;
    if (!ReadFile(reinterpret_cast<HANDLE>(hf), pv, cb, &got, nullptr)) return static_cast<UINT>(-1);
    return got;
}

FNWRITE(fdiWrite) {
    DWORD wrote = 0;
    if (!WriteFile(reinterpret_cast<HANDLE>(hf), pv, cb, &wrote, nullptr))
        return static_cast<UINT>(-1);
    return wrote;
}

FNCLOSE(fdiClose) {
    HANDLE h = reinterpret_cast<HANDLE>(hf);
    if (!h || h == INVALID_HANDLE_VALUE) return -1;
    BOOL ok = CloseHandle(h);
    if (g_ctx && g_ctx->spillHandle == hf && !g_ctx->spillPath.empty()) {
        DeleteFileW(g_ctx->spillPath.c_str());
        g_ctx->spillPath.clear();
        g_ctx->spillHandle = -1;
    }
    return ok ? 0 : -1;
}

FNSEEK(fdiSeek) {
    LARGE_INTEGER distance{};
    distance.QuadPart = dist;
    LARGE_INTEGER pos{};
    DWORD method = seektype == SEEK_CUR ? FILE_CURRENT
                                        : (seektype == SEEK_END ? FILE_END : FILE_BEGIN);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(hf), distance, &pos, method)) return -1L;
    return static_cast<long>(pos.QuadPart);
}

FNFDINOTIFY(fdiNotify) {
    Context* ctx = g_ctx;
    if (!ctx || !ctx->result) return -1;

    switch (fdint) {
        case fdintCOPY_FILE: {
            std::wstring name = util::fromAcp(pfdin->psz1 ? pfdin->psz1 : "");
            std::wstring key = util::toLower(name);
            std::wstring target;
            auto it = ctx->map->find(key);
            if (it != ctx->map->end())
                target = it->second;
            else
                target = util::path::join(ctx->fallbackDir,
                                          util::path::sanitizeRelative(name));

            util::verboseLine(ctx->verbose, name + L"  ->  " + target);

            if (ctx->dryRun) {
                ctx->result->extracted.push_back(key);
                return 0;  // skip decompression, just report the plan
            }
            if (ctx->skipExisting && util::path::exists(target)) {
                ctx->result->skipped.push_back(key);
                return 0;
            }

            std::wstring dir = util::path::dirName(target);
            if (!dir.empty() && !util::path::makeDirs(dir)) {
                ctx->result->warnings.push_back(L"Cannot create directory " + dir);
                return 0;
            }
            HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                ctx->result->warnings.push_back(util::fmt(L"Cannot create %s (Win32 %lu)",
                                                        target.c_str(),
                                                        static_cast<unsigned long>(GetLastError())));
                return 0;
            }
            INT_PTR hf = reinterpret_cast<INT_PTR>(h);
            ctx->handleNames[hf] = key;
            return hf;
        }

        case fdintCLOSE_FILE_INFO: {
            HANDLE h = reinterpret_cast<HANDLE>(pfdin->hf);
            auto it = ctx->handleNames.find(static_cast<INT_PTR>(pfdin->hf));
            if (it != ctx->handleNames.end()) {
                ctx->result->extracted.push_back(it->second);
                ctx->handleNames.erase(it);
            }
            if (h && h != INVALID_HANDLE_VALUE) {
                FILETIME ft{};
                if (DosDateTimeToFileTime(pfdin->date, pfdin->time, &ft))
                    SetFileTime(h, nullptr, nullptr, &ft);
                CloseHandle(h);
            }
            return TRUE;
        }

        case fdintNEXT_CABINET:
            ctx->spanning = true;
            return -1;  // files spanning multiple cabinets are not supported

        case fdintPARTIAL_FILE:
            ctx->result->warnings.push_back(
                L"CAB contains a partial file (continued from another cabinet)");
            return 0;

        default:
            return 0;
    }
}

}  // namespace

Result extract(const std::wstring& cabPath,
               const std::unordered_map<std::wstring, std::wstring>& nameToTarget,
               const std::wstring& fallbackDir,
               const std::wstring& tempDir,
               bool dryRun,
               bool skipExisting,
               bool verbose) {
    Result result;
    if (!util::path::exists(cabPath)) {
        result.error = L"Cabinet not found: " + cabPath;
        return result;
    }

    Context ctx;
    ctx.map = &nameToTarget;
    ctx.fallbackDir = fallbackDir;
    ctx.tempDir = tempDir;
    ctx.dryRun = dryRun;
    ctx.skipExisting = skipExisting;
    ctx.verbose = verbose;
    ctx.result = &result;

    ERF erf{};
    HFDI hfdi = FDICreate(fdiAlloc, fdiFree, fdiOpen, fdiRead, fdiWrite, fdiClose,
                          fdiSeek, cpuUNKNOWN, &erf);
    if (!hfdi) {
        result.error = L"FDICreate failed: " + fdiErrorText(erf);
        return result;
    }

    std::string nameU8 = util::narrow(util::path::fileName(cabPath));
    std::string dirU8 = util::narrow(util::path::dirName(cabPath));
    if (dirU8.empty()) dirU8 = ".\\";
    std::vector<char> cabinet(nameU8.begin(), nameU8.end());
    cabinet.push_back('\0');
    std::vector<char> cabDir(dirU8.begin(), dirU8.end());
    cabDir.push_back('\0');

    g_ctx = &ctx;
    BOOL ok = FDICopy(hfdi, cabinet.data(), cabDir.data(), 0, fdiNotify, nullptr, nullptr);
    g_ctx = nullptr;
    FDIDestroy(hfdi);

    if (!ok) {
        result.error = ctx.spanning
                           ? L"CAB set spans multiple cabinets; this is not supported"
                           : L"FDICopy(" + cabPath + L"): " + fdiErrorText(erf);
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace cab
