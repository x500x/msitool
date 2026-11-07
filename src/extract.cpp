#include "extract.h"
#include "cab.h"

#include <stdlib.h>
#include <algorithm>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace extract {

namespace {

struct TableData {
    std::vector<std::wstring>              cols;
    std::vector<std::vector<std::wstring>> rows;

    int column(const std::wstring& name) const {
        for (size_t i = 0; i < cols.size(); ++i)
            if (util::iequals(cols[i], name)) return static_cast<int>(i);
        return -1;
    }
    std::wstring get(size_t row, const std::wstring& name) const {
        int c = column(name);
        if (c < 0 || row >= rows.size() || static_cast<size_t>(c) >= rows[row].size())
            return L"";
        return rows[row][static_cast<size_t>(c)];
    }
};

TableData loadTable(msi::Db& db, const std::wstring& table) {
    TableData t;
    if (!db.tableExists(table)) return t;
    try {
        msi::QueryResult r = db.query(L"SELECT * FROM " + msi::quoteIdent(table));
        t.cols = std::move(r.columns);
        t.rows = std::move(r.rows);
    } catch (const util::Error&) {
        // A broken/non-standard table should not kill the whole extraction.
    }
    return t;
}

std::wstring stripProps(std::wstring s) {
    for (;;) {
        size_t a = s.find(L'[');
        if (a == std::wstring::npos) break;
        size_t b = s.find(L']', a);
        if (b == std::wstring::npos) { s.erase(a); break; }
        s.erase(a, b - a + 1);
    }
    while (!s.empty() && (s.front() == L'\\' || s.front() == L'/')) s.erase(0, 1);
    return util::trim(s);
}

std::wstring expandProps(std::wstring s,
                         const std::unordered_map<std::wstring, std::wstring>& props) {
    for (int pass = 0; pass < 3; ++pass) {
        std::wstring r;
        bool changed = false;
        size_t i = 0;
        while (i < s.size()) {
            if (s[i] == L'[') {
                size_t b = s.find(L']', i);
                if (b != std::wstring::npos) {
                    std::wstring name = s.substr(i + 1, b - i - 1);
                    auto it = props.find(util::toLower(name));
                    if (it != props.end() && !it->second.empty()) {
                        r += it->second;
                        changed = true;
                    } else {
                        r += name;  // keep the property name as a visible directory
                        changed = true;
                    }
                    i = b + 1;
                    continue;
                }
            }
            r.push_back(s[i++]);
        }
        s = std::move(r);
        if (!changed || s.find(L'[') == std::wstring::npos) break;
    }
    return s;
}

// MSI Directory.DefaultDir syntax:
//     "target[:source]", each side optionally "short|long".
// The target side is the directory created on the installed machine, the
// source side is the directory inside an uncompressed ("layout") source
// image. A dot means "same as the parent"; with no colon both sides are
// identical. Administrative images (Media.Cabinet empty) rely on this
// distinction, e.g. ".:x64" installs into APPDIR but reads from .\x64.
struct DirPaths {
    std::wstring target;
    std::wstring source;
};

std::pair<std::wstring, std::wstring> splitTargetSource(const std::wstring& def) {
    size_t colon = def.find(L':');
    if (colon == std::wstring::npos) return {def, def};
    // A literal "C:\..." is a path, not a target:source pair.
    if (colon == 1 && def.size() > 2 && (def[2] == L'\\' || def[2] == L'/'))
        return {def, def};
    return {def.substr(0, colon), def.substr(colon + 1)};
}

std::wstring dirLeaf(const std::wstring& raw,
                     const std::unordered_map<std::wstring, std::wstring>& props) {
    std::wstring s = raw;
    size_t bar = s.find(L'|');
    if (bar != std::wstring::npos) s = s.substr(bar + 1);  // short|long -> long
    s = util::trim(s);
    if (s.empty() || s == L"." || s == L".." || util::iequals(s, L"SourceDir"))
        return L"";
    return util::path::sanitizeRelative(expandProps(s, props));
}

DirPaths combineDirs(const DirPaths& parent, const std::wstring& targetLeaf,
                     const std::wstring& sourceLeaf) {
    DirPaths r;
    r.target = util::path::join(parent.target, targetLeaf);
    r.source = util::path::join(parent.source, sourceLeaf);
    return r;
}

std::wstring findStreamName(const std::vector<std::wstring>& streams,
                            const std::wstring& wanted) {
    for (const auto& s : streams)
        if (s == wanted) return s;
    for (const auto& s : streams)
        if (util::iequals(s, wanted)) return s;
    return L"";
}

unsigned long long streamSize(msi::Db& db, const std::wstring& name) {
    msi::Handle view, rec, params;
    if (MsiDatabaseOpenViewW(db.handle(),
                             L"SELECT `Data` FROM `_Streams` WHERE `Name`=?",
                             view.out()) != ERROR_SUCCESS)
        return 0;
    params.reset(MsiCreateRecord(1));
    if (!params) return 0;
    MsiRecordSetStringW(params.get(), 1, name.c_str());
    if (MsiViewExecute(view.get(), params.get()) != ERROR_SUCCESS) return 0;
    if (MsiViewFetch(view.get(), rec.out()) != ERROR_SUCCESS) return 0;
    return MsiRecordDataSize(rec.get(), 1);
}

unsigned long long fileSize(const std::wstring& p) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned long long>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

void writeText(const std::wstring& path, const std::wstring& text) {
    std::string bytes = util::narrow(text, CP_UTF8);
    util::path::writeFile(path, bytes.data(), bytes.size());
}

void showWarnings(const std::vector<std::wstring>& warnings) {
    const size_t shown = std::min<size_t>(warnings.size(), 20);
    for (size_t i = 0; i < shown; ++i) util::errLine(L"[warn] " + warnings[i]);
    if (warnings.size() > shown)
        util::errLine(util::fmt(L"[warn] ... and %llu more warning(s)",
                                static_cast<unsigned long long>(warnings.size() - shown)));
}

struct FileItem {
    std::wstring key;        // File.File  == name inside the CAB
    std::wstring component;  // File.Component_
    std::wstring name;       // File.FileName ("short|long")
    std::wstring target;     // absolute destination path
    std::wstring targetRel;  // installed directory (Directory.DefaultDir target side)
    std::wstring sourceRel;  // source-image directory (source side)
    int          sequence = 0;
};

struct CabJob {
    MediaInfo                                              media;
    std::unordered_map<std::wstring, std::wstring>         nameToTarget;
    std::unordered_map<std::wstring, size_t>               nameToFile;
    std::wstring                                           localPath;
};

}  // namespace

