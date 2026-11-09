#include "util.h"
#include "msidb.h"
#include "extract.h"
#include "pack.h"
#include "transform.h"
#include "effects.h"
#include "ui.h"

#include <stdio.h>
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
    bool                      desktopShortcut = false;
    bool                      startMenuShortcut = false;
    std::wstring              mainExe;
    std::wstring              productName;
    std::wstring              version = L"1.0.0";
    std::wstring              manufacturer = L"msitool";
    std::wstring              productCode;
    std::wstring              upgradeCode;
    std::wstring              platform = L"Intel";
    std::wstring              scope = L"perMachine";   // --per-user
    std::wstring              installDir;                // --install-dir
    int                       language = 1033;
    std::wstring              configFile;
    std::wstring              effectsFormat = L"all";   // --format for 'effects'
    std::wstring              uiTemplate;
    std::wstring              uiLocale;
    std::wstring              dumpUiDir;
    unsigned long long        cabSize = 0;
    std::wstring              largeFiles = L"layout";
    std::wstring              ui = L"none";
    std::wstring              signCert;
    std::wstring              signPassword;
    std::wstring              signTimestamp;
    std::wstring              signTool;
    bool                      keepEmptyDirs = true;
    bool                      uiSet = false;
    bool                      largeFilesSet = false;
    bool                      cabSizeSet = false;
    bool                      signCertSet = false;
    bool                      installDirSet = false;
    bool                      uiTemplateSet = false;
    bool                      productNameSet = false;
    bool                      versionSet = false;
    bool                      manufacturerSet = false;
    bool                      productCodeSet = false;
    bool                      upgradeCodeSet = false;
    bool                      platformSet = false;
    bool                      scopeSet = false;
    bool                      languageSet = false;
    int                       mstErrors = 0;             // --errors   (MSITRANSFORM_ERROR_*)
    int                       mstValidation = 0;         // --validate (MSITRANSFORM_VALIDATE_*)
    bool                      noSummary = false;         // --no-summary
    std::wstring              baseMsi;                    // extract .msp with a target MSI
    std::vector<std::wstring> transforms;               // --transform (repeatable)
    std::vector<std::wstring> files;                    // --files (repeatable)
    std::wstring              command;
    std::vector<std::wstring> args;   // positional arguments after the command
};

const wchar_t* kUsage = LR"(
msitool - view, unpack and build Windows Installer (.msi/.msp) packages
         built with the Windows Installer (Msi) API + FDI/FCI (cabinet.dll)

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
  effects <outdir>         Export install effects: registry (.reg), service
                           (.cmd), environment (.cmd), shortcut (.ps1) scripts
                           and a pack.xml manifest for --config (alias: fx)
  extract <outdir>         Unpack: rebuild the directory tree and decompress
                           every file from the package cabinets
  pack <srcdir> <out.msi>  Pack a directory into a self-contained MSI with
                           one or more embedded MSZIP cabinets
  transform <base.msi> <updated.msi> <out.mst>
                           Generate a Windows Installer transform (.mst) from
                           the differences between two packages
  apply <base.msi> <out.msi> <file.mst...>
                           Apply one or more transforms to a copy of a package
                           and commit the result to <out.msi>
  mst <file.mst>           Describe a transform: summary stream (validation /
                           error conditions) and its OLE streams. Add
                           --base-msi <setup.msi> to list the change log the
                           installer sees (_TransformView). Alias: mstinfo

