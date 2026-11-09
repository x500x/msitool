#include "custom_actions.h"

#include "pack_util.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>

namespace pack {

namespace {

const CaType kTypes[] = {
    {L"dll",          L"DLL（Binary）",   1},
    {L"exe",          L"EXE（Binary）",   2},
    {L"dllfile",      L"DLL（文件）",     1 | 0x10},
    {L"exefile",      L"EXE（文件）",     2 | 0x10},
    {L"exedir",       L"EXE（目录）",     2 | 0x20},
    {L"setproperty",  L"设置属性 (51)",   3 | 0x30},
    {L"setdirectory", L"设置目录 (35)",   3 | 0x20},
    {L"runcommand",   L"执行命令 (50)",   2 | 0x30},
};

const CaPhase kPhases[] = {
    {L"不排入执行序列",                 0},
    {L"成本计算后 · 1100",          1100},
    {L"安装初始化后 · 1550",        1550},
    {L"文件复制前 · 3900",          3900},
    {L"文件复制后 · 4100",          4100},
    {L"注册表 / 快捷方式后 · 5300", 5300},
    {L"安装完成前 · 6450",          6450},
    {L"自定义序号",                  -1},
};

// Mirrors kSequence in pack.cpp. The editor uses it for the timeline and for
// "this number is already taken" hints; the packer remains authoritative.
const std::pair<const wchar_t*, int> kStandard[] = {
    {L"CostInitialize", 800}, {L"FileCost", 900}, {L"CostFinalize", 1000},
    {L"InstallValidate", 1400}, {L"InstallInitialize", 1500},
    {L"ProcessComponents", 1600}, {L"UnpublishFeatures", 1800},
    {L"StopServices", 1900}, {L"DeleteServices", 2000},
    {L"RemoveRegistryValues", 2600}, {L"RemoveShortcuts", 3500},
    {L"RemoveFiles", 3500}, {L"CreateFolders", 3700},
    {L"InstallFiles", 4000}, {L"CreateShortcuts", 4500},
    {L"WriteRegistryValues", 5000}, {L"WriteEnvironmentStrings", 5200},
    {L"InstallServices", 5800}, {L"StartServices", 5900},
    {L"RegisterProduct", 6100}, {L"PublishFeatures", 6300},
    {L"PublishProduct", 6400}, {L"InstallExecute", 6500},
    {L"InstallFinalize", 6600},
};

std::wstring joinPath(const std::wstring& base, const std::wstring& rel) {
    if (rel.empty()) return base;
    bool absolute = (rel.size() >= 2 && rel[1] == L':') ||
                    (rel.size() >= 2 && rel[0] == L'\\' && rel[1] == L'\\') ||
                    (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/'));
    if (absolute) return util::path::full(rel);
    return util::path::full(util::path::join(base, rel));
}

int strictInt(const std::wstring& text, const std::wstring& context) {
    std::wstring s = util::trim(text);
    if (s.empty()) util::fail(L"缺少数字（" + context + L"）");
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0')
        util::fail(L"不是合法数字：'" + text + L"'（" + context + L"）");
    return static_cast<int>(v);
}

void checkAttrs(const xml::Node& n, const std::wstring& context) {
    static const wchar_t* allowed[] = {
        L"id", L"type", L"binary", L"file", L"entry", L"target", L"args",
        L"property", L"directory", L"value", L"source", L"impersonate",
        L"noimpersonate", L"deferred", L"commit", L"rollback", L"async",
        L"continue", L"taware", L"x64", L"hidetarget"};
    for (const auto& a : n.attrs) {
        bool ok = false;
        for (const wchar_t* x : allowed)
            if (util::iequals(a.first, x)) { ok = true; break; }
        if (!ok)
            util::fail(L"<" + n.name + L"> 有未知属性 '" + a.first +
                       L"'（" + context + L"）");
    }
}

int baseTypeFor(const std::wstring& type, const std::wstring& id) {
    std::wstring t = util::toLower(util::trim(type));
    for (const CaType& k : kTypes)
        if (t == k.id) return k.base;
    if (t.empty() || t.find_first_not_of(L"0123456789") != std::wstring::npos)
        util::fail(L"customaction " + id + L": unknown type '" + t + L"'");
    int raw = strictInt(t, L"customaction " + id + L" type");
    if (raw <= 0 || raw > 0x7fff)
        util::fail(L"customaction " + id + L": type 超出 1..32767");
    return raw;
}

std::wstring xmlEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size() + 8);
    for (wchar_t c : s) {
        switch (c) {
        case L'&':  out += L"&amp;"; break;
        case L'<':  out += L"&lt;"; break;
        case L'>':  out += L"&gt;"; break;
        case L'"':  out += L"&quot;"; break;
        case L'\'': out += L"&apos;"; break;
        case L'\r': out += L"&#13;"; break;
        case L'\n': out += L"&#10;"; break;
        case L'\t': out += L"&#9;"; break;
        default:    out += c; break;
        }
    }
    return out;
}

void attr(std::wstring& line, const wchar_t* name, const std::wstring& value) {
    if (value.empty()) return;
    line += L" ";
    line += name;
    line += L"=\"";
    line += xmlEscape(value);
    line += L"\"";
}

void flag(std::wstring& line, const wchar_t* name, bool on) {
    if (on) attr(line, name, L"true");
}

std::wstring actionLine(const CustomActionSpec& s) {
    std::wstring line = L"  <customaction";
    attr(line, L"id", s.id);
    attr(line, L"type", s.type);
    const int mask = caFieldMask(s.type);
    if (mask & CaBinary)    attr(line, L"binary", s.binary);
    if (mask & CaFile)      attr(line, L"file", s.file);
    if (mask & CaEntry)     attr(line, L"entry", s.entry);
    if (mask & CaDirectory) attr(line, L"directory", s.directory);
    if (mask & CaProperty)  attr(line, L"property", s.property);
    if (mask & CaValue)     attr(line, L"value", s.value);
    if (mask & CaTarget) {
        attr(line, L"target", s.target.empty() ? s.args : s.target);
    }
    flag(line, L"deferred", s.deferred);
    flag(line, L"commit", s.commit);
    flag(line, L"rollback", s.rollback);
    flag(line, L"async", s.asyncRun);
    flag(line, L"continue", s.continueOnError);
    attr(line, L"impersonate", s.noImpersonate ? L"false" : L"");
    flag(line, L"taware", s.taware);
    flag(line, L"x64", s.x64);
    flag(line, L"hidetarget", s.hideTarget);
    line += L"/>";
    return line;
}

}  // namespace

