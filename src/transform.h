#pragma once

#include "util.h"
#include "msidb.h"

#include <string>
#include <utility>
#include <vector>

// Windows Installer transform (.mst) support:
//   * generate  - diff two databases into a transform (MsiDatabaseGenerateTransform)
//   * apply     - apply transforms to a copy of a database and commit it
//                 (MsiDatabaseApplyTransform + MsiDatabaseCommit)
//   * inspect   - describe a standalone .mst file (changed tables, summary info)
namespace mst {

// MSITRANSFORM_ERROR_* / MSITRANSFORM_VALIDATE_* bit sets. The numeric values
// come straight from MsiQuery.h so both options can be passed to
// MsiDatabaseApplyTransformW / MsiCreateTransformSummaryInfoW.
struct Flags {
    int errors = 0;      // MSITRANSFORM_ERROR_*    (errors to suppress)
    int validation = 0;  // MSITRANSFORM_VALIDATE_* (properties to validate)
};

// "addexistingrow,delmissingrow" / "0x11" / "" -> bit set.
// Unknown names raise util::Error listing the accepted ones.
int  parseErrorFlags(const std::wstring& text);
int  parseValidationFlags(const std::wstring& text);
// "addexistingrow+changecodepage" / "none" - used by --verbose and `mst`.
std::wstring describeErrorFlags(int flags);
std::wstring describeValidationFlags(int flags);

// One row of a table-by-table comparison of two databases.
struct TableDiff {
    std::wstring table;
    size_t       added = 0;
    size_t       removed = 0;
    size_t       modified = 0;
    bool         schemaChanged = false;  // same name, different columns/types
    bool         onlyBase = false;       // table exists only in the base package
    bool         onlyUpdated = false;    // table exists only in the updated package

    size_t changes() const noexcept { return added + removed + modified; }
};

// Compares every persistent table of two databases. Row identity comes from
// the table's primary key (content hash for keyless tables); binary columns
// are compared by an FNV-1a fingerprint of their payload.
std::vector<TableDiff> diff(msi::Db& base, msi::Db& updated, bool hashStreams = true);

struct GenerateOptions {
    std::wstring baseMsi;     // reference (original) package
    std::wstring updatedMsi;  // changed package
    std::wstring outMst;      // transform to write
    Flags        flags;
    bool         summaryInfo = true;  // MsiCreateTransformSummaryInfo
    bool         force = false;
    bool         verbose = false;
    bool         dryRun = false;      // only compare, write nothing
};

struct GenerateResult {
    std::wstring       outMst;
    unsigned long long size = 0;
    bool               written = false;
    bool               identical = false;  // nothing to transform (ERROR_NO_DATA)
    std::vector<TableDiff> tables;  // per-table change counts
};

GenerateResult generate(const GenerateOptions& o);

struct ApplyOptions {
    std::wstring              baseMsi;
    std::wstring              outMsi;
    std::vector<std::wstring> transforms;  // applied in order
    Flags                     flags;
    bool                      force = false;
    bool                      verbose = false;
    bool                      verifyResult = true;
};

struct ApplyResult {
    std::wstring outMsi;
    size_t       applied = 0;
    std::wstring packageKind;  // "installable" or the verify error
};

ApplyResult apply(const ApplyOptions& o);

// Read-only description of a transform file. The table streams inside a .mst
// carry obfuscated names, so the changed tables come from the installer
// itself: viewTransform() applies the transform as a _TransformView on a base
// package (the same mechanism Orca uses).
struct TransformInfo {
    std::wstring       path;
    unsigned long long size = 0;
    bool               oleOk = false;      // OLE compound-file signature
    bool               hasSummary = false;
    // Decoded from the summary stream: the installer stores
    // (validation << 16) | errorConditions in the "Character count" slot.
    int                errorConditions = 0;
    int                validation = 0;
    std::wstring       targetProduct;      // ProductCode / version the MST applies to
    std::vector<std::pair<std::wstring, unsigned long long>> streams;
    std::vector<msi::SummaryItem> summary;
};

TransformInfo inspect(const std::wstring& path);

// One row of the installer's _TransformView table.
struct ChangeRow {
    std::wstring table;
    std::wstring column;
    std::wstring row;      // primary key of the affected row
    std::wstring data;     // new value
    std::wstring current;  // existing value in the base package
};

// Applies the transform to baseMsi with MSITRANSFORM_ERROR_VIEWTRANSFORM and
// reads the temporary _TransformView table. The base package is opened
// read-only and never modified.
std::vector<ChangeRow> viewTransform(const std::wstring& baseMsi,
                                     const std::wstring& transform);

}  // namespace mst