Options:
  --tsv                    Emit tables as tab-separated values
  -v, --verbose            Verbose progress on stderr
  --flat                   extract: flatten all files into outdir
  --dry-run                extract: show the plan, write nothing;
                           transform: only diff the two packages (the output
                           .mst is optional, nothing is written)
  --skip-existing          extract: do not overwrite existing files
  --source-layout          extract: use the source-image directory tree
                           instead of the installed (target) tree
  --format <all|scripts|manifest>
                           effects: what to write (default: all)
  --keep-temp              extract: keep staged temporary cabinets
  --transform <file.mst>   apply an MST as a temporary view (repeatable);
                           info/tables/schema/query/list/extract see the
                           transformed database. 'apply' uses it as an input
                           transform (same as passing it positionally)
  --errors <flags>         transform/apply: installer error conditions to
                           suppress, comma separated: addexistingrow,
                           delmissingrow, addexistingtable, delmissingtable,
                           updatemissingrow, changecodepage, viewtransform
                           (or a numeric value such as 0x11)
  --validate <flags>       transform/apply: properties validated on apply:
                           language, product, platform, majorversion,
                           minorversion, updateversion, newlessbaseversion,
                           newlessequalbaseversion, newequalbaseversion,
                           newgreaterequalbaseversion, newgreaterbaseversion,
                           upgradecode (default: none)
  --no-summary             transform: skip MsiCreateTransformSummaryInfo (the
                           transform then carries no summary stream)
  --base-msi <file.msi>    extract .msp: apply the patch transforms to this
                           target MSI and lay out the payload with real names;
                           mst: view the transform's changes against this
                           package (read-only, never modified)
  --name <text>            pack: ProductName (default: directory name)
  --version <x.y.z>        pack: ProductVersion (default 1.0.0)
  --manufacturer <text>    pack: Manufacturer (default: msitool)
  --product-code <GUID>    pack: fixed ProductCode (default: random GUID)
  --upgrade-code <GUID>    pack: fixed UpgradeCode (default: random GUID)
  --platform <x64|Intel>   pack: platform (default: Intel)
  --per-user               pack: per-user install (no ALLUSERS, defaults to
                           LocalAppDataFolder\<ProductName>)
  --all-users              pack: per-machine install (default, ALLUSERS=1)
  --install-dir <path>     pack: where the payload is installed. "App" or
                           "Tools\App" is relative to Program Files,
                           "LocalAppDataFolder\App" starts from a standard
                           Windows Installer folder and "D:\Apps\App" is an
                           absolute path (pinned with the INSTALLFOLDER
                           property). Default: Program Files\<ProductName>
  --language <LCID>        pack: ProductLanguage (default: 1033)
  --cab-size <n>[M|G]      pack: split into CABs of ~n MiB (default: one CAB)
  --no-empty-dirs          pack: do not emit CreateFolder rows
  --large-files <mode>     pack: layout (default) or error for > 2 GiB files
  --ui <none|basic|minimal|wizard|custom>
                           pack: installer UI. wizard = built-in multi-page
                           wizard (welcome / license / folder / progress /
                           finish, or maintenance), minimal = the compact
                           legacy wizard, custom = dialogs from the XML
  --ui-template <file.msi> pack: use an existing package's UI tables as base
  --ui-locale <en|zh-CN>   pack: built-in wizard language (default: follows
                           --language; 1033 = en, 2052 = zh-CN)
  --desktop-shortcut       pack: create a desktop shortcut for the main .exe
  --start-menu-shortcut    pack: create a Start Menu shortcut
  --main-exe <relpath>     pack: program used by the generated shortcuts
                           (default: the only root .exe, or the largest .exe)
  --dump-ui <dir>          pack: write the embedded wizard XML / EULA / bitmaps
                           to <dir> and exit
  --config <file.xml>      pack: XML manifest (features, shortcuts, registry,
                           services, environment, UI, signing, defaults)
  --files <path[;path...]> pack: install only these entries (repeatable, ; or
                           , separated). Relative paths resolve against the
                           source directory; a directory adds its whole
                           subtree; each entry keeps its path relative to the
                           source directory in the installed layout
  --sign-cert <pfx>        pack: Authenticode-sign with signtool.exe
  --sign-password <text>   pack: PFX password
  --sign-timestamp <url>   pack: RFC 3161 timestamp URL
  --sign-tool <path>       pack: explicit path to signtool.exe
  --force                  pack: overwrite an existing output MSI
  -h, --help               Show this help

)"
LR"(
Examples:
  msitool verify setup.msi
  msitool info setup.msi
  msitool tables setup.msi
  msitool query setup.msi "SELECT `File`,`FileName`,`FileSize` FROM `File`"
  msitool list setup.msi files --tsv
  msitool cabs setup.msi C:\out\cabs
  msitool export setup.msi C:\out\archive
  msitool effects setup.msi C:\out\effects
  msitool effects setup.msi C:\out\effects --format manifest
  msitool extract setup.msi C:\out\files --verbose
  msitool extract setup.msi C:\out\files --transform custom.mst
  msitool extract update.msp C:\out\patch
  msitool extract update.msp C:\out\patch --base-msi setup.msi
  msitool pack C:\payload C:\out\app.msi --name "My App" --version 1.2.3
  msitool pack C:\payload C:\out\app.msi --install-dir "LocalAppDataFolder\My App"
  msitool pack C:\payload C:\out\app.msi --install-dir "D:\Apps\MyApp"
  msitool pack C:\payload C:\out\app.msi --config examples\pack.xml
  msitool pack C:\payload C:\out\app.msi --cab-size 16 --ui wizard
  msitool pack C:\payload C:\out\app.msi --ui custom --config custom-ui.xml
  msitool pack C:\payload C:\out\app.msi --ui-template C:\templates\ui.msi
  msitool pack C:\payload C:\out\app.msi --dump-ui C:\out\wizard-ui
  msitool pack C:\payload C:\out\app.msi --files "app.exe;cfg\app.ini"
  msitool pack C:\payload C:\out\app.msi --files "app.exe" --files tools
  msitool transform setup.msi setup2.msi custom.mst --validate product,language
  msitool apply setup.msi setup-custom.msi custom.mst --force
  msitool mst custom.mst

