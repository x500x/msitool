#include "util.h"

#include <msi.h>
#include <msiquery.h>

#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <algorithm>

namespace util {

// ---- text / encoding -------------------------------------------------------

std::wstring widen(const std::string& s, UINT codePage) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring r(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(codePage, 0, s.data(), static_cast<int>(s.size()), r.data(), n);
    return r;
}

std::wstring widen(const char* s, UINT codePage) {
    return s ? widen(std::string(s), codePage) : L"";
}

std::wstring fromAcp(const char* s) {
    // Cabinet file names are raw CAB header bytes (normally plain ASCII).
    // Try UTF-8 first, fall back to the active code page.
    if (!s || !*s) return L"";
    std::wstring u8 = widen(s, CP_UTF8);
    if (!u8.empty()) return u8;
    return widen(s, CP_ACP);
}

std::string narrow(const std::wstring& s, UINT codePage) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(codePage, 0, s.data(), static_cast<int>(s.size()),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string r(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(codePage, 0, s.data(), static_cast<int>(s.size()),
                        r.data(), n, nullptr, nullptr);
    return r;
}

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(towlower(static_cast<wint_t>(c)));
    });
    return s;
}

std::wstring trim(std::wstring s) {
    const wchar_t* ws = L" \t\r\n";
    auto b = s.find_first_not_of(ws);
    if (b == std::wstring::npos) return L"";
    auto e = s.find_last_not_of(ws);
    return s.substr(b, e - b + 1);
}

bool startsWith(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool iequals(const std::wstring& a, const std::wstring& b) {
    return toLower(a) == toLower(b);
}

std::wstring replaceAll(std::wstring s, const std::wstring& from, const std::wstring& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::wstring::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::wstring fmt(const wchar_t* format, ...) {
    va_list ap;
    va_start(ap, format);
    wchar_t buf[1024];
    int n = _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, format, ap);
    va_end(ap);
    if (n < 0) {
        va_start(ap, format);
        std::vector<wchar_t> big(8192);
        _vsnwprintf_s(big.data(), big.size(), _TRUNCATE, format, ap);
        va_end(ap);
        return std::wstring(big.data());
    }
    return std::wstring(buf);
}

// ---- console ---------------------------------------------------------------

static void writeUtf8(FILE* stream, const std::wstring& s) {
    std::string bytes = narrow(s, CP_UTF8);
    if (!bytes.empty()) fwrite(bytes.data(), 1, bytes.size(), stream);
}

// Installed once by the GUI before any worker thread starts, and never
// changed while workers are running (the callback itself is stateless).
static LogSink g_logSink;
static thread_local std::wstring g_pending;

static void publishLine(bool isError, const std::wstring& line) {
    std::wstring full = g_pending + line;
    g_pending.clear();
    if (g_logSink) g_logSink(isError, full);
}

void setLogSink(LogSink sink) { g_logSink = std::move(sink); }

void setConsoleUtf8() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
}

void out(const std::wstring& s) {
    writeUtf8(stdout, s);
    if (g_logSink) g_pending += s;
}
void outLine(const std::wstring& s) {
    writeUtf8(stdout, s);
    fputc('\n', stdout);
    if (g_logSink) publishLine(false, s);
}
void errLine(const std::wstring& s) {
    writeUtf8(stderr, s);
    fputc('\n', stderr);
    fflush(stderr);
    if (g_logSink) {
        if (!g_pending.empty()) publishLine(false, L"");
        publishLine(true, s);
    }
}
void verboseLine(bool verbose, const std::wstring& s) {
    if (verbose) errLine(L"[v] " + s);
}

