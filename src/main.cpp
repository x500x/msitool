#include "util.h"
#include "msidb.h"
#include "extract.h"
#include "pack.h"

#include <stdlib.h>
#include <cstdlib>
#include <cwctype>
#include <utility>

namespace {

struct Cli {
    bool                      help = false;
    bool                      tsv = false;
    bool                      verbose = false;
    bool                      flat = false;
    bool                      dryRun = false;
    bool                      skipExisting = false;
    bool                      keepTemp = false;
    bool                      sourceLayout = false;
    bool                      force = false;
    std::wstring              productName;
    std::wstring              version = L"1.0.0";
    std::wstring              manufacturer = L"msitool";
    std::wstring              productCode;
    std::wstring              upgradeCode;
    std::wstring              platform = L"Intel";
    int                       language = 1033;
    std::wstring              command;
    std::vector<std::wstring> args;   // positional arguments after the command
};

const wchar_t* kUsage = LR"(
msitool - view and unpack Windows Installer (.msi/.msp) packages
         built with the Windows Installer (Msi) API + FDI (cabinet.dll)

Usage:
  msitool <command> <package.msi> [arguments...] [options...]

Commands:
  verify                   Check that a file really is an MSI/MSP package
                           (signature, open, _Tables, summary, CAB media)
  info                     SummaryInformation, key properties, CAB/media list
  tables                   List every table with its row count
  schema <table>           Show columns, MSI types and primary keys
  query <SQL>              Run an arbitrary (read-only) MSI SQL query
  list <name>              Dump a table. Common aliases:
                           files components features dirs media properties
                           streams cabs shortcuts registry services
                           customactions upgrade binaries
                           (any real table name is accepted too)
  cabs <outdir>            Export embedded/external CABs + manifest.txt
  export <outdir>          Export all tables as .idt, all raw streams,
                           Binary entries and summary.txt
  extract <outdir>         Unpack: rebuild the directory tree and decompress
                           every file from the package cabinets
  pack <srcdir> <out.msi>  Pack a directory into a self-contained MSI with
                           one embedded MSZIP cabinet

Options:
  --tsv                    Emit tables as tab-separated values
  -v, --verbose            Verbose progress on stderr
  --flat                   extract: flatten all files into outdir
  --dry-run                extract: show the plan, write nothing
  --skip-existing          extract: do not overwrite existing files
  --source-layout          extract: use the source-image directory tree
                           instead of the installed (target) tree
  --keep-temp              extract: keep staged temporary cabinets
  --name <text>            pack: ProductName (default: directory name)
  --version <x.y.z>        pack: ProductVersion (default 1.0.0)
  --manufacturer <text>    pack: Manufacturer (default: msitool)
  --product-code <GUID>    pack: fixed ProductCode (default: random GUID)
  --upgrade-code <GUID>    pack: fixed UpgradeCode (default: random GUID)
  --platform <x64|Intel>   pack: platform (default: Intel)
  --language <LCID>        pack: ProductLanguage (default: 1033)
  --force                  pack: overwrite an existing output MSI
  -h, --help               Show this help

Examples:
  msitool verify setup.msi
  msitool info setup.msi
  msitool tables setup.msi
  msitool query setup.msi "SELECT `File`,`FileName`,`FileSize` FROM `File`"
  msitool list setup.msi files --tsv
  msitool cabs setup.msi C:\out\cabs
  msitool export setup.msi C:\out\archive
  msitool extract setup.msi C:\out\files --verbose
  msitool pack C:\payload C:\out\app.msi --name "My App" --version 1.2.3

Notes:
  * Every command preflights the package first (OLE compound-file signature,
    MsiOpenDatabase, _Tables). 'verify' prints the full validation report.
  * Output is UTF-8. Redirect with e.g. `msitool list ... > files.tsv`.
  * Embedded cabinets are read from the _Streams table; external cabinets are
    looked up next to the MSI. Uncompressed ("layout") packages are copied
    from their source tree next to the MSI.
  * Extraction never installs anything and never needs administrator rights.
  * .msp patch packages can be inspected (info/tables/query/export/cabs), but
    their payload is a transform, not an MSI directory tree - 'extract' is
    intended for .msi packages.
)";

