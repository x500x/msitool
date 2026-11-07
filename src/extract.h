#pragma once

#include "util.h"
#include "msidb.h"

namespace extract {

struct Options {
    std::wstring outDir;
    bool flat = false;
    bool dryRun = false;
    bool skipExisting = false;
    bool keepTemp = false;
    bool sourceLayout = false;  // output the source-image tree instead of the installed tree
    bool verbose = false;
};

struct Stats {
    size_t fileRows = 0;
    size_t written = 0;
    size_t copied = 0;
    size_t skipped = 0;
    size_t cabCount = 0;
    size_t warnings = 0;
};

struct MediaInfo {
    int               diskId = 0;
    int               lastSequence = 0;
    std::wstring      cabinet;
    bool              embedded = false;
    std::wstring      streamName;
    std::wstring      sourcePath;   // external cabinet, resolved against the MSI folder
    unsigned long long size = 0;    // bytes (embedded stream or external file)
    bool              available = false;
};

std::vector<MediaInfo> readMedia(msi::Db& db);

Stats extractPackage(msi::Db& db, const Options& opt);

// Writes every embedded / external cabinet into outDir plus manifest.txt.
void exportCabs(msi::Db& db, const std::wstring& outDir, bool verbose);

// Full archive export: .idt tables, raw streams, Binary entries, summary.txt.
void exportArchive(msi::Db& db, const std::wstring& outDir, bool verbose);

}  // namespace extract