Notes:
  * Every command preflights the package first (OLE compound-file signature,
    MsiOpenDatabase, _Tables). 'verify' prints the full validation report.
  * Output is UTF-8. Redirect with e.g. `msitool list ... > files.tsv`.
  * Embedded cabinets are read from the _Streams table; external cabinets are
    looked up next to the MSI. Uncompressed ("layout") packages are copied
    from their source tree next to the MSI.
  * Extraction never installs anything and never needs administrator rights.
  * .msp patch packages can be inspected (info/tables/query/export/cabs) and
    'extract' dumps their embedded CAB payload, transform and raw streams.
    Use --base-msi <target.msi> to lay the patch payload out with real names.
  * --transform <file.mst> applies a transform as a temporary read-only view;
    it never modifies the package on disk. Repeatable.
  * 'transform' writes a real .mst (MsiDatabaseGenerateTransformW plus
    MsiCreateTransformSummaryInfoW) by diffing two .msi databases; 'apply'
    copies the base package, applies the .mst files on a transaction handle
    and commits (MsiDatabaseCommit), so the base package is never touched.
    'mst' inspects a .mst without touching any package.
  * --files narrows down what 'pack' installs: the source directory stays the
    package root, so a selection of files under subdir\ still installs into
    <root>\subdir. Without --files the complete tree is packed.
)";