std::vector<MediaInfo> readMedia(msi::Db& db) {
    std::vector<MediaInfo> result;
    TableData t = loadTable(db, L"Media");
    if (t.cols.empty()) return result;

    std::vector<std::wstring> streams = db.streamNames();
    std::wstring msiDir = util::path::dirName(db.path());

    for (size_t i = 0; i < t.rows.size(); ++i) {
        MediaInfo m;
        m.diskId = _wtoi(t.get(i, L"DiskId").c_str());
        m.lastSequence = _wtoi(t.get(i, L"LastSequence").c_str());
        m.cabinet = util::trim(t.get(i, L"Cabinet"));
        if (!m.cabinet.empty()) {
            if (m.cabinet.front() == L'#') {
                m.embedded = true;
                m.streamName = m.cabinet.substr(1);
                std::wstring resolved = findStreamName(streams, m.streamName);
                if (!resolved.empty()) m.streamName = resolved;
                m.available = !resolved.empty();
                m.size = streamSize(db, m.streamName);
            } else {
                std::wstring rel = util::path::sanitizeRelative(stripProps(m.cabinet));
                if (!rel.empty()) {
                    std::wstring p = util::path::join(msiDir, rel);
                    if (!util::path::exists(p)) {
                        std::wstring alt =
                            util::path::join(msiDir, util::path::fileName(rel));
                        if (util::path::exists(alt)) p = alt;
                    }
                    m.sourcePath = util::path::full(p);
                    m.available = util::path::exists(m.sourcePath) &&
                                  !util::path::isDirectory(m.sourcePath);
                    if (m.available) m.size = fileSize(m.sourcePath);
                }
            }
        }
        result.push_back(std::move(m));
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const MediaInfo& a, const MediaInfo& b) {
                         return a.lastSequence < b.lastSequence;
                     });
    return result;
}

