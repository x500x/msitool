#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace util {

// All errors are wide-string based. main() catches and prints them as UTF-8.
class Error : public std::runtime_error {
public:
    explicit Error(std::wstring msg)
        : std::runtime_error("msitool error"), msg_(std::move(msg)) {}
    const std::wstring& message() const noexcept { return msg_; }

private:
    std::wstring msg_;
};

[[noreturn]] inline void fail(const std::wstring& msg) { throw Error(msg); }

// ---- text / encoding -------------------------------------------------------
std::wstring widen(const std::string& s, UINT codePage = CP_UTF8);
std::wstring widen(const char* s, UINT codePage = CP_UTF8);
std::string  narrow(const std::wstring& s, UINT codePage = CP_UTF8);
std::wstring fromAcp(const char* s);
std::wstring toLower(std::wstring s);
std::wstring trim(std::wstring s);
bool startsWith(const std::wstring& s, const std::wstring& prefix);
bool iequals(const std::wstring& a, const std::wstring& b);
std::wstring replaceAll(std::wstring s, const std::wstring& from, const std::wstring& to);
std::wstring fmt(const wchar_t* format, ...);

// ---- console ---------------------------------------------------------------
// GUI mode installs a sink: every line that the CLI commands print through
// out()/outLine()/errLine() is also delivered to the window's log view. The
// partial writes of out() are buffered per thread until a line is completed.
using LogSink = std::function<void(bool isError, const std::wstring& line)>;
void setLogSink(LogSink sink);

void setConsoleUtf8();
void out(const std::wstring& s);
void outLine(const std::wstring& s);
void errLine(const std::wstring& s);
void verboseLine(bool verbose, const std::wstring& s);
// Non-fatal problem: reported on stderr (and the GUI log) but never throws.
inline void warn(const std::wstring& s) { errLine(L"warning: " + s); }

struct Table {
    std::vector<std::wstring>              headers;
    std::vector<std::vector<std::wstring>> rows;
};
void printTable(const Table& table, bool tsv);

// ---- errors ----------------------------------------------------------------
std::wstring winError(DWORD code);
std::wstring msiError(UINT code, const std::wstring& context = L"");
inline void checkWin(bool ok, const std::wstring& context) {
    if (!ok) fail(context + L": " + winError(GetLastError()));
}
inline void checkMsi(UINT rc, const std::wstring& context) {
    if (rc != ERROR_SUCCESS) fail(context + L": " + msiError(rc));
}

// ---- paths -----------------------------------------------------------------
namespace path {

std::wstring full(const std::wstring& p);
std::wstring dirName(const std::wstring& p);
std::wstring fileName(const std::wstring& p);
std::wstring ext(const std::wstring& p);
std::wstring join(const std::wstring& a, const std::wstring& b);
std::wstring parent(const std::wstring& p);
bool exists(const std::wstring& p);
bool isDirectory(const std::wstring& p);
bool makeDirs(const std::wstring& p);
bool copyFile(const std::wstring& src, const std::wstring& dst, bool overwrite = true);
bool writeFile(const std::wstring& p, const void* data, size_t size);
bool writeFile(const std::wstring& p, const std::vector<unsigned char>& data);
void removeTree(const std::wstring& p);

std::wstring tempRoot();
std::wstring makeTempDir(const std::wstring& prefix);

// MSI "short|long" -> "long"; also handles the special "." directory marker.
std::wstring longName(const std::wstring& shortLong);
// Make one path component safe on Windows (no traversal, no illegal chars).
std::wstring sanitizeComponent(const std::wstring& name);
// Split on \ or /, drop "." / ".." and re-join with backslashes.
std::wstring sanitizeRelative(const std::wstring& rel);

}  // namespace path

class TempDir {
public:
    explicit TempDir(const std::wstring& prefix);
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::wstring& path() const noexcept { return path_; }
    void keep() noexcept { keep_ = true; }

private:
    std::wstring path_;
    bool         keep_ = false;
};

}  // namespace util