// "16", "16M", "512K", "1G" (1024 based) -> bytes.
// (pack::parseSize is declared in pack.h.)

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
        if (a == L"--no-empty-dirs") { cli.keepEmptyDirs = false; continue; }
        if (a == L"--per-user") { cli.scope = L"perUser"; cli.scopeSet = true; continue; }
        if (a == L"--all-users") { cli.scope = L"perMachine"; cli.scopeSet = true; continue; }
        if (a == L"--desktop-shortcut") { cli.desktopShortcut = true; continue; }
        if (a == L"--start-menu-shortcut") { cli.startMenuShortcut = true; continue; }
        if (a == L"--transform" || a == L"--base-msi" || a == L"--files" ||
            a == L"--errors" || a == L"--validate") {
            if (i + 1 >= argc) util::fail(L"Option " + a + L" requires a value");
            std::wstring v = argv[++i] ? argv[i] : L"";
            if (a == L"--transform") cli.transforms.push_back(v);
            else if (a == L"--files") cli.files.push_back(v);
            else if (a == L"--errors") cli.mstErrors |= mst::parseErrorFlags(v);
            else if (a == L"--validate") cli.mstValidation |= mst::parseValidationFlags(v);
            else cli.baseMsi = v;
            continue;
        }
        if (a == L"--no-summary") { cli.noSummary = true; continue; }
        if (a == L"--name" || a == L"--version" || a == L"--manufacturer" ||
            a == L"--product-code" || a == L"--upgrade-code" ||
            a == L"--platform" || a == L"--language" || a == L"--install-dir" ||
            a == L"--config" || a == L"--format" ||
            a == L"--cab-size" || a == L"--large-files" ||
            a == L"--ui" || a == L"--ui-template" || a == L"--ui-locale" ||
            a == L"--main-exe" ||
            a == L"--dump-ui" || a == L"--sign-cert" || a == L"--sign-password" ||
            a == L"--sign-timestamp" || a == L"--sign-tool") {
            if (i + 1 >= argc) util::fail(L"Option " + a + L" requires a value");
            std::wstring v = argv[++i] ? argv[i] : L"";
            if (a == L"--name") { cli.productName = v; cli.productNameSet = true; }
            else if (a == L"--version") { cli.version = v; cli.versionSet = true; }
            else if (a == L"--manufacturer") { cli.manufacturer = v; cli.manufacturerSet = true; }
            else if (a == L"--product-code") { cli.productCode = v; cli.productCodeSet = true; }
            else if (a == L"--upgrade-code") { cli.upgradeCode = v; cli.upgradeCodeSet = true; }
            else if (a == L"--platform") { cli.platform = v; cli.platformSet = true; }
            else if (a == L"--install-dir") { cli.installDir = v; cli.installDirSet = true; }
            else if (a == L"--config") cli.configFile = v;
            else if (a == L"--format") cli.effectsFormat = v;
            else if (a == L"--ui-template") { cli.uiTemplate = v; cli.uiTemplateSet = true; }
            else if (a == L"--ui-locale") cli.uiLocale = v;
            else if (a == L"--main-exe") cli.mainExe = v;
            else if (a == L"--dump-ui") cli.dumpUiDir = v;
            else if (a == L"--cab-size") { cli.cabSize = pack::parseSize(v); cli.cabSizeSet = true; }
            else if (a == L"--large-files") { cli.largeFiles = v; cli.largeFilesSet = true; }
            else if (a == L"--ui") { cli.ui = v; cli.uiSet = true; }
            else if (a == L"--sign-cert") { cli.signCert = v; cli.signCertSet = true; }
            else if (a == L"--sign-password") cli.signPassword = v;
            else if (a == L"--sign-timestamp") cli.signTimestamp = v;
            else if (a == L"--sign-tool") cli.signTool = v;
            else { cli.language = _wtoi(v.c_str()); cli.languageSet = true; }
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
    opt.transforms = cli.transforms;
    opt.baseMsi = cli.baseMsi;
    if (db.isPatch()) extract::extractPatch(db, opt);
    else              extract::extractPackage(db, opt);
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
                for (const auto& tr : cli.transforms) {
                    db.applyTransform(tr);
                    add(L"Transform", L"applied " + tr);
                }
            }
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
    if (!cli.dumpUiDir.empty()) {
        opt.dumpUiDir = cli.dumpUiDir;
        pack::pack(opt);
        return;
    }
    opt.inputDir = requireArg(cli, 0, L"input directory");
    opt.inputFiles = cli.files;
    opt.outputMsi = requireArg(cli, 1, L"output .msi path");
    opt.productName = cli.productName;
    opt.version = cli.version;
    opt.manufacturer = cli.manufacturer;
    opt.productCode = cli.productCode;
    opt.upgradeCode = cli.upgradeCode;
    opt.platform = cli.platform;
    opt.scope = cli.scope;
    opt.installDir = cli.installDir;
    opt.installDirSet = cli.installDirSet;
    opt.language = cli.language;
    opt.force = cli.force;
    opt.dryRun = cli.dryRun;
    opt.verbose = cli.verbose;
    opt.configFile = cli.configFile;
    opt.uiTemplate = cli.uiTemplate;
    opt.uiLocale = cli.uiLocale;
    opt.desktopShortcut = cli.desktopShortcut;
    opt.startMenuShortcut = cli.startMenuShortcut;
    opt.mainExe = cli.mainExe;
    opt.cabSize = cli.cabSize;
    opt.keepEmptyDirs = cli.keepEmptyDirs;
    opt.largeFiles = cli.largeFiles;
    opt.ui = cli.ui;
    opt.signCert = cli.signCert;
    opt.signPassword = cli.signPassword;
    opt.signTimestamp = cli.signTimestamp;
    opt.signTool = cli.signTool;
    opt.uiSet = cli.uiSet;
    opt.largeFilesSet = cli.largeFilesSet;
    opt.cabSizeSet = cli.cabSizeSet;
    opt.signCertSet = cli.signCertSet;
    opt.uiTemplateSet = cli.uiTemplateSet;
    opt.productNameSet = cli.productNameSet;
    opt.versionSet = cli.versionSet;
    opt.manufacturerSet = cli.manufacturerSet;
    opt.productCodeSet = cli.productCodeSet;
    opt.upgradeCodeSet = cli.upgradeCodeSet;
    opt.platformSet = cli.platformSet;
    opt.scopeSet = cli.scopeSet;
    opt.languageSet = cli.languageSet;
    pack::pack(opt);
}