Stats extractPackage(msi::Db& db, const Options& opt) {
    Stats st;
    std::vector<std::wstring> warnings;
    const std::wstring outDir = util::path::full(opt.outDir);
    const std::wstring msiDir = util::path::dirName(db.path());

    if (!opt.dryRun && !util::path::makeDirs(outDir))
        util::fail(L"Cannot create output directory: " + outDir);

    TableData dirTable = loadTable(db, L"Directory");
    TableData compTable = loadTable(db, L"Component");
    TableData fileTable = loadTable(db, L"File");
    TableData propTable = loadTable(db, L"Property");
    std::vector<MediaInfo> media = readMedia(db);

    struct DirRec {
        std::wstring parent;
        std::wstring def;
    };
    std::unordered_map<std::wstring, DirRec> dirs;
    for (size_t i = 0; i < dirTable.rows.size(); ++i) {
        std::wstring key = dirTable.get(i, L"Directory");
        if (key.empty()) continue;
        dirs[util::toLower(key)] = {dirTable.get(i, L"Directory_Parent"),
                                    dirTable.get(i, L"DefaultDir")};
    }
    std::unordered_map<std::wstring, std::wstring> compDirs;
    for (size_t i = 0; i < compTable.rows.size(); ++i) {
        std::wstring key = compTable.get(i, L"Component");
        if (!key.empty()) compDirs[util::toLower(key)] = compTable.get(i, L"Directory_");
    }
    std::unordered_map<std::wstring, std::wstring> props;
    for (size_t i = 0; i < propTable.rows.size(); ++i) {
        std::wstring key = propTable.get(i, L"Property");
        if (!key.empty()) props[util::toLower(key)] = propTable.get(i, L"Value");
    }

    std::function<DirPaths(const std::wstring&, int)> resolveDir =
        [&](const std::wstring& key, int depth) -> DirPaths {
        std::wstring k = util::toLower(util::trim(key));
        if (k.empty() || k == L"targetdir" || depth > 64) return {};
        auto it = dirs.find(k);
        if (it == dirs.end()) return {};
        DirPaths parent;
        if (!it->second.parent.empty() &&
            !util::iequals(it->second.parent, key) &&
            !util::iequals(it->second.parent, L"TARGETDIR"))
            parent = resolveDir(it->second.parent, depth + 1);
        auto sides = splitTargetSource(it->second.def);
        return combineDirs(parent, dirLeaf(sides.first, props),
                           dirLeaf(sides.second, props));
    };

    std::unordered_map<std::wstring, int> targetCount;
    auto uniqueTarget = [&](const std::wstring& p) {
        std::wstring key = util::toLower(p);
        int n = ++targetCount[key];
        if (n == 1) return p;
        std::wstring dir = util::path::dirName(p);
        std::wstring base = util::path::fileName(p);
        std::wstring ext = util::path::ext(base);
        std::wstring stem = base.substr(0, base.size() - ext.size());
        return dir + stem + L"_" + std::to_wstring(n) + ext;
    };

    std::vector<FileItem> files;
    for (size_t i = 0; i < fileTable.rows.size(); ++i) {
        FileItem f;
        f.key = fileTable.get(i, L"File");
        if (f.key.empty()) continue;
        f.component = fileTable.get(i, L"Component_");
        f.name = fileTable.get(i, L"FileName");
        f.sequence = _wtoi(fileTable.get(i, L"Sequence").c_str());

        auto cd = compDirs.find(util::toLower(f.component));
        DirPaths dp = cd == compDirs.end() ? DirPaths{} : resolveDir(cd->second, 0);
        std::wstring leaf = util::path::longName(f.name);
        if (leaf.empty()) leaf = f.key;
        leaf = util::path::sanitizeComponent(leaf);
        std::wstring outRel = opt.sourceLayout ? dp.source : dp.target;
        std::wstring base = opt.flat ? outDir : util::path::join(outDir, outRel);
        f.target = uniqueTarget(util::path::join(base, leaf));
        f.targetRel = outRel;
        f.sourceRel = dp.source;
        util::verboseLine(opt.verbose && dp.target != dp.source,
                          f.key + L": installed dir='" + dp.target +
                              L"', layout dir='" + dp.source + L"'");
        files.push_back(std::move(f));
    }
    st.fileRows = files.size();
    util::verboseLine(opt.verbose,
                      util::fmt(L"File table: %llu row(s), Directory: %llu, Media: %llu",
                                static_cast<unsigned long long>(files.size()),
                                static_cast<unsigned long long>(dirTable.rows.size()),
                                static_cast<unsigned long long>(media.size())));

    // Count aliases so that ambiguous short/long names are never mapped.
    std::unordered_map<std::wstring, size_t> aliasCount;
    for (const auto& f : files) {
        size_t bar = f.name.find(L'|');
        std::wstring shortName = bar == std::wstring::npos ? L"" : f.name.substr(0, bar);
        std::wstring longName = util::path::longName(f.name);
        for (const std::wstring& a : {shortName, longName}) {
            if (!a.empty())
                ++aliasCount[util::toLower(util::path::sanitizeComponent(a))];
        }
    }

    // Assign File.Sequence -> Media cabinet.
    std::vector<CabJob> jobs;
    std::vector<int> fileCab(files.size(), -1);
    int previousLast = 0;
    for (const auto& m : media) {
        if (!m.cabinet.empty()) {
            CabJob job;
            job.media = m;
            int jobIndex = static_cast<int>(jobs.size());
            for (size_t k = 0; k < files.size(); ++k) {
                if (fileCab[k] >= 0 || files[k].sequence <= previousLast ||
                    files[k].sequence > m.lastSequence)
                    continue;
                fileCab[k] = jobIndex;
                std::wstring key = util::toLower(files[k].key);
                job.nameToTarget[key] = files[k].target;
                job.nameToFile[key] = k;
            }
            jobs.push_back(std::move(job));
        }
        previousLast = m.lastSequence;
    }

    // Secondary (unique) aliases help with packages whose CAB names differ
    // slightly from the File table primary key. Only add them to the cabinet
    // the file actually belongs to, otherwise a name from another disk could
    // be redirected to the wrong destination.
    for (size_t j = 0; j < jobs.size(); ++j) {
        CabJob& job = jobs[j];
        for (size_t k = 0; k < files.size(); ++k) {
            if (fileCab[k] != static_cast<int>(j)) continue;
            size_t bar = files[k].name.find(L'|');
            std::wstring shortName = bar == std::wstring::npos ? L"" : files[k].name.substr(0, bar);
            std::wstring longName = util::path::longName(files[k].name);
            for (const std::wstring& a : {shortName, longName}) {
                std::wstring key = util::toLower(util::path::sanitizeComponent(a));
                if (key.empty() || aliasCount[key] != 1 || job.nameToTarget.count(key)) continue;
                job.nameToTarget[key] = files[k].target;
                job.nameToFile[key] = k;
            }
        }
    }

    util::TempDir temp(L"msitool");
    if (opt.keepTemp) temp.keep();
    const std::wstring fallbackDir = util::path::join(outDir, L"_unmapped");

    std::unordered_set<size_t> writtenFiles;
    for (size_t j = 0; j < jobs.size(); ++j) {
        CabJob& job = jobs[j];
        job.localPath = util::path::join(
            temp.path(), util::fmt(L"c%u.cab", static_cast<unsigned>(j + 1)));

        bool prepared = false;
        std::wstring stageError;
        if (job.media.embedded) {
            if (!job.media.available) {
                stageError = L"embedded stream '" + job.media.streamName + L"' not found";
            } else {
                try {
                    auto data = db.readStream(L"SELECT `Data` FROM `_Streams` WHERE `Name`=?",
                                              {job.media.streamName}, 1);
                    prepared = util::path::writeFile(job.localPath, data);
                    if (!prepared) stageError = L"cannot write temporary cabinet";
                } catch (const util::Error& e) {
                    stageError = L"cannot read stream '" + job.media.streamName +
                                 L"': " + e.message();
                }
            }
        } else if (job.media.available) {
            prepared = util::path::copyFile(job.media.sourcePath, job.localPath);
            if (!prepared) stageError = L"cannot copy " + job.media.sourcePath;
        } else {
            stageError = L"cabinet file not found: " +
                         (job.media.sourcePath.empty() ? job.media.cabinet
                                                       : job.media.sourcePath);
        }
        if (!prepared) {
            warnings.push_back(L"Skipping cabinet '" + job.media.cabinet + L"': " + stageError);
            continue;
        }

        std::unordered_set<size_t> mappedFiles;
        for (const auto& kv : job.nameToFile) mappedFiles.insert(kv.second);
        util::outLine(util::fmt(L"[cab %u/%llu] %s (%llu bytes, %llu file(s), %llu name(s))",
                                static_cast<unsigned>(j + 1),
                                static_cast<unsigned long long>(jobs.size()),
                                job.media.cabinet.c_str(),
                                fileSize(job.localPath),
                                static_cast<unsigned long long>(mappedFiles.size()),
                                static_cast<unsigned long long>(job.nameToTarget.size())));

        cab::Result r = cab::extract(job.localPath, job.nameToTarget, fallbackDir,
                                     temp.path(), opt.dryRun, opt.skipExisting, opt.verbose);
        for (const auto& w : r.warnings) warnings.push_back(w);
        if (!r.ok) {
            warnings.push_back(r.error);
            continue;
        }
        ++st.cabCount;
        st.skipped += r.skipped.size();
        st.written += r.extracted.size();
        for (const auto& name : r.extracted) {
            auto it = job.nameToFile.find(name);
            if (it != job.nameToFile.end()) writtenFiles.insert(it->second);
        }
        // Skipped files are already present on disk - do not treat them as
        // missing sources in the fallback pass below.
        for (const auto& name : r.skipped) {
            auto it = job.nameToFile.find(name);
            if (it != job.nameToFile.end()) writtenFiles.insert(it->second);
        }
    }

    // Anything not produced by a cabinet: try an uncompressed ("layout") source
    // tree next to the MSI, which is how administrative images are shipped.
    for (size_t k = 0; k < files.size(); ++k) {
        if (writtenFiles.count(k)) continue;
        // The source image may store either the long name, the 8.3 short name
        // or the File table key. Try each of them in the layout source dir
        // first, then the installed dir (in case target/source were identical
        // but expressed differently), then the bare MSI folder.
        std::wstring leaf = util::path::longName(files[k].name);
        size_t bar = files[k].name.find(L'|');
        std::wstring shortName = bar == std::wstring::npos ? L"" : files[k].name.substr(0, bar);
        std::vector<std::wstring> names;
        for (const std::wstring& n : {leaf, shortName, files[k].key})
            if (!n.empty()) names.push_back(n);

        std::vector<std::wstring> candidates;
        for (const auto& n : names)
            if (!files[k].sourceRel.empty())
                candidates.push_back(
                    util::path::join(msiDir, util::path::join(files[k].sourceRel, n)));
        if (files[k].targetRel != files[k].sourceRel)
            for (const auto& n : names)
                if (!files[k].targetRel.empty())
                    candidates.push_back(
                        util::path::join(msiDir, util::path::join(files[k].targetRel, n)));
        for (const auto& n : names)
            candidates.push_back(util::path::join(msiDir, n));

        bool done = false;
        for (const auto& c : candidates) {
            if (!util::path::exists(c) || util::path::isDirectory(c)) continue;
            if (opt.skipExisting && util::path::exists(files[k].target)) {
                ++st.skipped;
                done = true;
                break;
            }
            if (opt.dryRun) {
                util::verboseLine(opt.verbose, L"copy " + c + L" -> " + files[k].target);
                ++st.copied;
                done = true;
                break;
            }
            if (util::path::copyFile(c, files[k].target)) {
                ++st.copied;
                done = true;
                break;
            }
        }
        if (!done)
            warnings.push_back(L"No source data for '" + files[k].key + L"' (expected " +
                               files[k].target + L")");
    }

    st.warnings = warnings.size();
    showWarnings(warnings);

    util::outLine(L"");
    util::outLine(util::fmt(
        L"%s %llu file(s) from %llu cabinet(s), %llu copied from layout, "
        L"%llu skipped, %llu warning(s).  File table: %llu row(s).",
        opt.dryRun ? L"[dry-run] Would extract" : L"Extracted",
        static_cast<unsigned long long>(st.written),
        static_cast<unsigned long long>(st.cabCount),
        static_cast<unsigned long long>(st.copied),
        static_cast<unsigned long long>(st.skipped),
        static_cast<unsigned long long>(st.warnings),
        static_cast<unsigned long long>(st.fileRows)));
    util::outLine(L"Output: " + outDir);
    if (opt.keepTemp) util::outLine(L"Temporary cabinets kept in: " + temp.path());
    return st;
}

