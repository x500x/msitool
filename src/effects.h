#pragma once

// "Install effects" export: turn the non-file side of an MSI (registry,
// services, environment variables, shortcuts, custom actions) into readable /
// executable scripts AND into a pack manifest that can be fed back into
// `msitool pack --config`.
//
// Everything is derived from the MSI tables only: no properties are evaluated
// by the installer, no custom action is executed and no registry key is read
// or written. Formatted strings ([ProductVersion] etc.) are resolved against
// the Property table when the value is a plain literal, otherwise they are
// kept verbatim and flagged in the generated report.

#include "util.h"
#include "msidb.h"

#include <string>
#include <vector>

namespace effects {

enum class Format {
    All,        // scripts + pack.xml
    Scripts,    // .reg / .cmd / .ps1 / .txt / .md only
    Manifest,   // pack.xml (+ referenced binary/icon resources) only
};

struct Options {
    std::wstring outDir;
    Format       format = Format::All;
    bool         verbose = false;
};

struct Stats {
    size_t                    registry = 0;
    size_t                    services = 0;
    size_t                    serviceControls = 0;
    size_t                    environment = 0;
    size_t                    shortcuts = 0;
    size_t                    customActions = 0;
    size_t                    properties = 0;
    size_t                    files = 0;
    std::vector<std::wstring> outputs;   // absolute paths, in write order
};

// Accepts "all" / "scripts" / "manifest" (and a few friendly aliases).
Format parseFormat(const std::wstring& text);

Stats exportEffects(msi::Db& db, const Options& opt);

}  // namespace effects
