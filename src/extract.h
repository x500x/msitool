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

    // .mst transforms applied as a temporary view to the opened database.
    std::vector<std::wstring> transforms;
    // For .msp packages: an optional target MSI whose transformed view is
    // used to name and lay out the patch payload.
    std::wstring baseMsi;
    // Additional staged cabinet streams (name -> file path), e.g. the CABs
    // carried inside a patch package. Used when a Media row references a
    // stream that is not present in the opened database.
    std::vector<std::pair<std::wstring, std::wstring>> extraStreams;

    // When non-empty, extract only these File keys (single file / single
    // directory extraction from the GUI browser). Empty extracts everything.
    std::vector<std::wstring> onlyFiles;
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
    std::wstring      stagedPath;   // pre-staged CAB (e.g. from a patch package)
    unsigned long long size = 0;    // bytes (embedded stream or external file)
    bool              available = false;
};

// extraStreams maps a stream name to a staged CAB file; it lets a patch
// package supply cabinets referenced by a transformed base MSI.
std::vector<MediaInfo> readMedia(
    msi::Db& db,
    const std::vector<std::pair<std::wstring, std::wstring>>& extraStreams = {});

// ---------------------------------------------------------------------------
// Package inventory (read-only, no disk writes) used by the GUI file browser.
// ---------------------------------------------------------------------------
struct BrowseDir {
    std::wstring key;        // Directory.Directory
    std::wstring name;       // Directory.DefaultDir long name (sanitized)
    std::wstring parentKey;  // Directory.Directory_Parent ("" for a root)
    std::wstring targetRel;  // installed relative path ("" for the root)
    std::wstring sourceRel;  // source-image relative path
};

struct BrowseFile {
    std::wstring       key;        // File.File
    std::wstring       name;       // File.FileName long name (sanitized)
    std::wstring       component;  // File.Component_
    std::wstring       dirKey;     // Directory key of the component
    std::wstring       relPath;    // installed / source image relative path
    unsigned long long size = 0;   // File.FileSize (0 when unknown)
    int                sequence = 0;
};

struct BrowseTree {
    std::vector<BrowseDir>  dirs;
    std::vector<BrowseFile> files;
    unsigned long long      totalSize = 0;
};

// Directory/file inventory of the package. sourceLayout picks the source
// image path for relPath/targetRel instead of the installed path.
BrowseTree browse(msi::Db& db, bool sourceLayout);

Stats extractPackage(msi::Db& db, const Options& opt);

// Extracts a Windows Installer patch (.msp): embedded CAB payloads, raw
// streams, transform streams, and optionally the transformed base MSI view.
Stats extractPatch(msi::Db& patch, const Options& opt);

// Writes every embedded / external cabinet into outDir plus manifest.txt.
void exportCabs(msi::Db& db, const std::wstring& outDir, bool verbose);

// Full archive export: .idt tables, raw streams, Binary entries, summary.txt.
void exportArchive(msi::Db& db, const std::wstring& outDir, bool verbose);

}  // namespace extract
