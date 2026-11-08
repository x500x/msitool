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

// Cabinet header information used to reconstruct cabinet sets (chains).
struct CabinetInfo {
    bool           valid = false;
    unsigned short setID = 0;
    unsigned short iCabinet = 0;   // 0-based index within the set
    bool           hasPrev = false;
    bool           hasNext = false;
};

CabinetInfo inspect(const std::wstring& cabPath);

// Groups staged cabinets into independent chains: cabinets linked through
// hasprev/hasnext with a matching setID and consecutive iCabinet values are
// returned together, in order. Standalone cabinets form one-element groups.
std::vector<std::vector<size_t>> groupCabinetChains(
    const std::vector<std::wstring>& cabPaths);

// Extracts a cabinet set with FDI (cabinet.dll). cabPaths[0] is the first
// cabinet; following entries are offered to FDI through fdintNEXT_CABINET so
// a file (or folder) that spans cabinet boundaries is written as one file.
//
// nameToTarget maps a *lowercased* CAB-internal file name to an absolute
// destination path. Entries missing from the map are written below
// fallbackDir (their relative CAB path is sanitised first), or skipped
// entirely when skipUnmapped is true (used by selective extraction).
// tempDir is used for Quantum spill files, and must exist.
Result extract(const std::vector<std::wstring>& cabPaths,
               const std::unordered_map<std::wstring, std::wstring>& nameToTarget,
               const std::wstring& fallbackDir,
               const std::wstring& tempDir,
               bool dryRun,
               bool skipExisting,
               bool verbose,
               bool skipUnmapped = false);

inline Result extract(const std::wstring& cabPath,
                      const std::unordered_map<std::wstring, std::wstring>& nameToTarget,
                      const std::wstring& fallbackDir,
                      const std::wstring& tempDir,
                      bool dryRun,
                      bool skipExisting,
                      bool verbose,
                      bool skipUnmapped = false) {
    return extract(std::vector<std::wstring>{cabPath}, nameToTarget, fallbackDir,
                   tempDir, dryRun, skipExisting, verbose, skipUnmapped);
}

}  // namespace cab
