#pragma once

#include "util.h"

namespace pack {

struct Options {
    std::wstring inputDir;
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
};

struct Stats {
    size_t          files = 0;
    size_t          directories = 0;
    size_t          components = 0;
    unsigned long long cabBytes = 0;
    unsigned long long msiBytes = 0;
    std::wstring    output;
};

// Packs a directory tree into a self-contained .msi with one embedded CAB.
Stats pack(const Options& opt);

}  // namespace pack