// ---- transform (.mst) commands --------------------------------------------

void cmdTransform(const Cli& cli) {
    mst::GenerateOptions o;
    o.baseMsi = requireArg(cli, 0, L"base package (.msi)");
    o.updatedMsi = requireArg(cli, 1, L"updated package (.msi)");
    // --dry-run only compares the packages, so the output path is optional.
    if (!cli.dryRun || cli.args.size() > 2)
        o.outMst = requireArg(cli, 2, L"output transform (.mst)");
    o.flags.errors = cli.mstErrors;
    o.flags.validation = cli.mstValidation;
    o.summaryInfo = !cli.noSummary;
    o.force = cli.force;
    o.verbose = cli.verbose;
    o.dryRun = cli.dryRun;

    mst::GenerateResult r = mst::generate(o);

    if (r.written) {
        util::outLine(L"Transform : " + r.outMst);
        util::outLine(L"Size      : " + humanSize(r.size));
        util::outLine(L"Errors    : " + mst::describeErrorFlags(cli.mstErrors) + L" (suppressed)");
        util::outLine(L"Validation: " + mst::describeValidationFlags(cli.mstValidation));
        util::outLine(L"Summary   : " +
                      std::wstring(cli.noSummary ? L"not written (--no-summary)"
                                                 : L"written (MsiCreateTransformSummaryInfo)"));
    } else if (r.identical) {
        util::outLine(L"Transform : (not written - the packages are identical)");
        util::outLine(L"Base      : " + util::path::full(o.baseMsi));
        util::outLine(L"Updated   : " + util::path::full(o.updatedMsi));
    } else {
        util::outLine(L"Transform : (dry run - nothing written)");
        util::outLine(L"Base      : " + util::path::full(o.baseMsi));
        util::outLine(L"Updated   : " + util::path::full(o.updatedMsi));
    }

    size_t tables = 0, added = 0, removed = 0, modified = 0, schema = 0;
    for (const auto& d : r.tables) {
        if (d.changes() == 0 && !d.schemaChanged) continue;
        ++tables;
        added += d.added;
        removed += d.removed;
        modified += d.modified;
        if (d.schemaChanged) ++schema;
    }

    if (tables == 0) {
        util::outLine(L"Changes   : none - the two packages have identical tables");
    } else {
        util::outLine(util::fmt(L"Changes   : %llu table(s), %llu added row(s), "
                                L"%llu removed, %llu modified",
                                static_cast<unsigned long long>(tables),
                                static_cast<unsigned long long>(added),
                                static_cast<unsigned long long>(removed),
                                static_cast<unsigned long long>(modified)));
        if (schema) util::outLine(L"            " + std::to_wstring(schema) +
                                  L" table(s) with a different column layout");
        util::outLine(L"");
        util::Table t;
        t.headers = {L"Table", L"Added", L"Removed", L"Modified", L"Schema"};
        for (const auto& d : r.tables) {
            if (d.changes() == 0 && !d.schemaChanged) continue;
            t.rows.push_back({d.table, std::to_wstring(d.added), std::to_wstring(d.removed),
                              std::to_wstring(d.modified),
                              d.schemaChanged ? L"changed"
                                              : (d.onlyBase ? L"only in base"
                                                            : (d.onlyUpdated ? L"only in updated"
                                                                             : L""))});
        }
        util::printTable(t, cli.tsv);
    }

    if (r.written)
        util::outLine(L"Preview   : msitool list " + o.baseMsi + L" properties --transform " +
                      r.outMst);
}