const CaType* caTypes() { return kTypes; }
int caTypeCount() { return static_cast<int>(sizeof(kTypes) / sizeof(kTypes[0])); }
const CaPhase* caPhases() { return kPhases; }
int caPhaseCount() { return static_cast<int>(sizeof(kPhases) / sizeof(kPhases[0])); }

std::wstring caTypeLabel(const std::wstring& type) {
    std::wstring t = util::toLower(util::trim(type));
    for (const CaType& k : kTypes)
        if (t == k.id) return k.label;
    if (!t.empty() && t.find_first_not_of(L"0123456789") == std::wstring::npos)
        return L"原始类型 " + t;
    return t.empty() ? L"（未设置）" : type;
}

int caFieldMask(const std::wstring& type) {
    std::wstring t = util::toLower(util::trim(type));
    if (t == L"dll")          return CaBinary | CaEntry | CaTarget;
    if (t == L"exe")          return CaBinary | CaTarget;
    if (t == L"dllfile")      return CaFile | CaEntry | CaTarget;
    if (t == L"exefile")      return CaFile | CaTarget;
    if (t == L"exedir")       return CaDirectory | CaTarget;
    if (t == L"setproperty")  return CaProperty | CaValue;
    if (t == L"setdirectory") return CaDirectory | CaValue;
    if (t == L"runcommand")   return CaProperty | CaTarget;
    // Raw numeric type: show every source field, the user knows what it means.
    if (!t.empty() && t.find_first_not_of(L"0123456789") == std::wstring::npos) {
        const int raw = static_cast<int>(wcstol(t.c_str(), nullptr, 10));
        int mask = CaTarget;
        switch (raw & 0x30) {
        case 0x00: mask |= CaBinary | CaEntry; break;
        case 0x10: mask |= CaFile; break;
        case 0x20: mask |= CaDirectory | CaValue; break;
        case 0x30: mask |= CaProperty | CaValue; break;
        default: break;
        }
        return mask;
    }
    return CaTarget;
}

