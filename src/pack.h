#pragma once

#include "util.h"

namespace pack {

struct Options {
    std::wstring inputDir;

    // Selection mode: pack only these entries instead of the whole tree below
    // inputDir. Paths may be absolute or relative to inputDir; a directory
    // entry adds its complete subtree. inputDir stays the package root, so
    // Directory keys, the CAB layout and the companion layout are unchanged.
    std::vector<std::wstring> inputFiles;

    std::wstring outputMsi;
    std::wstring productName;      // empty => directory name
    std::wstring version = L"1.0.0";
    std::wstring manufacturer = L"msitool";
    std::wstring productCode;      // empty => generate a GUID
    std::wstring upgradeCode;      // empty => generate a GUID
    std::wstring platform = L"Intel";// "x64" or "Intel"
    int         language = 1033;

    bool        force = false;
    bool        dryRun = false;
    bool        verbose = false;

    // Split the payload into several embedded cabinets. 0 = a single CAB.
    // The value is a byte limit of *uncompressed* input data per cabinet.
    unsigned long long cabSize = 0;

    // Emit CreateFolder rows so directories without files survive setup.
    bool        keepEmptyDirs = true;

    // "layout": files > 2 GiB are kept as uncompressed companion files next
    // to the MSI (MSI FileSize is a 32-bit integer and FCI seeks with long).
    // "error": reject such files (old behaviour).
    std::wstring largeFiles = L"layout";

    // "none" | "basic" | "minimal".
    std::wstring ui = L"none";

    // Optional manifest describing features / shortcuts / registry / services
    // / environment variables / [pack] overrides. See README.md.
    std::wstring configFile;

    // Optional Authenticode signing of the finished package.
    std::wstring signCert;         // .pfx with a code-signing certificate
    std::wstring signPassword;
    std::wstring signTimestamp;    // RFC 3161 timestamp URL
    std::wstring signTool;         // explicit path to signtool.exe

    // True when the corresponding value came from the command line (manifest
    // [pack] values are only defaults, never override explicit CLI options).
    bool        uiSet = false;
    bool        largeFilesSet = false;
    bool        cabSizeSet = false;
    bool        signCertSet = false;
};

struct Stats {
    size_t          files = 0;
    size_t          directories = 0;
    size_t          components = 0;
    size_t          cabinets = 0;
    size_t          emptyDirs = 0;
    size_t          features = 0;
    size_t          shortcuts = 0;
    size_t          registry = 0;
    size_t          services = 0;
    size_t          environment = 0;
    size_t          externalFiles = 0;
    unsigned long long cabBytes = 0;
    unsigned long long msiBytes = 0;
    std::wstring    output;
    bool            signedPackage = false;
};

// Packs a directory tree into a self-contained .msi (one or more embedded
// CABs, plus an optional uncompressed companion tree for > 2 GiB files).
Stats pack(const Options& opt);

}  // namespace pack