void cmdApply(const Cli& cli) {
    mst::ApplyOptions o;
    o.baseMsi = requireArg(cli, 0, L"base package (.msi)");
    o.outMsi = requireArg(cli, 1, L"output package (.msi)");
    for (size_t i = 2; i < cli.args.size(); ++i) o.transforms.push_back(cli.args[i]);
    for (const auto& tr : cli.transforms) o.transforms.push_back(tr);
    o.flags.errors = cli.mstErrors;
    o.flags.validation = cli.mstValidation;
    o.force = cli.force;
    o.verbose = cli.verbose;

    mst::ApplyResult r = mst::apply(o);
    util::outLine(util::fmt(L"Applied   : %llu transform(s)",
                            static_cast<unsigned long long>(r.applied)));
    util::outLine(L"Output    : " + r.outMsi);
    if (!r.packageKind.empty()) util::outLine(L"Package   : " + r.packageKind);

    // Confirm the committed rows by reopening the written package.
    try {
        msi::Db db(r.outMsi);
        util::Table t;
        t.headers = {L"Property", L"Value"};
        for (const wchar_t* key : {L"ProductName", L"ProductVersion", L"ProductCode",
                                   L"UpgradeCode", L"ProductLanguage"}) {
            std::wstring v = db.property(key);
            if (!v.empty()) t.rows.push_back({key, v});
        }
        size_t tables = db.tableNames().size();
        util::outLine(util::fmt(L"Tables    : %llu", static_cast<unsigned long long>(tables)));
        if (!t.rows.empty()) {
            util::outLine(L"");
            util::printTable(t, cli.tsv);
        }
    } catch (const util::Error& e) {
        util::warn(L"cannot reopen " + r.outMsi + L": " + e.message());
    }
}

// Table streams inside a transform are stored with obfuscated names; render
// them as printable text so a terminal never shows mojibake.
std::wstring escapeName(const std::wstring& name) {
    std::wstring out;
    for (wchar_t c : name) {
        if (c >= 0x20 && c < 0x7F) out.push_back(c);
        else out += util::fmt(L"\\x%04X", static_cast<unsigned>(c));
    }
    return out;
}

void cmdMstInfo(const Cli& cli) {
    std::wstring file = requireArg(cli, 0, L"transform path (.mst)");
    mst::TransformInfo info = mst::inspect(file);

    util::outLine(L"Transform : " + info.path);
    util::outLine(L"Size      : " + humanSize(info.size));
    util::outLine(L"Storage   : " + std::wstring(info.oleOk ? L"OLE compound file" : L"not OLE"));
    if (!info.targetProduct.empty()) {
        // Revision slot: "<base ProductCode><version>;<new code><version>;<UpgradeCode>"
        std::vector<std::wstring> parts;
        std::wstring cur;
        for (wchar_t c : info.targetProduct) {
            if (c == L';') { parts.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        parts.push_back(cur);
        std::wstring line = L"Applies to: " + parts[0];
        if (parts.size() > 1) line += L"  (transform creates " + parts[1] + L")";
        if (parts.size() > 2) line += L"  [UpgradeCode " + parts[2] + L"]";
        util::outLine(line);
    }
    util::outLine(L"Errors    : " + mst::describeErrorFlags(info.errorConditions) +
                  (info.hasSummary ? L"" : L" (no summary stream)"));
    util::outLine(L"Validation: " + mst::describeValidationFlags(info.validation));

    util::outLine(L"");
    util::outLine(L"=== SummaryInformation ===");
    if (info.summary.empty()) {
        util::outLine(L"(no summary stream; the transform carries no validation or "
                      L"error conditions)");
    } else {
        util::Table t;
        t.headers = {L"Property", L"Value"};
        for (const auto& s : info.summary) t.rows.push_back({s.name, s.value});
        util::printTable(t, cli.tsv);
    }

    util::outLine(L"");
    util::outLine(L"=== Changes ===");
    if (cli.baseMsi.empty()) {
        util::outLine(util::fmt(L"%llu table stream(s) inside the transform.",
                                static_cast<unsigned long long>(info.streams.size())));
        util::outLine(L"Pass --base-msi <setup.msi> to list the change log the installer sees "
                      L"(_TransformView).");
    } else {
        std::vector<mst::ChangeRow> changes = mst::viewTransform(cli.baseMsi, info.path);
        if (changes.empty()) {
            util::outLine(L"(the transform makes no change to this base package)");
        } else {
            util::Table t;
            t.headers = {L"Table", L"Row", L"Column", L"New value", L"Current value"};
            for (const auto& c : changes) {
                t.rows.push_back({c.table, c.row, c.column, c.data, c.current});
            }
            util::printTable(t, cli.tsv);
            util::outLine(util::fmt(L"%llu change row(s) against %s.",
                                    static_cast<unsigned long long>(changes.size()),
                                    cli.baseMsi.c_str()));
        }
    }

    if (cli.verbose) {
        util::outLine(L"");
        util::outLine(L"=== OLE streams ===");
        util::Table t;
        t.headers = {L"Stream", L"Bytes"};
        for (const auto& s : info.streams)
            t.rows.push_back({escapeName(s.first), std::to_wstring(s.second)});
        util::printTable(t, cli.tsv);
    }
}

int run(int argc, wchar_t** argv) {
    Cli cli = parseCommandLine(argc, argv);
    if (cli.help || cli.command.empty()) {
        util::out(kUsage);
        return 0;
    }

    std::wstring cmd = util::toLower(cli.command);
    // verify, pack and the transform commands validate their own inputs.
    if (cmd == L"verify") return cmdVerify(cli) ? 0 : 1;
    if (cmd == L"pack") { cmdPack(cli); return 0; }
    if (cmd == L"transform") { cmdTransform(cli); return 0; }
    if (cmd == L"apply") { cmdApply(cli); return 0; }
    if (cmd == L"mst" || cmd == L"mstinfo") { cmdMstInfo(cli); return 0; }

    std::wstring package = requireArg(cli, 0, L"package path (.msi)");
    msi::Db db(package);  // preflight + MsiOpenDatabase + _Tables check
    if (!db.isPatch()) {
        for (const auto& tr : cli.transforms) {
            db.applyTransform(tr);
            util::verboseLine(cli.verbose, L"applied transform " + tr);
        }
    }

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
    } else if (cmd == L"effects" || cmd == L"fx" || cmd == L"install-effects") {
        effects::Options o;
        o.outDir = requireArg(cli, 1, L"output directory");
        o.format = effects::parseFormat(cli.effectsFormat);
        o.verbose = cli.verbose;
        effects::exportEffects(db, o);
    } else {
        util::fail(L"Unknown command '" + cli.command + L"'. Run 'msitool --help'.");
    }
    return 0;
}

}  // namespace

