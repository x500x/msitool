#include "transform.h"

#include <msi.h>
#include <msiquery.h>
#include <ole2.h>  // StgOpenStorage (coml2api.h) + property sets

#include <algorithm>
#include <cwctype>
#include <map>
#include <set>
#include <memory>

namespace mst {

namespace {

struct FlagDef {
    const wchar_t* name;
    int            value;
};

// MSITRANSFORM_ERROR_* (MsiQuery.h): installer conditions that are reported
// as an error while a transform is applied.
const FlagDef kErrorFlags[] = {
    {L"addexistingrow",   MSITRANSFORM_ERROR_ADDEXISTINGROW},
    {L"delmissingrow",    MSITRANSFORM_ERROR_DELMISSINGROW},
    {L"addexistingtable", MSITRANSFORM_ERROR_ADDEXISTINGTABLE},
    {L"delmissingtable",  MSITRANSFORM_ERROR_DELMISSINGTABLE},
    {L"updatemissingrow", MSITRANSFORM_ERROR_UPDATEMISSINGROW},
    {L"changecodepage",   MSITRANSFORM_ERROR_CHANGECODEPAGE},
    {L"viewtransform",    MSITRANSFORM_ERROR_VIEWTRANSFORM},
};

// MSITRANSFORM_VALIDATE_*: properties checked against the target package
// before the transform is applied.
const FlagDef kValidationFlags[] = {
    {L"language",                 MSITRANSFORM_VALIDATE_LANGUAGE},
    {L"product",                  MSITRANSFORM_VALIDATE_PRODUCT},
    {L"platform",                 MSITRANSFORM_VALIDATE_PLATFORM},
    {L"majorversion",             MSITRANSFORM_VALIDATE_MAJORVERSION},
    {L"minorversion",             MSITRANSFORM_VALIDATE_MINORVERSION},
    {L"updateversion",            MSITRANSFORM_VALIDATE_UPDATEVERSION},
    {L"newlessbaseversion",       MSITRANSFORM_VALIDATE_NEWLESSBASEVERSION},
    {L"newlessequalbaseversion",  MSITRANSFORM_VALIDATE_NEWLESSEQUALBASEVERSION},
    {L"newequalbaseversion",      MSITRANSFORM_VALIDATE_NEWEQUALBASEVERSION},
    {L"newgreaterequalbaseversion", MSITRANSFORM_VALIDATE_NEWGREATEREQUALBASEVERSION},
    {L"newgreaterbaseversion",    MSITRANSFORM_VALIDATE_NEWGREATERBASEVERSION},
    {L"upgradecode",              MSITRANSFORM_VALIDATE_UPGRADECODE},
};

std::vector<std::wstring> splitList(const std::wstring& text) {
    std::vector<std::wstring> parts;
    std::wstring cur;
    for (wchar_t c : text) {
        if (c == L',' || c == L';' || c == L'|' || iswspace(static_cast<wint_t>(c))) {
            if (!cur.empty()) { parts.push_back(cur); cur.clear(); }
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

int parseFlags(const std::wstring& text, const FlagDef* defs, size_t count,
               const wchar_t* kind) {
    int bits = 0;
    for (const std::wstring& raw : splitList(text)) {
        std::wstring token = util::toLower(raw);
        if (token == L"none" || token == L"0") continue;

        // Numeric form: 0x11 / 17. Lets callers pass raw MSITRANSFORM bits.
        wchar_t* end = nullptr;
        unsigned long numeric = wcstoul(token.c_str(), &end, 0);
        if (end && *end == L'\0' && end != token.c_str()) {
            bits |= static_cast<int>(numeric);
            continue;
        }

        bool matched = false;
        for (size_t i = 0; i < count; ++i) {
            if (token == defs[i].name) { bits |= defs[i].value; matched = true; break; }
        }
        if (!matched) {
            std::wstring known;
            for (size_t i = 0; i < count; ++i)
                known += std::wstring(known.empty() ? L"" : L", ") + defs[i].name;
            util::fail(L"Unknown " + std::wstring(kind) + L" flag '" + raw + L"'. Known: " +
                       known + L" (or a numeric value such as 0x11)");
        }
    }
    return bits;
}

std::wstring describeFlags(int bits, const FlagDef* defs, size_t count) {
    if (bits == 0) return L"none";
    std::wstring text;
    int covered = 0;
    for (size_t i = 0; i < count; ++i) {
        if ((bits & defs[i].value) == defs[i].value) {
            text += std::wstring(text.empty() ? L"" : L"+") + defs[i].name;
            covered |= defs[i].value;
        }
    }
    int rest = bits & ~covered;
    if (rest) text += std::wstring(text.empty() ? L"" : L"+") + util::fmt(L"0x%X", rest);
    return text;
}

// ---- file helpers ----------------------------------------------------------

unsigned long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// Opens a compound file read-only. StgOpenStorage is picky about sharing
// modes depending on what else holds the file, so try the common ones.
IStorage* openStorage(const std::wstring& path) {
    static const DWORD kModes[] = {
        STGM_READ | STGM_SHARE_EXCLUSIVE,
        STGM_READ | STGM_SHARE_DENY_WRITE,
        STGM_READ | STGM_SHARE_DENY_NONE,
    };
    for (DWORD mode : kModes) {
        IStorage* stg = nullptr;
        if (SUCCEEDED(StgOpenStorage(path.c_str(), nullptr, mode, nullptr, 0, &stg)) && stg)
            return stg;
    }
    return nullptr;
}

// Enumerates the streams of an OLE compound file (a transform is one).
std::vector<std::pair<std::wstring, unsigned long long>> listStreams(const std::wstring& path,
                                                                    bool& ok) {
    std::vector<std::pair<std::wstring, unsigned long long>> out;
    ok = false;
    IStorage* stg = openStorage(path);
    if (!stg) return out;
    ok = true;

    IEnumSTATSTG* en = nullptr;
    if (SUCCEEDED(stg->EnumElements(0, nullptr, 0, &en)) && en) {
        STATSTG s{};
        ULONG got = 0;
        while (en->Next(1, &s, &got) == S_OK && got == 1) {
            std::wstring name = s.pwcsName ? s.pwcsName : L"";
            if (s.pwcsName) CoTaskMemFree(s.pwcsName);
            unsigned long long size = static_cast<unsigned long long>(s.cbSize.QuadPart);
            if (s.type == STGTY_STREAM) out.emplace_back(name, size);
            else if (s.type == STGTY_STORAGE) out.emplace_back(name + L"/", size);
            got = 0;
        }
        en->Release();
    }
    stg->Release();
    return out;
}

// Reads the SummaryInformation property set of any OLE file. Works for .mst
// files, where MsiGetSummaryInformationW has no database handle to attach to.
std::vector<msi::SummaryItem> readSummary(const std::wstring& path) {
    static const struct { UINT id; const wchar_t* name; } kProps[] = {
        {2,  L"Title"},
        {3,  L"Subject"},
        {4,  L"Author"},
        {5,  L"Keywords"},
        {6,  L"Comments"},
        {7,  L"Template (platform;language)"},
        {8,  L"Last saved by"},
        {9,  L"Revision (base code;version; new code;version; UpgradeCode)"},
        {11, L"Last printed"},
        {12, L"Create time"},
        {13, L"Last save time"},
        {14, L"Page count (min. installer version)"},
        {15, L"Word count"},
        {16, L"Flags (validation << 16 | error conditions)"},
        {18, L"Creating application"},
        {19, L"Security"},
        {20, L"Transform flags"},
    };

    std::vector<msi::SummaryItem> out;
    IStorage* stg = openStorage(path);
    if (!stg) return out;

    IPropertySetStorage* sets = nullptr;
    IPropertyStorage*     sum = nullptr;
    if (SUCCEEDED(stg->QueryInterface(IID_IPropertySetStorage,
                                      reinterpret_cast<void**>(&sets))) &&
        sets) {
        if (SUCCEEDED(sets->Open(FMTID_SummaryInformation,
                                 STGM_READ | STGM_SHARE_EXCLUSIVE, &sum)) &&
            sum) {
            for (const auto& p : kProps) {
                PROPSPEC spec{};
                PROPVARIANT var{};
                spec.ulKind = PRSPEC_PROPID;
                spec.propid = p.id;
                PropVariantInit(&var);
                if (SUCCEEDED(sum->ReadMultiple(1, &spec, &var))) {
                    std::wstring text;
                    if (var.vt == VT_LPSTR && var.pszVal) {
                        text = util::widen(var.pszVal, CP_ACP);
                    } else if (var.vt == VT_LPWSTR && var.pwszVal) {
                        text = var.pwszVal;
                    } else if (var.vt == VT_I2) {
                        text = std::to_wstring(var.iVal);
                    } else if (var.vt == VT_I4) {
                        text = std::to_wstring(var.lVal);
                    } else if (var.vt == VT_UI2) {
                        text = std::to_wstring(var.uiVal);
                    } else if (var.vt == VT_UI4) {
                        text = std::to_wstring(var.ulVal);
                    } else if (var.vt == VT_FILETIME) {
                        SYSTEMTIME st{};
                        if (FileTimeToSystemTime(&var.filetime, &st))
                            text = util::fmt(L"%04u-%02u-%02u %02u:%02u:%02u UTC", st.wYear,
                                             st.wMonth, st.wDay, st.wHour, st.wMinute,
                                             st.wSecond);
                    }
                    if (!text.empty()) out.push_back({p.id, p.name, text});
                    PropVariantClear(&var);
                }
            }
            sum->Release();
        }
        sets->Release();
    }
    stg->Release();
    return out;
}

std::wstring packageProperty(msi::Db& db, const wchar_t* name) {
    if (!db.tableExists(L"Property")) return L"";
    return db.property(name);
}

}  // namespace

// ---- flags ----------------------------------------------------------------

int parseErrorFlags(const std::wstring& text) {
    return parseFlags(text, kErrorFlags, std::size(kErrorFlags), L"error-condition");
}

int parseValidationFlags(const std::wstring& text) {
    return parseFlags(text, kValidationFlags, std::size(kValidationFlags), L"validation");
}

std::wstring describeErrorFlags(int flags) {
    return describeFlags(flags, kErrorFlags, std::size(kErrorFlags));
}

std::wstring describeValidationFlags(int flags) {
    return describeFlags(flags, kValidationFlags, std::size(kValidationFlags));
}

// ---- diff -----------------------------------------------------------------

namespace {

std::wstring joinFields(const std::vector<std::wstring>& values,
                        const std::vector<size_t>& indexes) {
    std::wstring key;
    for (size_t i : indexes) {
        if (i < values.size()) key += values[i];
        key.push_back(L'\x1f');   // never appears in MSI text values
    }
    return key;
}

size_t columnIndex(const std::vector<std::wstring>& columns, const std::wstring& name) {
    for (size_t i = 0; i < columns.size(); ++i)
        if (util::iequals(columns[i], name)) return i;
    return static_cast<size_t>(-1);
}

// Adds one table's differences to the report.
void diffTable(msi::Db& base, msi::Db& updated, const std::wstring& table, bool hashStreams,
               std::vector<TableDiff>& out) {
    TableDiff d;
    d.table = table;

    const bool inBase = base.canOpenTable(table);
    const bool inUpdated = updated.canOpenTable(table);
    d.onlyBase = inBase && !inUpdated;
    d.onlyUpdated = !inBase && inUpdated;

    if (!inBase || !inUpdated) {
        msi::Db& only = inBase ? base : updated;
        const size_t n = only.rowCount(table);
        d.added = inBase ? 0 : n;
        d.removed = inBase ? n : 0;
        if (d.added == static_cast<size_t>(-1)) d.added = 0;
        if (d.removed == static_cast<size_t>(-1)) d.removed = 0;
        out.push_back(std::move(d));
        return;
    }

    msi::QueryResult colsBase = base.columnsOf(table);
    msi::QueryResult colsUpdated = updated.columnsOf(table);
    if (colsBase.columns != colsUpdated.columns || colsBase.types != colsUpdated.types)
        d.schemaChanged = true;

    // Row identity: the primary key. A keyless table falls back to the whole
    // row, which turns any change into one removal plus one addition.
    std::vector<std::wstring> keys = base.primaryKeys(table);
    std::vector<size_t> keyIndexes;
    for (const auto& k : keys) {
        size_t i = columnIndex(colsBase.columns, k);
        if (i != static_cast<size_t>(-1)) keyIndexes.push_back(i);
    }
    if (keyIndexes.empty()) {
        // The installer does not report a primary key for the hidden
        // _Streams / _Storages tables, but their key is the Name column:
        // without it a changed payload would show up as "removed + added".
        size_t name = columnIndex(colsBase.columns, L"Name");
        if (name != static_cast<size_t>(-1)) keyIndexes.push_back(name);
    }

    msi::QueryResult rowsBase = base.scanTable(table, hashStreams);
    msi::QueryResult rowsUpdated = updated.scanTable(table, hashStreams);

    // _Streams also carries the OLE property-set streams (SummaryInformation,
    // DigitalSignature, ...). They are not transform payload - the installer
    // never diffs them, and they change on every commit - so drop them.
    if (table == L"_Streams") {
        size_t nameCol = columnIndex(rowsBase.columns, L"Name");
        auto strip = [&](msi::QueryResult& rows) {
            std::vector<std::vector<std::wstring>> kept;
            for (auto& row : rows.rows) {
                if (nameCol < row.size() && !row[nameCol].empty() &&
                    row[nameCol][0] == 0x05)
                    continue;
                kept.push_back(std::move(row));
            }
            rows.rows.swap(kept);
        };
        strip(rowsBase);
        strip(rowsUpdated);
    }

    auto rowKey = [&](const std::vector<std::wstring>& row, size_t columns) {
        if (!keyIndexes.empty()) return joinFields(row, keyIndexes);
        std::vector<size_t> all;
        for (size_t i = 0; i < columns; ++i) all.push_back(i);
        return joinFields(row, all);
    };

    std::map<std::wstring, const std::vector<std::wstring>*> baseRows, updatedRows;
    for (const auto& row : rowsBase.rows)
        baseRows[rowKey(row, rowsBase.columns.size())] = &row;
    for (const auto& row : rowsUpdated.rows)
        updatedRows[rowKey(row, rowsUpdated.columns.size())] = &row;

    for (const auto& kv : updatedRows) {
        auto it = baseRows.find(kv.first);
        if (it == baseRows.end()) {
            ++d.added;
            continue;
        }
        if (*it->second != *kv.second) ++d.modified;
    }
    for (const auto& kv : baseRows)
        if (updatedRows.find(kv.first) == updatedRows.end()) ++d.removed;

    out.push_back(std::move(d));
}

}  // namespace

std::vector<TableDiff> diff(msi::Db& base, msi::Db& updated, bool hashStreams) {
    std::set<std::wstring> names;
    for (const auto& t : base.tableNames())
        if (base.isPersistent(t)) names.insert(t);
    for (const auto& t : updated.tableNames())
        if (updated.isPersistent(t)) names.insert(t);

    // _Streams / _Storages never show up in _Tables, but a transform does
    // carry changed payload streams (embedded cabinets, Binary rows ...), so
    // they have to be compared explicitly.
    for (const wchar_t* hidden : {L"_Streams", L"_Storages"}) {
        if (base.canOpenTable(hidden) || updated.canOpenTable(hidden)) names.insert(hidden);
    }

    std::vector<TableDiff> out;
    for (const auto& name : names) diffTable(base, updated, name, hashStreams, out);
    return out;
}

// ---- generate -------------------------------------------------------------

GenerateResult generate(const GenerateOptions& o) {
    if (o.baseMsi.empty() || o.updatedMsi.empty())
        util::fail(L"transform needs a base MSI and an updated MSI");
    if (!o.dryRun && o.outMst.empty())
        util::fail(L"transform needs an output .mst path");

    const std::wstring base = util::path::full(o.baseMsi);
    const std::wstring updated = util::path::full(o.updatedMsi);
    const std::wstring out = o.outMst.empty() ? std::wstring() : util::path::full(o.outMst);

    for (const auto& p : {base, updated}) {
        if (!util::path::exists(p)) util::fail(L"File not found: " + p);
        if (util::path::isDirectory(p)) util::fail(L"Path is a directory, not a package: " + p);
    }
    if (util::iequals(base, updated))
        util::fail(L"Base and updated package are the same file: " + base);

    GenerateResult result;
    result.outMst = out;

    if (!o.dryRun) {
        if (util::iequals(out, base) || util::iequals(out, updated))
            util::fail(L"Output transform must not overwrite an input package: " + out);
        if (util::iequals(util::path::ext(out), L".msi"))
            util::warn(L"output '" + out +
                       L"' uses the .msi extension; a transform is normally .mst");
        if (util::path::exists(out)) {
            if (!o.force) util::fail(L"Output transform already exists (use --force): " + out);
            if (!DeleteFileW(out.c_str()))
                util::fail(L"Cannot replace " + out + L": " + util::winError(GetLastError()));
        }
        std::wstring outDir = util::path::dirName(out);
        if (!outDir.empty() && !util::path::isDirectory(outDir)) util::path::makeDirs(outDir);
    }

    // Both packages are validated by Db (OLE signature, MsiOpenDatabase, _Tables).
    msi::Db baseDb(base);
    msi::Db updatedDb(updated);
    if (baseDb.isPatch() || updatedDb.isPatch())
        util::fail(L"Transforms are generated between two .msi databases, not .msp "
                   L"patch packages");

    // A transform is meant to be applied to its base package; a different
    // ProductCode means applying it turns one product into another.
    const std::wstring baseCode = packageProperty(baseDb, L"ProductCode");
    const std::wstring updatedCode = packageProperty(updatedDb, L"ProductCode");
    if (!baseCode.empty() && !updatedCode.empty() && !util::iequals(baseCode, updatedCode))
        util::warn(L"ProductCode differs (base " + baseCode + L", updated " + updatedCode +
                   L"); applying this transform changes the product");
    const std::wstring baseVersion = packageProperty(baseDb, L"ProductVersion");
    const std::wstring updatedVersion = packageProperty(updatedDb, L"ProductVersion");
    if (o.verbose)
        util::verboseLine(true, L"base    : " + base + L" (" + baseVersion + L")");
    if (o.verbose)
        util::verboseLine(true, L"updated : " + updated + L" (" + updatedVersion + L")");

    result.tables = diff(baseDb, updatedDb, true);
    size_t changedTables = 0, changeRows = 0;
    for (const auto& t : result.tables) {
        if (t.changes() == 0 && !t.schemaChanged) continue;
        ++changedTables;
        changeRows += t.changes();
    }
    util::verboseLine(o.verbose,
                      util::fmt(L"diff       : %llu of %llu table(s) differ, %llu change(s)",
                                static_cast<unsigned long long>(changedTables),
                                static_cast<unsigned long long>(result.tables.size()),
                                static_cast<unsigned long long>(changeRows)));
    if (o.dryRun) {
        // Report only: nothing is written, no --force needed.
        return result;
    }

    // Preferred path: two read-only handles. Some installer builds reject
    // that for GenerateTransform, so fall back to transaction copies.
    MSIHANDLE hBase = baseDb.handle();
    MSIHANDLE hUpdated = updatedDb.handle();
    UINT rc = MsiDatabaseGenerateTransformW(hUpdated, hBase, out.c_str(), 0, 0);

    std::unique_ptr<util::TempDir>  copies;
    msi::Handle                     baseCopy, updatedCopy;
    if (rc != ERROR_SUCCESS && rc != ERROR_NO_DATA) {
        util::verboseLine(o.verbose, L"read-only generate returned " + util::msiError(rc) +
                                         L"; retrying with transaction copies");
        copies.reset(new util::TempDir(L"msitool-mst-"));
        const std::wstring b2 = util::path::join(copies->path(), L"base.msi");
        const std::wstring u2 = util::path::join(copies->path(), L"updated.msi");
        if (!util::path::copyFile(base, b2) || !util::path::copyFile(updated, u2))
            util::fail(L"Cannot stage the packages for transform generation in " +
                       copies->path());
        UINT rcb = MsiOpenDatabaseW(b2.c_str(), MSIDBOPEN_TRANSACT, baseCopy.out());
        UINT rcu = MsiOpenDatabaseW(u2.c_str(), MSIDBOPEN_TRANSACT, updatedCopy.out());
        if (rcb != ERROR_SUCCESS || rcu != ERROR_SUCCESS || !baseCopy || !updatedCopy)
            util::fail(L"Cannot open the staged packages: " +
                       util::msiError(rcb != ERROR_SUCCESS ? rcb : rcu));
        hBase = baseCopy.get();
        hUpdated = updatedCopy.get();
        rc = MsiDatabaseGenerateTransformW(hUpdated, hBase, out.c_str(), 0, 0);
    }
    if (rc == ERROR_NO_DATA) {
        // The installer reports ERROR_NO_DATA when the two databases have no
        // differences at all; no (pointless) empty file is created.
        result.identical = true;
        result.written = false;
        return result;
    }
    if (rc != ERROR_SUCCESS) {
        // Never leave a broken .mst behind (the first attempt can create one
        // before failing).
        if (!out.empty()) DeleteFileW(out.c_str());
        util::fail(L"MsiDatabaseGenerateTransformW failed: " + util::msiError(rc));
    }

    result.size = fileSize(out);
    if (result.size == 0)
        util::fail(L"Transform file was not created: " + out);

    if (o.summaryInfo) {
        UINT sr = MsiCreateTransformSummaryInfoW(hUpdated, hBase, out.c_str(), o.flags.errors,
                                                 o.flags.validation);
        if (sr != ERROR_SUCCESS) {
            util::warn(L"MsiCreateTransformSummaryInfoW failed (" + util::msiError(sr) +
                       L"); the transform itself is valid but carries no summary stream");
        }
    }

    result.written = true;

    util::verboseLine(o.verbose, L"errors    : " + describeErrorFlags(o.flags.errors));
    util::verboseLine(o.verbose, L"validation: " + describeValidationFlags(o.flags.validation));
    return result;
}

// ---- apply ----------------------------------------------------------------

ApplyResult apply(const ApplyOptions& o) {
    if (o.transforms.empty())
        util::fail(L"apply needs at least one transform (.mst)");
    if (o.baseMsi.empty() || o.outMsi.empty())
        util::fail(L"apply needs a base MSI and an output MSI path");

    const std::wstring base = util::path::full(o.baseMsi);
    const std::wstring out = util::path::full(o.outMsi);
    if (!util::path::exists(base)) util::fail(L"File not found: " + base);
    if (util::path::isDirectory(base)) util::fail(L"Path is a directory, not a package: " + base);
    if (util::iequals(util::path::ext(out), L".mst"))
        util::warn(L"output '" + out + L"' uses the .mst extension; apply writes a package (.msi)");

    std::vector<std::wstring> transforms;
    for (const auto& t : o.transforms) {
        std::wstring full = util::path::full(t);
        if (!util::path::exists(full)) util::fail(L"Transform not found: " + full);
        transforms.push_back(full);
    }

    // A copied Authenticode signature is invalid as soon as a row changes and
    // a transform cannot remove the signature streams, so warn up front. This
    // must happen before the write handle opens the file.
    bool signedBase = false;
    try {
        msi::Db probe(base);
        for (const auto& name : probe.streamNames()) {
            if (!name.empty() && name[0] == 0x05 &&
                name.find(L"DigitalSignature") != std::wstring::npos) {
                signedBase = true;
                break;
            }
        }
    } catch (const util::Error&) {
        // Not fatal: the copy/open below reports the real problem.
    }

    const bool inPlace = util::iequals(base, out);
    if (inPlace) {
        if (!o.force)
            util::fail(L"Applying in place modifies " + base +
                       L"; pass --force (or write to a different output file)");
    } else {
        if (util::path::exists(out)) {
            if (!o.force) util::fail(L"Output package already exists (use --force): " + out);
            if (!DeleteFileW(out.c_str()))
                util::fail(L"Cannot replace " + out + L": " + util::winError(GetLastError()));
        }
        std::wstring outDir = util::path::dirName(out);
        if (!outDir.empty() && !util::path::isDirectory(outDir)) util::path::makeDirs(outDir);
        if (!util::path::copyFile(base, out))
            util::fail(L"Cannot copy " + base + L" to " + out);
    }

    // MSIDBOPEN_TRANSACT: changes stay in memory until MsiDatabaseCommit,
    // so a failing transform leaves the output package untouched.
    msi::Handle db;
    UINT rc = MsiOpenDatabaseW(out.c_str(), MSIDBOPEN_TRANSACT, db.out());
    if (rc != ERROR_SUCCESS || !db) {
        if (!inPlace) DeleteFileW(out.c_str());
        util::fail(L"Cannot open " + out + L" for writing: " + util::msiError(rc));
    }

    if (signedBase)
        util::warn(L"the base package is Authenticode-signed; the copied signature "
                   L"streams become invalid after the change - re-sign " + out +
                   L" when you need a valid signature");

    ApplyResult result;
    result.outMsi = out;
    const int conditions = o.flags.errors | o.flags.validation;

    if (conditions & MSITRANSFORM_ERROR_VIEWTRANSFORM)
        util::warn(L"MSITRANSFORM_ERROR_VIEWTRANSFORM only fills a temporary "
                   L"_TransformView table; the package contents stay unchanged");
    for (const auto& t : transforms) {
        rc = MsiDatabaseApplyTransformW(db.get(), t.c_str(), conditions);
        if (rc != ERROR_SUCCESS) {
            db.reset();
            if (!inPlace) DeleteFileW(out.c_str());
            util::fail(L"MsiDatabaseApplyTransformW failed for " + t + L": " +
                       util::msiError(rc));
        }
        ++result.applied;
        util::verboseLine(o.verbose, L"applied transform " + t);
    }

    rc = MsiDatabaseCommit(db.get());
    if (rc != ERROR_SUCCESS) {
        db.reset();
        if (!inPlace) DeleteFileW(out.c_str());
        util::fail(L"MsiDatabaseCommit failed for " + out + L": " + util::msiError(rc));
    }
    db.reset();

    if (o.verifyResult) {
        UINT vr = MsiVerifyPackageW(out.c_str());
        result.packageKind = vr == ERROR_SUCCESS ? L"installable package"
                                                 : L"verify: " + util::msiError(vr);
    }
    return result;
}

// ---- view a transform against a base package ------------------------------

std::vector<ChangeRow> viewTransform(const std::wstring& baseMsi,
                                     const std::wstring& transform) {
    const std::wstring base = util::path::full(baseMsi);
    const std::wstring file = util::path::full(transform);
    if (!util::path::exists(base)) util::fail(L"File not found: " + base);
    if (!util::path::exists(file)) util::fail(L"Transform not found: " + file);

    // The base package is opened read-only: MSITRANSFORM_ERROR_VIEWTRANSFORM
    // does not apply the changes, it only fills a temporary _TransformView
    // table (the mechanism Orca uses for "View Transform").
    msi::Db db(base);
    if (db.isPatch())
        util::fail(L"A transform is viewed against an .msi database, not a patch package: " +
                   base);

    UINT rc = MsiDatabaseApplyTransformW(db.handle(), file.c_str(),
                                        MSITRANSFORM_ERROR_VIEWTRANSFORM);
    if (rc != ERROR_SUCCESS)
        util::fail(L"Cannot view " + file + L" against " + base + L": " + util::msiError(rc));

    std::vector<ChangeRow> rows;
    msi::QueryResult view;
    try {
        view = db.query(L"SELECT * FROM `_TransformView`");
    } catch (const util::Error&) {
        // An empty transform may not create the table at all.
        return rows;
    }
    auto indexOf = [&](const wchar_t* name, size_t fallback) {
        for (size_t i = 0; i < view.columns.size(); ++i)
            if (util::iequals(view.columns[i], name)) return i;
        return fallback;
    };
    const size_t cTable = indexOf(L"Table", 0);
    const size_t cColumn = indexOf(L"Column", 1);
    const size_t cRow = indexOf(L"Row", 2);
    const size_t cData = indexOf(L"Data", 3);
    const size_t cCurrent = indexOf(L"Current", 4);
    auto cell = [&](const std::vector<std::wstring>& row, size_t index) {
        return index < row.size() ? row[index] : std::wstring();
    };
    for (const auto& row : view.rows) {
        ChangeRow c;
        c.table = cell(row, cTable);
        c.column = cell(row, cColumn);
        c.row = cell(row, cRow);
        c.data = cell(row, cData);
        c.current = cell(row, cCurrent);
        rows.push_back(std::move(c));
    }
    return rows;
}

// ---- inspect --------------------------------------------------------------

TransformInfo inspect(const std::wstring& path) {
    TransformInfo info;
    info.path = util::path::full(path);
    if (!util::path::exists(info.path)) util::fail(L"File not found: " + info.path);
    msi::FileProbe probe = msi::probe(info.path);
    info.size = probe.size;
    info.oleOk = probe.signatureOk;
    if (!info.oleOk)
        util::fail(L"Not a Windows Installer transform (missing the OLE compound-file "
                   L"signature): " + info.path);

    const std::wstring ext = util::path::ext(info.path);
    if (util::iequals(ext, L".msi") || util::iequals(ext, L".msp"))
        util::warn(info.path + L" looks like a package, not a transform; "
                   L"'mst' is meant for .mst files (use 'info' for packages)");

    bool storageOk = false;
    info.streams = listStreams(info.path, storageOk);
    if (!storageOk)
        util::warn(L"cannot enumerate the OLE storage of " + info.path +
                   L" (the file may be locked by another process)");

    info.summary = readSummary(info.path);
    info.hasSummary = !info.summary.empty();

    // MsiCreateTransformSummaryInfoW packs both bit sets into one value:
    // low word = MSITRANSFORM_ERROR_*, high word = MSITRANSFORM_VALIDATE_*
    // (stored in the "Character count" slot, PID 16).
    for (const auto& item : info.summary) {
        if (item.id == 16) {
            unsigned long long packed = wcstoull(item.value.c_str(), nullptr, 10);
            info.errorConditions = static_cast<int>(packed & 0xFFFFull);
            info.validation = static_cast<int>((packed >> 16) & 0xFFFFull);
        } else if (item.id == 9) {
            info.targetProduct = item.value;   // base ProductCode + version
        }
    }
    return info;
}

}  // namespace mst