static int charWidth(wchar_t c) {
    if (c < 0x20) return 0;
    if ((c >= 0x0300 && c <= 0x036F) || (c >= 0x1AB0 && c <= 0x1AFF) ||
        (c >= 0xFE00 && c <= 0xFE0F) || (c >= 0x200B && c <= 0x200F))
        return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) ||
        (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
        (c >= 0xFE10 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}

static int displayWidth(const std::wstring& s) {
    int w = 0;
    for (wchar_t c : s) w += charWidth(c);
    return w;
}

// Make control characters visible: MSI stream names such as \x05DigitalSignature
// otherwise show up as garbage in a terminal.
static std::wstring visible(const std::wstring& s) {
    std::wstring r;
    for (wchar_t c : s) {
        if (c == L'\r') r += L"\\r";
        else if (c == L'\n') r += L"\\n";
        else if (c == L'\t') r += L"\\t";
        else if (c < 0x20 || c == 0x7F) r += util::fmt(L"\\x%02X", static_cast<unsigned>(c));
        else r.push_back(c);
    }
    return r;
}

static std::wstring fit(const std::wstring& s, int width) {
    std::wstring r;
    int used = 0;
    for (wchar_t c : s) {
        int cw = charWidth(c);
        if (used + cw > width) {
            if (width >= 2) r.push_back(L'\u2026');
            break;
        }
        r.push_back(c);
        used += cw;
    }
    return r;
}

static std::wstring pad(const std::wstring& s, int width) {
    std::wstring r = fit(visible(s), width);
    int w = displayWidth(r);
    if (w < width) r.append(static_cast<size_t>(width - w), L' ');
    return r;
}

static std::wstring tsvCell(const std::wstring& s) { return visible(s); }

void printTable(const Table& table, bool tsv) {
    const size_t cols = table.headers.size();
    if (cols == 0) return;

    if (tsv) {
        std::wstring line;
        for (size_t i = 0; i < cols; ++i) {
            if (i) line += L'\t';
            line += tsvCell(table.headers[i]);
        }
        outLine(line);
        for (const auto& row : table.rows) {
            line.clear();
            for (size_t i = 0; i < cols; ++i) {
                if (i) line += L'\t';
                line += tsvCell(i < row.size() ? row[i] : L"");
            }
            outLine(line);
        }
        return;
    }

    constexpr int kMaxCol = 64;
    std::vector<int> widths(cols, 1);
    for (size_t i = 0; i < cols; ++i)
        widths[i] = std::clamp(displayWidth(table.headers[i]), 1, kMaxCol);
    for (const auto& row : table.rows) {
        for (size_t i = 0; i < cols && i < row.size(); ++i) {
            int w = displayWidth(visible(row[i]));
            widths[i] = std::clamp(std::max(widths[i], w), 1, kMaxCol);
        }
    }

    auto emitRow = [&](const std::vector<std::wstring>& row) {
        std::wstring line;
        for (size_t i = 0; i < cols; ++i) {
            if (i) line += L" | ";
            line += pad(i < row.size() ? row[i] : L"", widths[i]);
        }
        // trim trailing padding of the last column
        while (!line.empty() && line.back() == L' ') line.pop_back();
        outLine(line);
    };
    emitRow(table.headers);
    {
        std::wstring sep;
        for (size_t i = 0; i < cols; ++i) {
            if (i) sep += L"-+-";
            sep.append(static_cast<size_t>(widths[i]), L'-');
        }
        outLine(sep);
    }
    for (const auto& row : table.rows) emitRow(row);
}

// ---- errors ----------------------------------------------------------------

std::wstring winError(DWORD code) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring r;
    if (n && buf) r = trim(std::wstring(buf, n));
    if (buf) LocalFree(buf);
    if (r.empty()) r = fmt(L"Win32 error %lu", static_cast<unsigned long>(code));
    return r;
}

std::wstring msiError(UINT code, const std::wstring& context) {
    std::wstring r;
    wchar_t buf[1024] = {};
    DWORD n = FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, 0, buf, _countof(buf), nullptr);
    if (n) r = trim(std::wstring(buf, n));

    // Installer-specific messages live in msi.dll's message table.
    if (r.empty()) {
        HMODULE mod = LoadLibraryExW(L"msi.dll", nullptr, LOAD_LIBRARY_AS_DATAFILE);
        if (mod) {
            wchar_t buf2[1024] = {};
            DWORD n2 = FormatMessageW(FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
                                      mod, code, 0, buf2, _countof(buf2), nullptr);
            if (n2) r = trim(std::wstring(buf2, n2));
            FreeLibrary(mod);
        }
    }
    if (r.empty()) r = fmt(L"Installer error %u", code);

    // MsiGetLastErrorRecord often carries a more precise description.
    MSIHANDLE hRec = MsiGetLastErrorRecord();
    if (hRec) {
        wchar_t detail[1024] = {};
        DWORD len = _countof(detail);
        UINT rc = MsiRecordGetStringW(hRec, 1, detail, &len);
        if (rc == ERROR_SUCCESS && detail[0]) {
            std::wstring extra = trim(std::wstring(detail));
            if (!extra.empty() && extra != r) r += L" (" + extra + L")";
        }
        MsiCloseHandle(hRec);
    }
    if (!context.empty()) return context + L": " + r + fmt(L" [code %u]", code);
    return r + fmt(L" [code %u]", code);
}

// ---- paths -----------------------------------------------------------------

namespace path {

std::wstring full(const std::wstring& p) {
    if (p.empty()) return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring r(n, L'\0');
    DWORD got = GetFullPathNameW(p.c_str(), n, r.data(), nullptr);
    if (!got || got >= n) return p;
    r.resize(got);
    return r;
}

std::wstring dirName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return L"";
    if (pos == 2 && p.size() == 3) return p;  // "C:\"
    return p.substr(0, pos + 1);              // keep trailing separator
}

std::wstring fileName(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    return pos == std::wstring::npos ? p : p.substr(pos + 1);
}

std::wstring ext(const std::wstring& p) {
    std::wstring f = fileName(p);
    size_t pos = f.rfind(L'.');
    return pos == std::wstring::npos ? L"" : f.substr(pos);
}