// A GUI-subsystem executable has no console of its own. In CLI mode we
// attach to the caller's console (or allocate one when started from Explorer)
// and rebind the CRT streams, so all existing CLI commands keep working
// byte-for-byte, including redirected stdout.
static void attachConsoleForCli() {
    // Remember the inherited handles: when stdout/stderr were redirected by
    // the shell (`> out.tsv`) we must NOT replace them with the console.
    auto valid = [](HANDLE h) { return h && h != INVALID_HANDLE_VALUE; };
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
    bool hasIn = valid(hIn), hasOut = valid(hOut), hasErr = valid(hErr);
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) AllocConsole();
    if (!hasIn)  _wfreopen(L"CONIN$", L"r", stdin);
    if (!hasOut) _wfreopen(L"CONOUT$", L"w", stdout);
    if (!hasErr) _wfreopen(L"CONOUT$", L"w", stderr);
    SetConsoleTitleW(L"msitool - Windows Installer toolkit");
}

static bool isGuiLaunch(int argc, wchar_t** argv, std::wstring& initialFile) {
    if (argc <= 1) return true;                       // double click / no args
    std::wstring first = argv[1] ? argv[1] : L"";
    if (util::iequals(first, L"gui") || util::iequals(first, L"--gui") ||
        util::iequals(first, L"/gui")) {
        if (argc > 2 && argv[2]) initialFile = argv[2];
        return true;
    }
    return false;
}

static int start(int argc, wchar_t** argv, HINSTANCE instance) {
    if (!argv) return 2;
    std::wstring initialFile;
    if (isGuiLaunch(argc, argv, initialFile))
        return ui::run(instance, initialFile);

    attachConsoleForCli();
    util::setConsoleUtf8();
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
    fflush(stdout);
    fflush(stderr);
    return rc;
}

// Primary entry point for the WINDOWS subsystem build.
int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int) {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = start(argc, argv, instance);
    if (argv) LocalFree(argv);
    return rc;
}

// Kept so the sources also link in a plain CONSOLE subsystem build.
int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    int rc = start(argc, argv, GetModuleHandleW(nullptr));
    if (argv) LocalFree(argv);
    return rc;
}