std::vector<std::pair<std::wstring, int>> standardSequence() {
    std::vector<std::pair<std::wstring, int>> out;
    for (const auto& s : kStandard) out.emplace_back(s.first, s.second);
    return out;
}

CustomActionSpec parseCustomActionSpec(const xml::Node& n) {
    checkAttrs(n, L"customaction");
    CustomActionSpec s;
    s.id = n.require(L"id", L"customaction");
    s.type = util::toLower(util::trim(n.attr(L"type", L"exe")));
    s.binary = n.attr(L"binary");
    s.file = n.attr(L"file");
    s.entry = n.attr(L"entry");
    s.target = n.attr(L"target");
    s.args = n.attr(L"args");
    s.property = n.attr(L"property");
    s.directory = n.attr(L"directory");
    s.value = n.attr(L"value");
    s.source = n.attr(L"source");
    auto boolean = [&n](const wchar_t* name) {
        return n.has(name) && parseBool(n.attr(name), false);
    };
    s.deferred = boolean(L"deferred");
    s.commit = boolean(L"commit");
    s.rollback = boolean(L"rollback");
    s.asyncRun = boolean(L"async");
    s.continueOnError = boolean(L"continue");
    s.taware = boolean(L"taware");
    s.x64 = boolean(L"x64");
    s.hideTarget = boolean(L"hidetarget");
    s.noImpersonate = boolean(L"noimpersonate");
    if (n.has(L"impersonate")) s.noImpersonate = !parseBool(n.attr(L"impersonate"), true);
    // Normalise the legacy aliases into the type-specific fields, so the GUI
    // editor always has one canonical place to show / edit each value.
    if (!s.args.empty() && s.target.empty()) s.target = s.args;
    s.args.clear();
    if (!s.source.empty()) {
        int srcKind = -1;
        std::wstring t = util::toLower(util::trim(s.type));
        for (const CaType& k : kTypes)
            if (t == k.id) { srcKind = k.base & 0x30; break; }
        if (srcKind < 0 && !t.empty() &&
            t.find_first_not_of(L"0123456789") == std::wstring::npos)
            srcKind = static_cast<int>(wcstol(t.c_str(), nullptr, 10)) & 0x30;
        if (srcKind == 0x10 && s.file.empty()) s.file = s.source;
        else if (srcKind == 0x20 && s.directory.empty()) s.directory = s.source;
        else if (srcKind == 0x30 && s.property.empty()) s.property = s.source;
        s.source.clear();
    }
    return s;
}