std::wstring join(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    wchar_t last = a.back();
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

std::wstring parent(const std::wstring& p) {
    std::wstring d = dirName(p);
    if (d.empty()) return L"";
    if (d.back() == L'\\' || d.back() == L'/') d.pop_back();
    // "C:" (drive-relative) is not the same path as "C:\" (drive root).
    if (d.size() == 2 && d[1] == L':') d.push_back(L'\\');
    return d;
}

bool exists(const std::wstring& p) {
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(const std::wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

bool makeDirs(const std::wstring& p) {
    if (p.empty()) return false;
    if (isDirectory(p)) return true;
    if (exists(p)) return false;
    std::wstring par = parent(p);
    if (!par.empty() && !isDirectory(par) && !makeDirs(par)) return false;
    return CreateDirectoryW(p.c_str(), nullptr) != 0 || isDirectory(p);
}

bool copyFile(const std::wstring& src, const std::wstring& dst, bool overwrite) {
    std::wstring d = dirName(dst);
    if (!d.empty() && !makeDirs(d)) return false;
    return CopyFileW(src.c_str(), dst.c_str(), overwrite ? FALSE : TRUE) != 0;
}

bool writeFile(const std::wstring& p, const void* data, size_t size) {
    std::wstring d = dirName(p);
    if (!d.empty() && !makeDirs(d)) return false;
    HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    const char* bytes = static_cast<const char*>(data);
    size_t done = 0;
    while (done < size) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - done, 1u << 20));
        DWORD wrote = 0;
        if (!WriteFile(h, bytes + done, chunk, &wrote, nullptr) || wrote == 0) break;
        done += wrote;
    }
    if (done < size) {
        CloseHandle(h);
        DeleteFileW(p.c_str());
        return false;
    }
    CloseHandle(h);
    return true;
}

bool writeFile(const std::wstring& p, const std::vector<unsigned char>& data) {
    return writeFile(p, data.data(), data.size());
}

void removeTree(const std::wstring& p) {
    if (!isDirectory(p)) {
        DeleteFileW(p.c_str());
        return;
    }
    std::wstring pattern = join(p, L"*");
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            removeTree(join(p, fd.cFileName));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(p.c_str());
}

std::wstring tempRoot() {
    wchar_t buf[MAX_PATH + 1] = {};
    DWORD n = GetTempPathW(_countof(buf), buf);
    if (!n) return full(L".\\");
    return std::wstring(buf, n);
}

std::wstring makeTempDir(const std::wstring& prefix) {
    static unsigned long counter = 0;
    for (int i = 0; i < 100; ++i) {
        std::wstring candidate = join(tempRoot(), prefix + fmt(L"_%lu_%lu",
                                      static_cast<unsigned long>(GetCurrentProcessId()),
                                      ++counter));
        if (CreateDirectoryW(candidate.c_str(), nullptr)) return candidate;
        if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_PATH_NOT_FOUND)
            break;
    }
    fail(L"Cannot create a temporary directory under " + tempRoot());
}

std::wstring longName(const std::wstring& shortLong) {
    std::wstring s = shortLong;
    size_t bar = s.find(L'|');
    if (bar != std::wstring::npos) s = s.substr(bar + 1);
    s = trim(s);
    if (s == L"." || s == L"..") return L"";  // "same as parent" marker
    return s;
}

std::wstring sanitizeComponent(const std::wstring& name) {
    std::wstring s = trim(name);
    // A trailing dot/space is silently stripped by Win32; avoid surprises.
    while (!s.empty() && (s.back() == L'.' || s.back() == L' ')) s.pop_back();
    std::wstring r;
    for (wchar_t c : s) {
        if (c < 0x20 || wcschr(L"<>:\"/\\|?*", c) != nullptr) {
            r.push_back(L'_');
        } else {
            r.push_back(c);
        }
    }
    if (r.empty()) r = L"_";
    if (r == L"." || r == L"..") r = L"_";
    // Reserved DOS device names.
    static const wchar_t* reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2",
                                        L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8",
                                        L"COM9", L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5",
                                        L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    std::wstring base = r;
    size_t dot = base.find(L'.');
    if (dot != std::wstring::npos) base = base.substr(0, dot);
    for (const wchar_t* x : reserved)
        if (iequals(base, x)) { r = L"_" + r; break; }
    if (r.size() > 160) r.resize(160);
    return r;
}

std::wstring sanitizeRelative(const std::wstring& rel) {
    std::wstring out;
    size_t i = 0;
    while (i < rel.size()) {
        while (i < rel.size() && (rel[i] == L'\\' || rel[i] == L'/')) ++i;
        size_t start = i;
        while (i < rel.size() && rel[i] != L'\\' && rel[i] != L'/') ++i;
        if (i == start) break;
        std::wstring part = rel.substr(start, i - start);
        if (part == L".") continue;
        std::wstring safe = sanitizeComponent(part);
        out = out.empty() ? safe : out + L"\\" + safe;
    }
    return out;
}

}  // namespace path

TempDir::TempDir(const std::wstring& prefix) : path_(path::makeTempDir(prefix)) {}
TempDir::~TempDir() {
    if (!keep_ && !path_.empty()) path::removeTree(path_);
}

}  // namespace util