void exportCabs(msi::Db& db, const std::wstring& outDir, bool verbose) {
    if (!util::path::makeDirs(outDir))
        util::fail(L"Cannot create output directory: " + outDir);
    std::vector<MediaInfo> media = readMedia(db);
    std::vector<std::wstring> warnings;
    std::wstring manifest =
        L"# DiskId\tLastSequence\tCabinet\tType\tSize\tOutput\n";
    size_t written = 0;

    for (size_t i = 0; i < media.size(); ++i) {
        const MediaInfo& m = media[i];
        std::wstring type = m.cabinet.empty() ? L"uncompressed-layout"
                                              : (m.embedded ? L"embedded" : L"external");
        std::wstring output = L"-";
        if (!m.cabinet.empty()) {
            std::wstring rel = util::path::sanitizeRelative(
                m.embedded ? m.streamName : stripProps(m.cabinet));
            if (rel.empty()) rel = util::fmt(L"cab%u.cab", static_cast<unsigned>(i + 1));
            if (util::path::ext(rel).empty()) rel += L".cab";
            output = rel;
            std::wstring dst = util::path::join(outDir, rel);
            bool ok = false;
            if (m.embedded) {
                try {
                    auto data = db.readStream(L"SELECT `Data` FROM `_Streams` WHERE `Name`=?",
                                              {m.streamName}, 1);
                    ok = util::path::writeFile(dst, data);
                } catch (const util::Error& e) {
                    warnings.push_back(L"stream '" + m.streamName + L"': " + e.message());
                }
            } else if (m.available) {
                ok = util::path::copyFile(m.sourcePath, dst);
            }
            if (!ok)
                warnings.push_back(L"cannot export cabinet '" + m.cabinet + L"'");
            else {
                ++written;
                util::verboseLine(verbose, m.cabinet + L" -> " + dst);
            }
        }
        manifest += std::to_wstring(m.diskId) + L"\t" +
                    std::to_wstring(m.lastSequence) + L"\t" + m.cabinet + L"\t" + type +
                    L"\t" + std::to_wstring(m.size) + L"\t" + output + L"\n";
    }
    writeText(util::path::join(outDir, L"manifest.txt"), manifest);
    showWarnings(warnings);
    util::outLine(util::fmt(L"Exported %llu cabinet(s) from %llu media row(s) into %s",
                            static_cast<unsigned long long>(written),
                            static_cast<unsigned long long>(media.size()), outDir.c_str()));
}