CustomActionDef compileCustomAction(const CustomActionSpec& spec,
                                    const std::wstring& baseDir) {
    CustomActionDef d;
    d.id = toIdentifier(spec.id);
    if (d.id.empty())
        util::fail(L"自定义动作需要一个 id");

    int type = baseTypeFor(spec.type, d.id);
    if (spec.deferred)        type |= 0x400;
    if (spec.commit)          type |= 0x200;
    if (spec.rollback)        type |= 0x100;
    if (spec.asyncRun)        type |= 0x80;
    if (spec.continueOnError) type |= 0x40;
    if (spec.taware)          type |= 0x4000;
    if (spec.x64)             type |= 0x1000;
    if (spec.hideTarget)      type |= 0x2000;
    if (spec.noImpersonate)   type |= 0x800;
    d.type = type;

    const int execType = type & 0x0f;
    const int srcKind = type & 0x30;
    d.target = spec.target.empty() ? spec.args : spec.target;

    if (srcKind == 0x00) {                       // BinaryData
        if (spec.binary.empty())
            util::fail(L"customaction " + d.id + L" 需要 binary=\"...\"");
        d.source = toIdentifier(d.id + L"Binary");
        d.binaryFile = joinPath(baseDir, spec.binary);
        d.hasBinary = true;
        if (execType == 1 && !spec.entry.empty()) d.target = spec.entry;
    } else if (srcKind == 0x10) {                // installed File key
        d.source = spec.file.empty() ? spec.source : spec.file;
        if (d.source.empty())
            util::fail(L"customaction " + d.id + L" 需要 file=\"...\"");
    } else if (srcKind == 0x20) {                // Directory key
        d.source = spec.directory.empty() ? spec.source : spec.directory;
        if (execType == 3 && !spec.value.empty()) d.target = spec.value;
        if (d.source.empty())
            util::fail(L"customaction " + d.id + L" 需要 directory=\"...\"");
    } else if (srcKind == 0x30) {                // Property
        d.source = spec.property.empty() ? spec.source : spec.property;
        if (execType == 3 && !spec.value.empty()) d.target = spec.value;
        if (d.source.empty())
            util::fail(L"customaction " + d.id + L" 需要 property=\"...\"");
    }
    return d;
}

std::vector<CustomActionSpec> loadCustomActionSpecs(const std::wstring& path) {
    xml::Node root = xml::parseFile(util::path::full(path));
    std::vector<CustomActionSpec> rows;
    std::vector<SequenceEntry> sequence;
    for (const xml::Node& ch : root.children) {
        if (util::iequals(ch.name, L"customactions")) {
            for (const xml::Node* c : ch.childList(L"customaction"))
                rows.push_back(parseCustomActionSpec(*c));
        } else if (util::iequals(ch.name, L"executesequence")) {
            for (const xml::Node* a : ch.childList(L"action")) {
                SequenceEntry e;
                e.action = a->require(L"name", L"executeSequence action");
                if (a->has(L"value"))
                    e.sequence = strictInt(a->attr(L"value"), L"executeSequence");
                else if (a->has(L"sequence"))
                    e.sequence = strictInt(a->attr(L"sequence"), L"executeSequence");
                if (a->has(L"condition")) {
                    e.condition = a->attr(L"condition");
                    e.hasCondition = true;
                }
                sequence.push_back(std::move(e));
            }
        }
    }
    // Attach sequence rows to the actions they schedule. Other rows (standard
    // actions) stay in the manifest and are written by the packer as-is.
    for (const SequenceEntry& e : sequence) {
        std::wstring key = util::toLower(toIdentifier(e.action));
        for (CustomActionSpec& s : rows) {
            if (util::toLower(toIdentifier(s.id)) != key) continue;
            s.sequence = e.sequence;
            if (e.hasCondition) s.condition = e.condition;
            break;
        }
    }
    return rows;
}