Cli parseCommandLine(int argc, wchar_t** argv) {
    Cli cli;
    std::vector<std::wstring> pos;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i] ? argv[i] : L"";
        if (a == L"-h" || a == L"--help" || a == L"/?" || util::iequals(a, L"help") && pos.empty()) {
            cli.help = true;
            continue;
        }
        if (a == L"-v" || a == L"--verbose") { cli.verbose = true; continue; }
        if (a == L"--tsv") { cli.tsv = true; continue; }
        if (a == L"--flat") { cli.flat = true; continue; }
        if (a == L"--dry-run") { cli.dryRun = true; continue; }
        if (a == L"--skip-existing") { cli.skipExisting = true; continue; }
        if (a == L"--source-layout") { cli.sourceLayout = true; continue; }
        if (a == L"--keep-temp") { cli.keepTemp = true; continue; }
        if (a == L"--force") { cli.force = true; continue; }
        if (a == L"--name" || a == L"--version" || a == L"--manufacturer" ||
            a == L"--product-code" || a == L"--upgrade-code" ||
            a == L"--platform" || a == L"--language") {
            if (i + 1 >= argc) util::fail(L"Option " + a + L" requires a value");
            std::wstring v = argv[++i] ? argv[i] : L"";
            if (a == L"--name") cli.productName = v;
            else if (a == L"--version") cli.version = v;
            else if (a == L"--manufacturer") cli.manufacturer = v;
            else if (a == L"--product-code") cli.productCode = v;
            else if (a == L"--upgrade-code") cli.upgradeCode = v;
            else if (a == L"--platform") cli.platform = v;
            else cli.language = _wtoi(v.c_str());
            continue;
        }
        if (util::startsWith(a, L"-"))
            util::fail(L"Unknown option (quote SQL that starts with '-'): " + a);
        pos.push_back(a);
    }
    if (pos.empty()) cli.help = true;
    if (!pos.empty()) {
        cli.command = pos[0];
        cli.args.assign(pos.begin() + 1, pos.end());
    }
    return cli;
}

std::wstring requireArg(const Cli& cli, size_t index, const wchar_t* what) {
    if (cli.args.size() <= index)
        util::fail(std::wstring(L"Missing ") + what + L". Run 'msitool --help' for usage.");
    return cli.args[index];
}