void exportArchive(msi::Db& db, const std::wstring& outDir, bool verbose) {
    if (!util::path::makeDirs(outDir))
        util::fail(L"Cannot create output directory: " + outDir);
    const std::wstring tablesDir = util::path::join(outDir, L"tables");
    const std::wstring streamsDir = util::path::join(outDir, L"streams");
    const std::wstring binaryDir = util::path::join(outDir, L"binary");
    util::path::makeDirs(tablesDir);
    util::path::makeDirs(streamsDir);
    util::path::makeDirs(binaryDir);

    std::vector<std::wstring> warnings;
    size_t tables = 0, streams = 0, binaries = 0;

    for (const auto& t : db.tableNames()) {
        if (!db.isPersistent(t)) continue;
        UINT rc = db.exportTable(t, tablesDir + L"\\", t + L".idt");
        if (rc == ERROR_SUCCESS) {
            ++tables;
            util::verboseLine(verbose, L"table " + t);
        } else {
            warnings.push_back(L"export table '" + t + L"' failed: " + util::msiError(rc));
        }
    }

    for (const auto& name : db.streamNames()) {
        std::wstring rel = util::path::sanitizeRelative(name);
        if (rel.empty()) continue;
        try {
            auto data = db.readStream(L"SELECT `Data` FROM `_Streams` WHERE `Name`=?",
                                      {name}, 1);
            if (util::path::writeFile(util::path::join(streamsDir, rel), data)) {
                ++streams;
                util::verboseLine(verbose, L"stream " + name);
            } else {
                warnings.push_back(L"cannot write stream '" + name + L"'");
            }
        } catch (const util::Error& e) {
            warnings.push_back(L"stream '" + name + L"': " + e.message());
        }
    }

    if (db.tableExists(L"Binary")) {
        try {
            msi::QueryResult r = db.query(L"SELECT `Name` FROM `Binary` ORDER BY `Name`");
            for (const auto& row : r.rows) {
                if (row.empty() || row[0].empty()) continue;
                std::wstring name = row[0];
                auto data = db.readStream(L"SELECT `Data` FROM `Binary` WHERE `Name`=?",
                                          {name}, 1);
                std::wstring rel = util::path::sanitizeRelative(name);
                if (rel.empty()) continue;
                if (!util::path::ext(rel).size()) rel += L".bin";
                if (util::path::writeFile(util::path::join(binaryDir, rel), data)) {
                    ++binaries;
                    util::verboseLine(verbose, L"binary " + name);
                }
            }
        } catch (const util::Error& e) {
            warnings.push_back(L"Binary table: " + e.message());
        }
    }

    std::wstring text = L"# SummaryInformation (UTF-8)\n";
    for (const auto& item : db.summary())
        text += item.name + L" = " + item.value + L"\n";
    text += L"\n# Property table\n";
    try {
        msi::QueryResult r = db.query(L"SELECT `Property`, `Value` FROM `Property` ORDER BY `Property`");
        for (const auto& row : r.rows)
            text += (row.size() > 0 ? row[0] : L"") + L" = " +
                    (row.size() > 1 ? row[1] : L"") + L"\n";
    } catch (const util::Error&) {
    }
    writeText(util::path::join(outDir, L"summary.txt"), text);
    showWarnings(warnings);
    util::outLine(util::fmt(L"Exported %llu table(s), %llu stream(s), %llu binary item(s) into %s",
                            static_cast<unsigned long long>(tables),
                            static_cast<unsigned long long>(streams),
                            static_cast<unsigned long long>(binaries), outDir.c_str()));
}

}  // namespace extract