std::vector<CaIssue> lintCustomActions(const std::vector<CustomActionSpec>& rows,
                                       const std::wstring& baseDir) {
    std::vector<CaIssue> out;
    std::map<std::wstring, int> byId;
    std::map<int, int> bySeq;
    for (size_t i = 0; i < rows.size(); ++i) {
        const CustomActionSpec& s = rows[i];
        const int row = static_cast<int>(i);
        auto add = [&](bool error, const std::wstring& text) {
            out.push_back({row, error, text});
        };
        std::wstring id = toIdentifier(s.id);
        if (id.empty()) { add(true, L"缺少动作 ID"); continue; }
        if (id != s.id)
            add(false, L"ID 会被规范化为 " + id);
        if (byId.count(util::toLower(id)))
            add(true, L"动作 ID 与第 " +
                      std::to_wstring(byId[util::toLower(id)] + 1) + L" 行重复");
        else byId[util::toLower(id)] = row;

        const int mask = caFieldMask(s.type);
        if ((mask & CaBinary) && s.binary.empty())
            add(true, L"内嵌类型需要选择 Binary 文件");
        else if ((mask & CaBinary) && !s.binary.empty()) {
            std::wstring p = joinPath(baseDir, s.binary);
            if (!util::path::exists(p))
                add(false, L"Binary 文件不存在：" + p);
        }
        if ((mask & CaFile) && s.file.empty()) add(true, L"需要填写已安装文件键");
        if ((mask & CaDirectory) && s.directory.empty()) add(true, L"需要填写目录键");
        if ((mask & CaProperty) && s.property.empty()) add(true, L"需要填写属性名");
        if ((mask & CaValue) && s.value.empty()) add(true, L"需要填写值");
        if ((mask & CaTarget) && s.target.empty() && s.args.empty()) {
            // EXE-style actions may legitimately have an empty command line,
            // but runcommand / exedir cannot do anything without one.
            std::wstring t = util::toLower(s.type);
            if (t == L"runcommand" || t == L"exedir")
                add(true, L"需要填写命令行 Target");
        }
        if ((s.type == L"dll" || s.type == L"dllfile") && s.entry.empty())
            add(true, L"DLL 动作需要填写入口点");

        if (s.sequence < 0 || s.sequence > 32767)
            add(true, L"序号必须在 0..32767 之间");
        if (s.sequence > 0) {
            if (bySeq.count(s.sequence))
                add(false, L"与第 " + std::to_wstring(bySeq[s.sequence] + 1) +
                           L" 行使用相同序号，执行顺序不确定");
            else bySeq[s.sequence] = row;
            for (const auto& a : kStandard)
                if (a.second == s.sequence) {
                    add(false, L"与标准动作 " + std::wstring(a.first) +
                               L" 的序号相同，执行顺序不确定");
                    break;
                }
        } else if (s.deferred || s.commit || s.rollback) {
            add(false, L"延迟 / 提交 / 回滚动作需要排入执行序列才会运行");
        }
    }
    return out;
}

std::wstring customActionsToXml(const std::vector<CustomActionSpec>& rows,
                                bool document) {
    std::wstring out;
    if (document) {
        out += L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n";
        out += L"<!-- 由 msitool 自定义动作编辑器生成；可作为 pack 的 config 清单使用 -->\r\n";
        out += L"<msitool xmlns=\"https://msitool.dev/schema/pack/v1\" "
               L"minVersion=\"1\">\r\n";
    }
    out += L"<customactions>\r\n";
    for (const auto& s : rows) out += actionLine(s) + L"\r\n";
    out += L"</customactions>\r\n";

    bool anyScheduled = false;
    for (const auto& s : rows) if (s.sequence > 0) { anyScheduled = true; break; }
    if (anyScheduled) {
        out += L"\r\n<executeSequence>\r\n";
        std::vector<const CustomActionSpec*> sorted;
        for (const auto& s : rows) if (s.sequence > 0) sorted.push_back(&s);
        std::stable_sort(sorted.begin(), sorted.end(),
                         [](const CustomActionSpec* a, const CustomActionSpec* b) {
                             return a->sequence < b->sequence;
                         });
        for (const CustomActionSpec* s : sorted) {
            std::wstring line = L"  <action";
            attr(line, L"name", toIdentifier(s->id));
            attr(line, L"value", std::to_wstring(s->sequence));
            attr(line, L"condition", s->condition);
            line += L"/>";
            out += line + L"\r\n";
        }
        out += L"</executeSequence>\r\n";
    }
    if (document) out += L"</msitool>\r\n";
    return out;
}

}  // namespace pack
