#pragma once

#include "util.h"
#include <msi.h>
#include <msiquery.h>

namespace msi {

// RAII owner of an MSIHANDLE (database / view / record / summary info).
class Handle {
public:
    Handle() noexcept = default;
    explicit Handle(MSIHANDLE h) noexcept : h_(h) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& other) noexcept : h_(other.release()) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) reset(other.release());
        return *this;
    }
    void reset(MSIHANDLE h = 0) noexcept {
        if (h_) MsiCloseHandle(h_);
        h_ = h;
    }
    MSIHANDLE release() noexcept {
        MSIHANDLE h = h_;
        h_ = 0;
        return h;
    }
    MSIHANDLE* out() noexcept {
        reset();
        return &h_;
    }
    MSIHANDLE get() const noexcept { return h_; }
    explicit operator bool() const noexcept { return h_ != 0; }

private:
    MSIHANDLE h_ = 0;
};

struct QueryResult {
    std::vector<std::wstring>              columns;  // column names
    std::vector<std::wstring>              types;    // MSI type strings: S255, i4, v0 ...
    std::vector<std::vector<std::wstring>> rows;     // every cell rendered as text
};

struct SummaryItem {
    UINT        id = 0;
    std::wstring name;
    std::wstring value;
};

// Lightweight preflight check performed before MsiOpenDatabaseW: verifies
// that the input really is an OLE compound file (MSI/MSP storage).
struct FileProbe {
    std::wstring       path;
    unsigned long long size = 0;
    std::wstring       signatureHex;   // first 8 bytes, uppercase hex
    bool               signatureOk = false;
    bool               isPatch = false;
    std::wstring       error;          // empty => preflight passed
};

FileProbe probe(const std::wstring& path);

class Db {
public:
    Db() = default;
    // Opens a package read-only. Throws util::Error on failure.
    explicit Db(const std::wstring& path);
    ~Db() = default;
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;

    MSIHANDLE handle() const noexcept { return h_.get(); }
    const std::wstring& path() const noexcept { return path_; }
    bool isPatch() const noexcept { return isPatch_; }

    // Applies a Windows Installer transform (.mst) as a temporary view on
    // this read-only database. Every later query sees the transformed rows.
    void applyTransform(const std::wstring& path, int errorConditions = 0);

    // SELECT-style query, optionally with '?' parameters (all passed as strings).
    QueryResult query(const std::wstring& sql,
                      const std::vector<std::wstring>& params = {}) const;
    // Returns the first cell of the first row, or L"" when there is none.
    std::wstring scalar(const std::wstring& sql,
                        const std::vector<std::wstring>& params = {}) const;
    // MSI SQL has no COUNT(*): count rows by fetching them. Returns (size_t)-1
    // when the table cannot be enumerated.
    size_t rowCount(const std::wstring& table) const;

    // Column names/types of a table without fetching any rows.
    QueryResult columnsOf(const std::wstring& table) const;
    std::vector<std::wstring> tableNames() const;
    std::wstring resolveTable(const std::wstring& name) const;
    bool tableExists(const std::wstring& name) const;
    bool isPersistent(const std::wstring& table) const;
    // Works for tables that _Tables hides, such as _Streams / _Storages.
    bool canOpenTable(const std::wstring& table) const;
    std::vector<std::wstring> primaryKeys(const std::wstring& table) const;
    std::vector<std::wstring> streamNames() const;

    // Whole-table scan used by the transform diff: every cell is rendered as
    // text, and binary (v*) fields become "v:<size>:<fnv64 hex>" so that a
    // changed payload is visible even when the row key stays the same.
    QueryResult scanTable(const std::wstring& table, bool hashStreams = true) const;

    // Reads a binary/stream column through MsiRecordReadStream.
    std::vector<unsigned char> readStream(const std::wstring& sql,
                                          const std::vector<std::wstring>& params = {},
                                          unsigned field = 1) const;

    UINT exportTable(const std::wstring& table,
                     const std::wstring& folder,
                     const std::wstring& file) const;

    std::wstring property(const std::wstring& name) const;
    std::vector<SummaryItem> summary() const;

private:
    Handle      h_;
    std::wstring path_;
    bool        isPatch_ = false;
};

// Escape an identifier for use inside backticks in MSI SQL.
std::wstring quoteIdent(const std::wstring& name);

}  // namespace msi