std::wstring humanSize(unsigned long long bytes) {
    if (bytes < 1024) return std::to_wstring(bytes) + L" B";
    if (bytes < 1024 * 1024)
        return util::fmt(L"%.1f KiB", static_cast<double>(bytes) / 1024.0);
    return util::fmt(L"%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
}

unsigned long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// ---- commands --------------------------------------------------------------

void cmdInfo(msi::Db& db, const Cli& cli) {
    util::outLine(L"Package : " + db.path());
    util::outLine(L"Size    : " + humanSize(fileSize(db.path())));
    util::outLine(L"");

    util::outLine(L"=== SummaryInformation ===");
    util::Table t;
    t.headers = {L"Property", L"Value"};
    for (const auto& item : db.summary()) t.rows.push_back({item.name, item.value});
    util::printTable(t, cli.tsv);

    util::outLine(L"");
    util::outLine(L"=== Key properties ===");
    static const wchar_t* kKeys[] = {L"ProductName", L"ProductVersion", L"ProductCode",
                                     L"UpgradeCode", L"Manufacturer", L"ProductLanguage",
                                     L"ALLUSERS", L"INSTALLLEVEL", L"Platform",
                                     L"WindowsBuild", L"DefaultUIFont"};
    util::Table p;
    p.headers = {L"Property", L"Value"};
    for (const wchar_t* k : kKeys) {
        std::wstring v = db.property(k);
        if (!v.empty()) p.rows.push_back({k, v});
    }
    util::printTable(p, cli.tsv);
    util::outLine(L"(full list: msitool list " + db.path() + L" properties)");

    util::outLine(L"");
    util::outLine(L"=== Media / cabinets ===");
    std::vector<extract::MediaInfo> media = extract::readMedia(db);
    util::Table m;
    m.headers = {L"DiskId", L"LastSequence", L"Cabinet", L"Type", L"Size", L"Status"};
    for (const auto& x : media) {
        std::wstring type = x.cabinet.empty() ? L"uncompressed"
                                              : (x.embedded ? L"embedded (#stream)" : L"external");
        std::wstring status = x.cabinet.empty() ? L"layout files"
                              : x.available     ? L"OK"
                                                : L"MISSING";
        m.rows.push_back({std::to_wstring(x.diskId), std::to_wstring(x.lastSequence),
                          x.cabinet.empty() ? L"(none)" : x.cabinet, type,
                          std::to_wstring(x.size), status});
    }
    if (media.empty())
        util::outLine(util::iequals(util::path::ext(db.path()), L".msp")
                          ? L"(patch package: use 'export' to dump its streams/transforms)"
                          : L"(no Media table - uncompressed layout)");
    else util::printTable(m, cli.tsv);
}

void cmdTables(msi::Db& db, const Cli& cli) {
    util::Table t;
    t.headers = {L"Table", L"Persistent", L"Rows"};
    for (const auto& name : db.tableNames()) {
        size_t n = db.rowCount(name);
        std::wstring rows = n == static_cast<size_t>(-1) ? L"?" : std::to_wstring(n);
        t.rows.push_back({name, db.isPersistent(name) ? L"yes" : L"no", rows});
    }
    util::printTable(t, cli.tsv);
    util::outLine(util::fmt(L"%llu table(s) in %s",
                            static_cast<unsigned long long>(t.rows.size()),
                            db.path().c_str()));
}

void cmdSchema(msi::Db& db, const Cli& cli) {
    std::wstring wanted = requireArg(cli, 1, L"table name");
    std::wstring table = db.resolveTable(wanted);
    if (table.empty()) util::fail(L"Table not found: " + wanted);

    msi::QueryResult meta = db.columnsOf(table);
    std::vector<std::wstring> keys = db.primaryKeys(table);
    util::outLine(L"Table      : " + table);
    util::outLine(L"Persistent : " + std::wstring(db.isPersistent(table) ? L"yes" : L"no"));
    {
        std::wstring k;
        for (const auto& x : keys) k += (k.empty() ? L"" : L", ") + x;
        util::outLine(L"Primary key: " + (k.empty() ? L"(none)" : k));
    }
    util::outLine(L"");

    util::Table t;
    t.headers = {L"#", L"Column", L"Type", L"Kind", L"Nullable", L"Key"};
    for (size_t i = 0; i < meta.columns.size(); ++i) {
        std::wstring type = i < meta.types.size() ? meta.types[i] : L"";
        wchar_t c = type.empty() ? L'?' : type[0];
        std::wstring kind;
        switch (towlower(static_cast<wint_t>(c))) {
            case L's': kind = L"string"; break;
            case L'i': kind = L"integer"; break;
            case L'v': kind = L"binary stream"; break;
            case L'l': kind = L"localizable string"; break;
            case L'g': kind = L"GUID"; break;
            default:   kind = L"other"; break;
        }
        bool nullable = type.empty() || iswupper(static_cast<wint_t>(c)) != 0;
        bool key = false;
        for (const auto& k : keys) if (util::iequals(k, meta.columns[i])) key = true;
        t.rows.push_back({std::to_wstring(i + 1), meta.columns[i], type, kind,
                          nullable ? L"yes" : L"no", key ? L"*" : L""});
    }
    util::printTable(t, cli.tsv);
}

void cmdQuery(msi::Db& db, const Cli& cli) {
    std::wstring sql = requireArg(cli, 1, L"SQL query");
    msi::QueryResult r = db.query(sql);
    if (r.columns.empty()) {
        util::outLine(L"Query executed, no result columns.");
    } else {
        util::Table t;
        t.headers = r.columns;
        t.rows = r.rows;
        util::printTable(t, cli.tsv);
    }
    util::outLine(util::fmt(L"%llu row(s).", static_cast<unsigned long long>(r.rows.size())));
}

void cmdList(msi::Db& db, const Cli& cli) {
    std::wstring what = requireArg(cli, 1, L"list name / table name");
    static const std::vector<std::pair<std::wstring, std::wstring>> aliases = {
        {L"files", L"File"},
        {L"components", L"Component"},
        {L"features", L"Feature"},
        {L"dirs", L"Directory"},
        {L"directories", L"Directory"},
        {L"media", L"Media"},
        {L"properties", L"Property"},
        {L"shortcuts", L"Shortcut"},
        {L"registry", L"Registry"},
        {L"services", L"ServiceInstall"},
        {L"customactions", L"CustomAction"},
        {L"upgrade", L"Upgrade"},
        {L"launchconditions", L"LaunchCondition"},
        {L"featurecomponents", L"FeatureComponents"},
        {L"binaries", L"Binary"},
        {L"assemblies", L"MsiAssembly"},
        {L"tables", L"_Tables"},
    };

    if (util::iequals(what, L"streams")) {
        msi::QueryResult r = db.query(L"SELECT `Name`, `Data` FROM `_Streams` ORDER BY `Name`");
        util::Table t;
        t.headers = {L"Name", L"Data"};
        t.rows = r.rows;
        util::printTable(t, cli.tsv);
        util::outLine(util::fmt(L"%llu stream(s).",
                                static_cast<unsigned long long>(r.rows.size())));
        return;
    }
    if (util::iequals(what, L"cabs")) {
        std::vector<extract::MediaInfo> media = extract::readMedia(db);
        util::Table t;
        t.headers = {L"DiskId", L"LastSequence", L"Cabinet", L"Type", L"Size",
                      L"Source / stream"};
        for (const auto& x : media) {
            t.rows.push_back({std::to_wstring(x.diskId), std::to_wstring(x.lastSequence),
                              x.cabinet.empty() ? L"(none)" : x.cabinet,
                              x.embedded ? L"embedded" : L"external",
                              std::to_wstring(x.size),
                              x.embedded ? x.streamName : x.sourcePath});
        }
        util::printTable(t, cli.tsv);
        return;
    }

    std::wstring table = what;
    for (const auto& a : aliases)
        if (util::iequals(what, a.first)) { table = a.second; break; }
    table = db.resolveTable(table);
    if (table.empty()) util::fail(L"Unknown list name / table: " + what +
                                  L" (try: files components features dirs media properties "
                                  L"streams cabs)");

    msi::QueryResult r = db.query(L"SELECT * FROM " + msi::quoteIdent(table));
    util::Table t;
    t.headers = r.columns;
    t.rows = r.rows;
    util::printTable(t, cli.tsv);
    util::outLine(util::fmt(L"%llu row(s) in %s.",
                            static_cast<unsigned long long>(r.rows.size()), table.c_str()));
}

void cmdExtract(msi::Db& db, const Cli& cli) {
    extract::Options opt;
    opt.outDir = requireArg(cli, 1, L"output directory");
    opt.flat = cli.flat;
    opt.dryRun = cli.dryRun;
    opt.skipExisting = cli.skipExisting;
    opt.sourceLayout = cli.sourceLayout;
    opt.keepTemp = cli.keepTemp;
    opt.verbose = cli.verbose;
    extract::extractPackage(db, opt);
}

// Stands alone (it must be able to report a bad file instead of throwing
// before any diagnostics are printed). Returns false for an invalid package.
bool cmdVerify(const Cli& cli) {
    std::wstring package = requireArg(cli, 0, L"package path");
    msi::FileProbe p = msi::probe(package);

    util::Table t;
    t.headers = {L"Check", L"Result"};
    auto add = [&](const wchar_t* name, std::wstring value) {
        t.rows.push_back({name, std::move(value)});
    };
    add(L"Path", p.path);
    add(L"Size", p.size ? humanSize(p.size) : L"-");
    add(L"OLE compound signature",
        p.signatureOk ? L"OK  (D0 CF 11 E0 A1 B1 1A E1)"
                      : L"FAIL  (got " + p.signatureHex + L")");
    add(L"Package kind", p.isPatch ? L".msp patch (patch open mode)"
                                   : L".msi database (read-only)");

    bool ok = p.error.empty();
    std::wstring reason = p.error;
    if (!ok) {
        add(L"Preflight", L"FAIL - " + p.error);
        add(L"Result", L"INVALID: not a Windows Installer package");
    } else {
        add(L"Preflight", L"OK");
        try {
            msi::Db db(p.path);
            add(L"MsiOpenDatabase", L"OK");
            if (!p.isPatch) {
                UINT vr = MsiVerifyPackageW(p.path.c_str());
                add(L"MsiVerifyPackage",
                    vr == ERROR_SUCCESS ? L"OK (installable package)"
                                        : L"reported " + util::msiError(vr));
            }
            add(L"_Tables", L"OK - " + std::to_wstring(db.tableNames().size()) + L" table(s)");
            add(L"_Streams", std::to_wstring(db.streamNames().size()) + L" stream(s)");

            std::vector<extract::MediaInfo> media = extract::readMedia(db);
            size_t cabs = 0;
            for (const auto& m : media)
                if (!m.cabinet.empty()) ++cabs;
            add(L"Media / cabinets",
                std::to_wstring(media.size()) + L" media row(s), " +
                    std::to_wstring(cabs) + L" cabinet(s)");

            std::wstring title, templ;
            for (const auto& s : db.summary()) {
                if (s.name == L"Title") title = s.value;
                if (util::startsWith(s.name, L"Template")) templ = s.value;
            }
            add(L"SummaryInformation", L"OK - Title=\"" + title + L"\", Template=\"" +
                                           templ + L"\"");
            add(L"Result", L"VALID Windows Installer package");
        } catch (const util::Error& e) {
            reason = e.message();
            add(L"MsiOpenDatabase / structure", L"FAIL - " + reason);
            add(L"Result", L"INVALID: cannot open as a Windows Installer database");
            ok = false;
        }
    }
    util::printTable(t, cli.tsv);
    // The table truncates long cells; always print the full reason as well.
    if (!ok && !reason.empty()) util::outLine(L"Reason: " + reason);
    return ok;
}

void cmdPack(const Cli& cli) {
    pack::Options opt;
    opt.inputDir = requireArg(cli, 0, L"input directory");
    opt.outputMsi = requireArg(cli, 1, L"output .msi path");
    opt.productName = cli.productName;
    opt.version = cli.version;
    opt.manufacturer = cli.manufacturer;
    opt.productCode = cli.productCode;
    opt.upgradeCode = cli.upgradeCode;
    opt.platform = cli.platform;
    opt.language = cli.language;
    opt.force = cli.force;
    opt.dryRun = cli.dryRun;
    opt.verbose = cli.verbose;
    pack::pack(opt);
}

int run(int argc, wchar_t** argv) {
    Cli cli = parseCommandLine(argc, argv);
    if (cli.help || cli.command.empty()) {
        util::out(kUsage);
        return 0;
    }

    std::wstring cmd = util::toLower(cli.command);
    // verify and pack must run before Db (which validates an input MSI).
    if (cmd == L"verify") return cmdVerify(cli) ? 0 : 1;
    if (cmd == L"pack") { cmdPack(cli); return 0; }

    std::wstring package = requireArg(cli, 0, L"package path (.msi)");
    msi::Db db(package);  // preflight + MsiOpenDatabase + _Tables check

    if (cmd == L"info")             cmdInfo(db, cli);
    else if (cmd == L"tables")      cmdTables(db, cli);
    else if (cmd == L"schema")      cmdSchema(db, cli);
    else if (cmd == L"query")       cmdQuery(db, cli);
    else if (cmd == L"list")        cmdList(db, cli);
    else if (cmd == L"extract")     cmdExtract(db, cli);
    else if (cmd == L"cabs") {
        extract::exportCabs(db, requireArg(cli, 1, L"output directory"), cli.verbose);
    } else if (cmd == L"export") {
        extract::exportArchive(db, requireArg(cli, 1, L"output directory"), cli.verbose);
    } else {
        util::fail(L"Unknown command '" + cli.command + L"'. Run 'msitool --help'.");
    }
    return 0;
}

}  // namespace

int main() {
    util::setConsoleUtf8();
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        util::errLine(L"error: cannot parse the command line");
        return 2;
    }
    int rc = 0;
    try {
        rc = run(argc, argv);
    } catch (const util::Error& e) {
        util::errLine(L"error: " + e.message());
        rc = 1;
    } catch (const std::exception& e) {
        util::errLine(L"error: " + util::widen(e.what(), CP_ACP));
        rc = 1;
    } catch (...) {
        util::errLine(L"error: unexpected exception");
        rc = 1;
    }
    LocalFree(argv);
    fflush(stdout);
    fflush(stderr);
    return rc;
}
