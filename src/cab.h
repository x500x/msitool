#pragma once

#include "util.h"
#include <unordered_map>

namespace cab {

struct Result {
    bool                       ok = false;
    std::vector<std::wstring>  extracted;  // lowercased names as stored in the cab
    std::vector<std::wstring>  skipped;    // lowercased names skipped (already present)
    std::vector<std::wstring>  warnings;
    std::wstring               error;
};

// Extracts one cabinet file with FDI (cabinet.dll).
//
// nameToTarget maps a *lowercased* CAB-internal file name to an absolute
// destination path. Entries missing from the map are written below
// fallbackDir (their relative CAB path is sanitised first).
// tempDir is used for Quantum spill files, and must exist.
Result extract(const std::wstring& cabPath,
               const std::unordered_map<std::wstring, std::wstring>& nameToTarget,
               const std::wstring& fallbackDir,
               const std::wstring& tempDir,
               bool dryRun,
               bool skipExisting,
               bool verbose);

}  // namespace cab
