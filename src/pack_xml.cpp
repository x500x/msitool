#include "pack_xml.h"

#include "custom_actions.h"
#include "msidb.h"
#include "pack_util.h"

#include <algorithm>
#include <cwchar>
#include <map>
#include <set>

namespace pack {

namespace {

const wchar_t* kNamespace = L"https://msitool.dev/schema/pack/v1";

std::vector<unsigned char> readBytes(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        util::fail(L"Cannot open file: " + path + L": " + util::winError(GetLastError()));
    DWORD hi = 0, size = GetFileSize(h, &hi);
    if (hi || size > 256u * 1024u * 1024u) {
        CloseHandle(h);
        util::fail(L"File is too large (limit 256 MiB): " + path);
    }
    std::vector<unsigned char> buf(size + 1, 0);
    DWORD got = 0;
    if (size && !ReadFile(h, buf.data(), size, &got, nullptr)) {
        CloseHandle(h);
        util::fail(L"Cannot read file: " + path);
    }
    CloseHandle(h);
    buf.resize(got);
    return buf;
}

std::wstring decodeText(const std::vector<unsigned char>& buf, const std::wstring& path) {
    (void)path;
    if (buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF)
        return util::widen(std::string(buf.begin() + 3, buf.end()), CP_UTF8);
    if (buf.size() >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) {
        std::wstring s;
        for (size_t i = 2; i + 1 < buf.size(); i += 2)
            s.push_back(static_cast<wchar_t>(buf[i] | (buf[i + 1] << 8)));
        return s;
    }
    if (buf.size() >= 2 && buf[0] == 0xFE && buf[1] == 0xFF) {
        std::wstring s;
        for (size_t i = 2; i + 1 < buf.size(); i += 2)
            s.push_back(static_cast<wchar_t>((buf[i] << 8) | buf[i + 1]));
        return s;
    }
    return util::widen(std::string(buf.begin(), buf.end()), CP_UTF8);
}

std::wstring readText(const std::wstring& path) {
    return decodeText(readBytes(path), path);
}

int parseIntStrict(const std::wstring& text, const std::wstring& context) {
    std::wstring s = util::trim(text);
    if (s.empty()) util::fail(L"Missing number (" + context + L")");
    wchar_t* end = nullptr;
    long v = wcstol(s.c_str(), &end, 10);
    if (!end || *end != L'\0')
        util::fail(L"Not a number: '" + text + L"' (" + context + L")");
    return static_cast<int>(v);
}

int intAttr(const xml::Node& n, const wchar_t* name, int deflt,
            const std::wstring& context) {
    if (!n.has(name)) return deflt;
    return parseIntStrict(n.attr(name), context + L", attribute " + name);
}

bool boolAttr(const xml::Node& n, const wchar_t* name, bool deflt) {
    if (!n.has(name)) return deflt;
    return parseBool(n.attr(name), deflt);
}

void checkAttrs(const xml::Node& n, std::initializer_list<const wchar_t*> allowed,
                const std::wstring& context) {
    for (const auto& a : n.attrs) {
        bool ok = false;
        for (const wchar_t* x : allowed)
            if (util::iequals(a.first, x)) { ok = true; break; }
        if (!ok)
            util::fail(L"<" + n.name + L"> has an unknown attribute '" + a.first +
                       L"' (" + context + L")");
    }
}

std::wstring joinPath(const std::wstring& base, const std::wstring& rel) {
    if (rel.empty()) return base;
    bool absolute = (rel.size() >= 2 && rel[1] == L':') ||
                    (rel.size() >= 2 && rel[0] == L'\\' && rel[1] == L'\\') ||
                    (!rel.empty() && (rel[0] == L'\\' || rel[0] == L'/'));
    if (absolute) return util::path::full(rel);
    return util::path::full(util::path::join(base, rel));
}

void parsePack(const xml::Node& n, Manifest& m) {
    const std::wstring ctx = L"pack";
    checkAttrs(n, {L"name", L"version", L"manufacturer", L"productCode",
                   L"upgradeCode", L"platform", L"language", L"installDir",
                   L"scope", L"ui", L"cabSize", L"largeFiles", L"keepEmptyDirs"},
               ctx);
    m.pack.name = n.attr(L"name");
    m.pack.version = n.attr(L"version");
    m.pack.manufacturer = n.attr(L"manufacturer");
    m.pack.productCode = n.attr(L"productCode");
    m.pack.upgradeCode = n.attr(L"upgradeCode");
    m.pack.platform = n.attr(L"platform");
    m.pack.installDir = n.attr(L"installDir");
    m.pack.scope = n.attr(L"scope");
    m.pack.ui = n.attr(L"ui");
    m.pack.largeFiles = n.attr(L"largeFiles");
    if (n.has(L"language")) {
        m.pack.language = intAttr(n, L"language", 1033, ctx);
        m.pack.hasLanguage = true;
    }
    if (n.has(L"cabSize")) {
        m.pack.cabSize = parseSize(n.attr(L"cabSize"));
        m.pack.hasCabSize = true;
        if (m.pack.cabSize == 0)
            util::fail(L"<pack cabSize=\"...\"> is not a valid size");
    }
    if (n.has(L"keepEmptyDirs"))
        m.pack.keepEmptyDirs = boolAttr(n, L"keepEmptyDirs", true);
}

void parseFeatures(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"features");
    for (const xml::Node* f : n.childList(L"feature")) {
        checkAttrs(*f, {L"id", L"title", L"description", L"dirs", L"level",
                        L"display", L"attributes"}, L"feature");
        FeatureDef d;
        d.id = toIdentifier(f->attr(L"id", L"Feature"), 38);
        d.title = f->attr(L"title", d.id);
        d.description = f->attr(L"description", d.title);
        d.dirs = f->attr(L"dirs");
        d.level = intAttr(*f, L"level", 1, L"feature " + d.id);
        d.display = intAttr(*f, L"display", 0, L"feature " + d.id);
        d.attributes = intAttr(*f, L"attributes", 0, L"feature " + d.id);
        m.features.push_back(std::move(d));
    }
}

void parseShortcuts(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"shortcuts");
    for (const xml::Node* s : n.childList(L"shortcut")) {
        checkAttrs(*s, {L"id", L"name", L"directory", L"dir", L"target",
                        L"arguments", L"args", L"description", L"icon",
                        L"iconIndex", L"workingDirectory", L"wkdir",
                        L"component", L"advertise"}, L"shortcut");
        ShortcutDef d;
        d.id = toIdentifier(s->attr(L"id", L"Shortcut"));
        d.name = s->attr(L"name", d.id + L".lnk");
        d.dir = s->attr(L"directory", s->attr(L"dir", L"ProgramMenuFolder"));
        d.target = s->attr(L"target");
        d.args = s->attr(L"arguments", s->attr(L"args"));
        d.description = s->attr(L"description");
        d.icon = s->attr(L"icon");
        d.iconIndex = intAttr(*s, L"iconIndex", 0, L"shortcut " + d.id);
        d.wkdir = s->attr(L"workingDirectory", s->attr(L"wkdir"));
        d.component = s->attr(L"component");
        d.advertise = boolAttr(*s, L"advertise", false);
        for (const xml::Node* p : s->childList(L"msishortcutproperty")) {
            checkAttrs(*p, {L"key", L"value"}, L"msishortcutproperty");
            ShortcutPropertyDef sp;
            sp.key = p->require(L"key", L"msishortcutproperty");
            sp.value = p->attr(L"value");
            d.properties.push_back(std::move(sp));
        }
        m.shortcuts.push_back(std::move(d));
    }
}

void parseRegistry(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"registry");
    for (const xml::Node* r : n.childList(L"registryvalue")) {
        checkAttrs(*r, {L"id", L"root", L"key", L"name", L"value", L"type",
                        L"component"}, L"registryValue");
        RegistryDef d;
        d.id = toIdentifier(r->attr(L"id", L"Reg"));
        d.root = r->attr(L"root", L"auto");
        d.key = r->attr(L"key");
        d.name = r->attr(L"name");
        d.value = r->attr(L"value");
        d.type = util::toLower(r->attr(L"type", L"string"));
        d.component = r->attr(L"component");
        if (d.key.empty())
            util::fail(L"<registryValue> " + d.id + L" needs a key");
        m.registry.push_back(std::move(d));
    }
}

void parseServices(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"services");
    for (const xml::Node* s : n.childList(L"service")) {
        checkAttrs(*s, {L"id", L"name", L"displayName", L"image", L"type",
                        L"start", L"error", L"account", L"password",
                        L"arguments", L"dependencies", L"description",
                        L"component", L"control", L"controlId",
                        L"controlEvent", L"controlWait", L"controlArguments"},
                   L"service");
        ServiceDef d;
        d.id = toIdentifier(s->attr(L"id", L"Service"));
        d.name = s->attr(L"name", d.id);
        d.displayName = s->attr(L"displayName", d.name);
        d.image = s->attr(L"image");
        d.type = util::toLower(s->attr(L"type", L"own"));
        d.start = util::toLower(s->attr(L"start", L"demand"));
        d.error = util::toLower(s->attr(L"error", L"normal"));
        d.account = s->attr(L"account");
        d.password = s->attr(L"password");
        d.args = s->attr(L"arguments");
        d.dependencies = s->attr(L"dependencies");
        d.description = s->attr(L"description");
        d.component = s->attr(L"component");
        d.control = util::toLower(s->attr(L"control", L"default"));
        if (d.control != L"none" && d.control != L"default")
            util::fail(L"<service> " + d.id + L": control must be default or none");
        d.controlId = s->attr(L"controlId");
        d.controlArguments = s->attr(L"controlArguments");
        d.controlEvent = intAttr(*s, L"controlEvent", 163, L"service " + d.id);
        d.controlWait = intAttr(*s, L"controlWait", 1, L"service " + d.id);
        m.services.push_back(std::move(d));
    }
}

void parseServiceControls(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"servicecontrols");
    for (const xml::Node* c : n.childList(L"servicecontrol")) {
        checkAttrs(*c, {L"id", L"name", L"event", L"arguments", L"wait",
                        L"component"}, L"servicecontrol");
        ServiceControlDef d;
        d.id = toIdentifier(c->attr(L"id", L"ServiceControl"), 38);
        d.name = c->attr(L"name", d.id);
        d.event = intAttr(*c, L"event", 163, L"servicecontrol " + d.id);
        d.arguments = c->attr(L"arguments");
        d.wait = intAttr(*c, L"wait", 1, L"servicecontrol " + d.id);
        d.component = c->attr(L"component");
        m.serviceControls.push_back(std::move(d));
    }
}

void parseProperties(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"properties");
    for (const xml::Node* p : n.childList(L"property")) {
        checkAttrs(*p, {L"name", L"value"}, L"property");
        PropertyDef d;
        d.name = p->require(L"name", L"property");
        d.value = p->attr(L"value");
        if (!d.name.empty()) m.properties.push_back(std::move(d));
    }
}

void parseEnvironment(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"environment");
    for (const xml::Node* e : n.childList(L"variable")) {
        checkAttrs(*e, {L"id", L"name", L"value", L"action", L"separator",
                        L"system", L"component"}, L"environment variable");
        EnvDef d;
        d.id = toIdentifier(e->attr(L"id", L"Env"));
        d.name = e->attr(L"name", d.id);
        d.value = e->attr(L"value");
        d.action = util::toLower(e->attr(L"action", L"set"));
        d.separator = e->attr(L"separator", L";");
        d.system = e->attr(L"system", L"no");
        d.component = e->attr(L"component");
        m.environment.push_back(std::move(d));
    }
}

// The XML loader and the GUI editor share one high-level model: parse into
// CustomActionSpec, then encode it into the MSI CustomAction row.
void parseCustomActions(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {}, L"customactions");
    for (const xml::Node* c : n.childList(L"customaction")) {
        CustomActionSpec spec = parseCustomActionSpec(*c);
        m.customActions.push_back(compileCustomAction(spec, m.dir));
    }
}

void parseSign(const xml::Node& n, Manifest& m) {
    checkAttrs(n, {L"cert", L"password", L"timestamp", L"tool"}, L"sign");
    m.sign.present = !m.sign.cert.empty();
    m.sign.cert = n.attr(L"cert");
    m.sign.password = n.attr(L"password");
    m.sign.timestamp = n.attr(L"timestamp");
    m.sign.tool = n.attr(L"tool");
    if (!m.sign.cert.empty()) m.sign.cert = joinPath(m.dir, m.sign.cert);
}

// ---------------------------------------------------------------------------
// UI elements
// ---------------------------------------------------------------------------

TextStyleDef parseTextStyle(const xml::Node& n, const LocaleStrings& strings) {
    checkAttrs(n, {L"id", L"face", L"size", L"color", L"style"}, L"textstyle");
    TextStyleDef s;
    s.id = n.require(L"id", L"textstyle");
    s.face = applyLocaleStrings(n.require(L"face", L"textstyle " + s.id), strings,
                                L"textstyle " + s.id);
    s.size = parseIntStrict(applyLocaleStrings(n.attr(L"size", L"8"), strings,
                                               L"textstyle " + s.id),
                            L"textstyle " + s.id);
    if (n.has(L"color")) { s.color = intAttr(n, L"color", 0, L"textstyle " + s.id); s.hasColor = true; }
    if (n.has(L"style")) { s.style = intAttr(n, L"style", 0, L"textstyle " + s.id); s.hasStyle = true; }
    if (s.size <= 0) util::fail(L"textstyle " + s.id + L" has an invalid size");
    return s;
}

BinaryDef parseBinary(const xml::Node& n, const std::wstring& baseDir) {
    checkAttrs(n, {L"id", L"file"}, L"binary");
    BinaryDef b;
    b.id = n.require(L"id", L"binary");
    std::wstring file = n.require(L"file", L"binary " + b.id);
    std::wstring full = joinPath(baseDir, file);
    b.data = readBytes(full);
    return b;
}

PublishDef parsePublish(const xml::Node& n, const LocaleStrings& strings) {
    checkAttrs(n, {L"event", L"value", L"argument", L"condition", L"order"},
               L"publish");
    PublishDef p;
    p.event = n.attr(L"event");
    p.value = n.attr(L"value", n.attr(L"argument"));
    p.condition = n.attr(L"condition", L"1");
    if (n.has(L"order")) { p.order = intAttr(n, L"order", 0, L"publish"); p.hasOrder = true; }
    if (p.event.empty()) util::fail(L"<publish> needs an event");
    (void)strings;
    return p;
}

PublishDef parseSetProperty(const xml::Node& n, const LocaleStrings& strings) {
    checkAttrs(n, {L"name", L"value", L"condition", L"order"}, L"setproperty");
    PublishDef p;
    std::wstring name = n.require(L"name", L"setproperty");
    p.event = L"[" + name + L"]";
    p.value = applyLocaleStrings(n.attr(L"value"), strings, L"setproperty " + name);
    p.condition = n.attr(L"condition", L"1");
    if (n.has(L"order")) { p.order = intAttr(n, L"order", 0, L"setproperty"); p.hasOrder = true; }
    return p;
}

ListItemDef parseItem(const xml::Node& n, const LocaleStrings& strings) {
    checkAttrs(n, {L"value", L"text", L"help"}, L"item");
    ListItemDef item;
    item.value = n.require(L"value", L"item");
    item.text = applyLocaleStrings(n.attr(L"text", item.value), strings, L"item " + item.value);
    item.help = applyLocaleStrings(n.attr(L"help"), strings, L"item " + item.value);
    return item;
}

ControlDef parseControl(const xml::Node& n, const LocaleStrings& strings,
                        const std::wstring& baseDir) {
    const std::wstring ctx = L"control " + n.attr(L"id");
    checkAttrs(n, {L"id", L"type", L"x", L"y", L"width", L"height", L"attributes",
                   L"visible", L"enabled", L"sunken", L"indirect", L"property",
                   L"text", L"textFile", L"binary", L"next", L"help"}, ctx);
    ControlDef c;
    c.id = n.require(L"id", L"control");
    c.type = n.require(L"type", L"control " + c.id);
    if (n.has(L"x")) { c.x = intAttr(n, L"x", 0, ctx); c.hasX = true; }
    if (n.has(L"y")) { c.y = intAttr(n, L"y", 0, ctx); c.hasY = true; }
    if (n.has(L"width")) { c.width = intAttr(n, L"width", 0, ctx); c.hasWidth = true; }
    if (n.has(L"height")) { c.height = intAttr(n, L"height", 0, ctx); c.hasHeight = true; }
    if (n.has(L"attributes")) {
        c.attributes = intAttr(n, L"attributes", 3, ctx);
        c.hasAttributes = true;
    }
    if (n.has(L"visible")) { c.visible = boolAttr(n, L"visible", true); c.hasVisible = true; }
    if (n.has(L"enabled")) { c.enabled = boolAttr(n, L"enabled", true); c.hasEnabled = true; }
    if (n.has(L"sunken")) { c.sunken = boolAttr(n, L"sunken", false); c.hasSunken = true; }
    if (n.has(L"indirect")) { c.indirect = boolAttr(n, L"indirect", false); c.hasIndirect = true; }
    if (n.has(L"property")) { c.property = n.attr(L"property"); c.hasProperty = true; }
    if (n.has(L"text")) {
        c.text = applyLocaleStrings(n.attr(L"text"), strings, ctx);
        c.hasText = true;
    } else if (n.has(L"textFile")) {
        std::wstring rel = n.attr(L"textFile");
        c.text = readText(joinPath(baseDir, rel));
        c.hasText = true;
    }
    if (n.has(L"binary")) { c.binary = n.attr(L"binary"); c.hasBinary = true; }
    if (n.has(L"next")) { c.next = n.attr(L"next"); c.hasNext = true; }
    if (n.has(L"help")) { c.help = n.attr(L"help"); c.hasHelp = true; }

    for (const auto& ch : n.children) {
        if (util::iequals(ch.name, L"publish")) {
            c.publish.push_back(parsePublish(ch, strings));
        } else if (util::iequals(ch.name, L"setproperty")) {
            c.publish.push_back(parseSetProperty(ch, strings));
        } else if (util::iequals(ch.name, L"controlcondition")) {
            checkAttrs(ch, {L"action", L"condition"}, L"controlcondition");
            ConditionDef cc;
            cc.action = ch.require(L"action", L"controlcondition");
            cc.condition = ch.require(L"condition", L"controlcondition");
            c.conditions.push_back(std::move(cc));
        } else if (util::iequals(ch.name, L"item")) {
            c.items.push_back(parseItem(ch, strings));
        } else {
            util::fail(L"<control> contains an unknown element <" + ch.name + L">");
        }
    }
    return c;
}

DialogDef parseDialog(const xml::Node& n, const LocaleStrings& strings,
                      const std::wstring& baseDir) {
    const std::wstring ctx = L"dialog " + n.attr(L"id");
    checkAttrs(n, {L"id", L"title", L"width", L"height", L"attributes",
                   L"modeless", L"first", L"default", L"cancel", L"sequence",
                   L"condition"}, ctx);
    DialogDef d;
    d.id = n.require(L"id", L"dialog");
    if (n.has(L"title")) {
        d.title = applyLocaleStrings(n.attr(L"title"), strings, ctx);
        d.hasTitle = true;
    }
    d.width = intAttr(n, L"width", 370, ctx);
    d.height = intAttr(n, L"height", 270, ctx);
    d.hasWidth = n.has(L"width");
    d.hasHeight = n.has(L"height");
    if (n.has(L"attributes")) {
        d.attributes = intAttr(n, L"attributes", 7, ctx);
        d.hasAttributes = true;
    } else if (boolAttr(n, L"modeless", false)) {
        d.attributes = 5;   // Visible | Minimize
        d.hasAttributes = true;
    }
    if (n.has(L"first")) { d.first = n.attr(L"first"); d.hasFirst = true; }
    if (n.has(L"default")) { d.def = n.attr(L"default"); d.hasDefault = true; }
    if (n.has(L"cancel")) { d.cancel = n.attr(L"cancel"); d.hasCancel = true; }
    if (n.has(L"sequence")) {
        d.sequence = intAttr(n, L"sequence", 0, ctx);
        d.hasSequence = true;
    }
    if (n.has(L"condition")) {
        d.condition = n.attr(L"condition");
        d.hasCondition = true;
    }
    for (const xml::Node* c : n.childList(L"control"))
        d.controls.push_back(parseControl(*c, strings, baseDir));
    for (const xml::Node& ch : n.children) {
        if (!util::iequals(ch.name, L"control"))
            util::fail(L"<dialog> contains an unknown element <" + ch.name + L">");
    }
    return d;
}

ErrorDef parseError(const xml::Node& n, const LocaleStrings& strings) {
    checkAttrs(n, {L"id", L"text"}, L"error");
    ErrorDef e;
    e.id = intAttr(n, L"id", 0, L"error");
    if (e.id <= 0) util::fail(L"<error> needs a positive id");
    e.text = applyLocaleStrings(n.attr(L"text"), strings, L"error");
    return e;
}

BillboardControlDef parseBillboardControl(const xml::Node& n,
                                          const LocaleStrings& strings) {
    checkAttrs(n, {L"id", L"type", L"x", L"y", L"width", L"height",
                   L"attributes", L"text"}, L"billboard control");
    BillboardControlDef c;
    c.id = n.require(L"id", L"billboard control");
    c.type = n.require(L"type", L"billboard control");
    c.x = intAttr(n, L"x", 0, L"billboard control");
    c.y = intAttr(n, L"y", 0, L"billboard control");
    c.width = intAttr(n, L"width", 0, L"billboard control");
    c.height = intAttr(n, L"height", 0, L"billboard control");
    if (n.has(L"attributes")) {
        c.attributes = intAttr(n, L"attributes", 1, L"billboard control");
        c.hasAttributes = true;
    }
    c.text = applyLocaleStrings(n.attr(L"text"), strings, L"billboard control");
    return c;
}

BillboardDef parseBillboard(const xml::Node& n, const LocaleStrings& strings,
                            const std::wstring& action) {
    checkAttrs(n, {L"id", L"feature", L"action", L"ordering"}, L"billboard");
    BillboardDef b;
    b.id = n.require(L"id", L"billboard");
    b.feature = n.attr(L"feature");
    b.action = n.attr(L"action", action);
    b.ordering = intAttr(n, L"ordering", 1, L"billboard " + b.id);
    for (const xml::Node* c : n.childList(L"control"))
        b.controls.push_back(parseBillboardControl(*c, strings));
    return b;
}

void parseSequence(const xml::Node& n, std::vector<SequenceEntry>& out,
                   const std::wstring& context) {
    checkAttrs(n, {}, context);
    for (const xml::Node* a : n.childList(L"action")) {
        checkAttrs(*a, {L"name", L"value", L"sequence", L"condition"}, context + L" action");
        SequenceEntry e;
        e.action = a->require(L"name", context);
        int value = intAttr(*a, L"value", 0, context);
        if (!a->has(L"value") && a->has(L"sequence"))
            value = intAttr(*a, L"sequence", 0, context);
        if (!a->has(L"value") && !a->has(L"sequence"))
            util::fail(context + L": action " + e.action + L" needs a value");
        e.sequence = value;
        if (a->has(L"condition")) {
            e.condition = a->attr(L"condition");
            e.hasCondition = true;
        }
        out.push_back(std::move(e));
    }
}

void parseUi(const xml::Node& n, UiSpec& spec, const LocaleStrings& outer,
             const std::wstring& baseDir) {
    const std::wstring ctx = L"ui";
    checkAttrs(n, {L"base", L"locale", L"font", L"template"}, ctx);
    spec.base = n.attr(L"base");
    if (!spec.base.empty() && !util::iequals(spec.base, L"wizard") &&
        !util::iequals(spec.base, L"minimal"))
        util::fail(L"<ui base=\"" + spec.base + L"\"> is not a supported base");
    if (n.has(L"locale")) spec.locale = n.attr(L"locale");
    if (n.has(L"font")) { spec.font = n.attr(L"font"); spec.hasFont = true; }
    if (n.has(L"template")) {
        spec.templateMsi = joinPath(baseDir, n.attr(L"template"));
        spec.hasTemplate = true;
    }

    // <strings> must be processed first so every later text can use $(Key).
    LocaleStrings strings = outer;
    for (const xml::Node& ch : n.children) {
        if (!util::iequals(ch.name, L"strings")) continue;
        LocaleStrings local = loadLocaleStrings(ch);
        for (const auto& kv : local.map) strings.map[kv.first] = kv.second;
    }

    for (const xml::Node& ch : n.children) {
        if (util::iequals(ch.name, L"strings")) continue;
        if (util::iequals(ch.name, L"textstyle")) {
            spec.styles.push_back(parseTextStyle(ch, strings));
        } else if (util::iequals(ch.name, L"binary")) {
            spec.binaries.push_back(parseBinary(ch, baseDir));
        } else if (util::iequals(ch.name, L"dialog")) {
            spec.dialogs.push_back(parseDialog(ch, strings, baseDir));
        } else if (util::iequals(ch.name, L"error")) {
            spec.errors.push_back(parseError(ch, strings));
        } else if (util::iequals(ch.name, L"billboard")) {
            spec.billboards.push_back(parseBillboard(ch, strings, L""));
        } else if (util::iequals(ch.name, L"billboardaction")) {
            checkAttrs(ch, {L"id"}, L"billboardaction");
            std::wstring action = ch.require(L"id", L"billboardaction");
            for (const xml::Node* b : ch.childList(L"billboard"))
                spec.billboards.push_back(parseBillboard(*b, strings, action));
        } else if (util::iequals(ch.name, L"property")) {
            checkAttrs(ch, {L"name", L"value"}, L"property");
            PropertyDef p;
            p.name = ch.require(L"name", L"ui property");
            p.value = applyLocaleStrings(ch.attr(L"value"), strings, L"property " + p.name);
            spec.properties.push_back(std::move(p));
        } else if (util::iequals(ch.name, L"checkbox")) {
            checkAttrs(ch, {L"property", L"value", L"name"}, L"checkbox");
            PropertyDef p;
            p.name = ch.attr(L"property", ch.attr(L"name"));
            if (p.name.empty()) util::fail(L"<checkbox> needs a property");
            p.value = ch.attr(L"value", L"1");
            spec.checkBoxes.push_back(std::move(p));
        } else if (util::iequals(ch.name, L"uisequence")) {
            parseSequence(ch, spec.sequence, L"uisequence");
            spec.hasSequence = true;
        } else if (util::iequals(ch.name, L"adminuisequence")) {
            parseSequence(ch, spec.adminSequence, L"adminuisequence");
            spec.hasAdminSequence = true;
        } else {
            util::fail(L"<ui> contains an unknown element <" + ch.name + L">");
        }
    }

    // Dialog sequence values turn into InstallUISequence rows when no explicit
    // sequence was given.
    if (!spec.hasSequence) {
        for (const auto& d : spec.dialogs) {
            if (!d.hasSequence) continue;
            SequenceEntry e;
            e.action = d.id;
            e.sequence = d.sequence;
            if (d.hasCondition) {
                e.condition = d.condition;
                e.hasCondition = true;
            }
            spec.sequence.push_back(std::move(e));
        }
        if (!spec.sequence.empty()) spec.hasSequence = true;
    }
}

void checkRoot(const xml::Node& root, const std::wstring& path) {
    if (!util::iequals(root.name, L"msitool"))
        util::fail(L"Root element must be <msitool> (got <" + root.name +
                   L">): " + path);
    if (root.ns != kNamespace)
        util::fail(L"Root element namespace must be " + std::wstring(kNamespace) +
                   L" (got '" + root.ns + L"'): " + path);
}

}  // namespace

LocaleStrings loadLocaleStrings(const xml::Node& root) {
    LocaleStrings out;
    if (!util::iequals(root.name, L"strings"))
        util::fail(L"Locale file must have a <strings> root (got <" + root.name + L">)");
    for (const xml::Node* s : root.childList(L"string")) {
        checkAttrs(*s, {L"id"}, L"string");
        std::wstring id = util::toLower(s->require(L"id", L"string"));
        out.map[id] = s->text;
    }
    return out;
}

std::wstring applyLocaleStrings(const std::wstring& text,
                                const LocaleStrings& strings,
                                const std::wstring& context) {
    std::wstring out;
    for (size_t i = 0; i < text.size();) {
        if (text[i] == L'$' && i + 1 < text.size() && text[i + 1] == L'$') {
            out += L'$';
            i += 2;
        } else if (text[i] == L'$' && i + 1 < text.size() && text[i + 1] == L'(') {
            size_t end = text.find(L')', i + 2);
            if (end == std::wstring::npos)
                util::fail(L"Unterminated $(...) in " + context);
            std::wstring key = util::toLower(text.substr(i + 2, end - i - 2));
            auto it = strings.map.find(key);
            if (it == strings.map.end())
                util::fail(L"Missing locale string '$(" + key + L")' in " + context);
            out += it->second;
            i = end + 1;
        } else {
            out += text[i++];
        }
    }
    return out;
}

UiSpec parseUiNode(const xml::Node& uiNode, const LocaleStrings& strings,
                   const std::wstring& baseDir) {
    UiSpec spec;
    parseUi(uiNode, spec, strings, baseDir);
    return spec;
}

std::wstring localeForLanguage(int lcid) {    switch (lcid) {
        case 2052:   // zh-CN
        case 1028:   // zh-TW -> reuse the simplified Chinese wizard
            return L"zh-CN";
        case 1033:
        default:
            return L"en";
    }
}

Manifest loadManifestXml(const std::wstring& path) {
    Manifest m;
    m.path = util::path::full(path);
    m.dir = util::path::dirName(m.path);
    if (m.dir.empty()) m.dir = L".";

    xml::Node root = xml::parseFile(m.path);
    checkRoot(root, m.path);

    for (const xml::Node& ch : root.children) {
        if (util::iequals(ch.name, L"pack")) {
            parsePack(ch, m);
        } else if (util::iequals(ch.name, L"features")) {
            parseFeatures(ch, m);
        } else if (util::iequals(ch.name, L"shortcuts")) {
            parseShortcuts(ch, m);
        } else if (util::iequals(ch.name, L"registry")) {
            parseRegistry(ch, m);
        } else if (util::iequals(ch.name, L"services")) {
            parseServices(ch, m);
        } else if (util::iequals(ch.name, L"servicecontrols")) {
            parseServiceControls(ch, m);
        } else if (util::iequals(ch.name, L"properties")) {
            parseProperties(ch, m);
        } else if (util::iequals(ch.name, L"environment")) {
            parseEnvironment(ch, m);
        } else if (util::iequals(ch.name, L"customactions")) {
            parseCustomActions(ch, m);
        } else if (util::iequals(ch.name, L"executesequence")) {
            parseSequence(ch, m.executeSequence, L"executeSequence");
            m.hasExecuteSequence = true;
        } else if (util::iequals(ch.name, L"adminexecutesequence")) {
            parseSequence(ch, m.adminExecuteSequence, L"adminExecuteSequence");
            m.hasAdminExecuteSequence = true;
        } else if (util::iequals(ch.name, L"sign")) {
            parseSign(ch, m);
        } else if (util::iequals(ch.name, L"ui")) {
            parseUi(ch, m.ui, LocaleStrings{}, m.dir);
        } else {
            util::fail(L"Unknown top-level element <" + ch.name + L"> in " + m.path);
        }
    }

    // De-duplicate generated identifiers.
    std::set<std::wstring> seen;
    auto unique = [&seen](std::wstring id) {
        std::wstring base = id;
        for (int n = 2; !seen.insert(id).second; ++n)
            id = base + L"_" + std::to_wstring(n);
        return id;
    };
    for (auto& f : m.features) f.id = unique(f.id);
    for (auto& d : m.shortcuts) d.id = unique(d.id);
    for (auto& d : m.registry) d.id = unique(d.id);
    for (auto& d : m.services) d.id = unique(d.id);
    for (auto& d : m.serviceControls) d.id = unique(d.id);
    for (auto& d : m.environment) d.id = unique(d.id);

    return m;
}

// ---------------------------------------------------------------------------
// UI template import (--ui-template <msi>)
// ---------------------------------------------------------------------------

namespace {

struct TemplateRow {
    std::map<std::wstring, std::wstring> cells;
    std::wstring get(const wchar_t* name) const {
        auto it = cells.find(util::toLower(name));
        return it == cells.end() ? std::wstring() : it->second;
    }
    int integer(const wchar_t* name, int deflt = 0) const {
        std::wstring v = util::trim(get(name));
        return v.empty() ? deflt : _wtoi(v.c_str());
    }
};

std::vector<TemplateRow> readRows(msi::Db& db, const wchar_t* table) {
    std::vector<TemplateRow> rows;
    if (!db.tableExists(table)) return rows;
    msi::QueryResult r = db.query(L"SELECT * FROM " + msi::quoteIdent(table));
    std::vector<std::wstring> cols;
    for (const auto& c : r.columns) cols.push_back(util::toLower(c));
    for (const auto& cells : r.rows) {
        TemplateRow row;
        for (size_t i = 0; i < cols.size() && i < cells.size(); ++i)
            row.cells[cols[i]] = cells[i];
        rows.push_back(std::move(row));
    }
    return rows;
}

const wchar_t* kTemplateActions[] = {
    L"CostInitialize", L"FileCost", L"CostFinalize", L"InstallValidate",
    L"InstallInitialize", L"InstallFinalize", L"InstallAdminPackage",
    L"ProcessComponents", L"UnpublishFeatures", L"PublishFeatures",
    L"PublishProduct", L"RegisterProduct", L"RegisterUser", L"MigrateFeatureStates",
    L"FindRelatedProducts", L"AppSearch", L"LaunchConditions", L"ValidateProductID",
    L"SetODBCFolders", L"CreateShortcuts", L"RemoveShortcuts",
    L"WriteRegistryValues", L"RemoveRegistryValues", L"WriteIniValues",
    L"RemoveIniValues", L"InstallFiles", L"RemoveFiles", L"CreateFolders",
    L"RemoveFolders", L"SelfRegModules", L"SelfUnregModules", L"InstallServices",
    L"StartServices", L"StopServices", L"DeleteServices",
    L"WriteEnvironmentStrings", L"RemoveEnvironmentStrings",
    L"InstallExecute", L"InstallExecuteAgain", L"ResolveSource",
    L"RemoveExistingProducts", L"ScheduleReboot", L"ForceReboot",
};

bool knownTemplateAction(const std::wstring& a) {
    for (const wchar_t* x : kTemplateActions)
        if (util::iequals(a, x)) return true;
    return false;
}

const wchar_t* kTemplateEvents[] = {
    L"EndDialog", L"NewDialog", L"SpawnDialog", L"SpawnWaitDialog",
    L"SetTargetPath", L"SetProperty", L"ActionText", L"SetProgress",
    L"DoAction", L"Reinstall", L"ReinstallMode", L"Remove", L"Reset",
    L"DirectoryListUp", L"DirectoryListOpen", L"DirectoryListNew",
    L"EnableRollback", L"AddLocal", L"AddSource", L"PublishProduct",
    L"Ignore", L"Communication", L"SelectionBrowse", L"SelectionDescription",
    L"SelectionPath", L"SelectionSize", L"SetInstallMode", L"RMShutdownAndRestart",
    L"RMShutdown", L"RMRestart", L"MsiRMFilesInUse",
};

bool knownTemplateEvent(const std::wstring& e) {
    if (e.size() >= 3 && e.front() == L'[' && e.back() == L']') return true;
    for (const wchar_t* x : kTemplateEvents)
        if (util::iequals(e, x)) return true;
    return false;
}

bool reservedProperty(const std::wstring& name) {
    static const wchar_t* reserved[] = {
        L"ProductName", L"ProductVersion", L"ProductCode", L"UpgradeCode",
        L"Manufacturer", L"ProductLanguage", L"ALLUSERS", L"INSTALLLEVEL",
        L"INSTALLFOLDER", L"DefaultUIFont",
    };
    for (const wchar_t* r : reserved)
        if (util::iequals(name, r)) return true;
    return false;
}

}  // namespace

UiSpec loadUiTemplate(const std::wstring& msiPath) {
    if (!util::path::exists(msiPath))
        util::fail(L"UI template not found: " + msiPath);
    msi::Db db(msiPath);
    if (!db.tableExists(L"Dialog"))
        util::fail(L"UI template has no Dialog table: " + msiPath);

    UiSpec spec;
    for (const auto& r : readRows(db, L"TextStyle")) {
        TextStyleDef s;
        s.id = r.get(L"TextStyle");
        s.face = r.get(L"FaceName");
        s.size = r.integer(L"Size", 8);
        if (!r.get(L"Color").empty()) { s.color = r.integer(L"Color"); s.hasColor = true; }
        if (!r.get(L"StyleBits").empty()) { s.style = r.integer(L"StyleBits"); s.hasStyle = true; }
        if (!s.id.empty() && !s.face.empty()) spec.styles.push_back(std::move(s));
    }

    for (const auto& r : readRows(db, L"Dialog")) {
        DialogDef d;
        d.id = r.get(L"Dialog");
        if (d.id.empty()) continue;
        d.width = r.integer(L"Width", 370);
        d.height = r.integer(L"Height", 270);
        d.attributes = r.integer(L"Attributes", 7);
        d.hasWidth = d.hasHeight = d.hasAttributes = true;
        d.title = r.get(L"Title");
        d.hasTitle = !d.title.empty();
        d.first = r.get(L"Control_First");
        d.hasFirst = !d.first.empty();
        d.def = r.get(L"Control_Default");
        d.hasDefault = !d.def.empty();
        d.cancel = r.get(L"Control_Cancel");
        d.hasCancel = !d.cancel.empty();
        spec.dialogs.push_back(std::move(d));
    }
    if (spec.dialogs.empty())
        util::fail(L"UI template contains no dialogs: " + msiPath);

    auto findDialog = [&](const std::wstring& id) -> DialogDef* {
        for (auto& d : spec.dialogs)
            if (util::iequals(d.id, id)) return &d;
        return nullptr;
    };
    auto findControl = [&](const std::wstring& dialog, const std::wstring& id) -> ControlDef* {
        DialogDef* d = findDialog(dialog);
        if (!d) return nullptr;
        for (auto& c : d->controls)
            if (util::iequals(c.id, id)) return &c;
        return nullptr;
    };

    size_t dropped = 0;
    for (const auto& r : readRows(db, L"Control")) {
        DialogDef* d = findDialog(r.get(L"Dialog_"));
        if (!d) { ++dropped; continue; }
        ControlDef c;
        c.id = r.get(L"Control");
        c.type = r.get(L"Type");
        if (c.id.empty() || c.type.empty()) { ++dropped; continue; }
        c.x = r.integer(L"X");
        c.y = r.integer(L"Y");
        c.width = r.integer(L"Width");
        c.height = r.integer(L"Height");
        c.hasX = c.hasY = c.hasWidth = c.hasHeight = true;
        c.attributes = r.integer(L"Attributes", 3);
        c.hasAttributes = true;
        c.property = r.get(L"Property");
        c.hasProperty = !c.property.empty();
        std::wstring text = r.get(L"Text");
        if (util::iequals(c.type, L"Bitmap") || util::iequals(c.type, L"Icon")) {
            c.binary = text;
            c.hasBinary = !text.empty();
        } else {
            c.text = text;
            c.hasText = !text.empty();
        }
        c.next = r.get(L"Control_Next");
        c.hasNext = !c.next.empty();
        c.help = r.get(L"Help");
        c.hasHelp = !c.help.empty();
        d->controls.push_back(std::move(c));
    }

    // Custom actions referenced by template UI events. Only actions whose
    // code lives in a Binary stream are kept: the stream is imported too, so
    // no file from the template package is needed at install time.
    std::set<std::wstring> templateActions;
    for (const auto& r : readRows(db, L"CustomAction")) {
        CustomActionDef ca;
        ca.id = r.get(L"Action");
        ca.type = r.integer(L"Type");
        ca.source = r.get(L"Source");
        ca.target = r.get(L"Target");
        if (ca.id.empty()) continue;
        const int execType = ca.type & 0x0f;
        const int srcKind = ca.type & 0x30;
        if (execType != 1 && execType != 2 && execType != 3) {
            util::errLine(L"[ui][warn] template custom action dropped (unsupported "
                          L"type " + std::to_wstring(ca.type) + L"): " + ca.id);
            ++dropped;
            continue;
        }
        if (srcKind != 0x00 && srcKind != 0x30) {
            util::errLine(L"[ui][warn] template custom action dropped (references "
                          L"template files): " + ca.id);
            ++dropped;
            continue;
        }
        templateActions.insert(util::toLower(ca.id));
        spec.customActions.push_back(std::move(ca));
    }

    for (const auto& r : readRows(db, L"ControlEvent")) {
        ControlDef* c = findControl(r.get(L"Dialog_"), r.get(L"Control_"));
        if (!c) { ++dropped; continue; }
        std::wstring event = r.get(L"Event");
        std::wstring value = r.get(L"Argument");
        if (!knownTemplateEvent(event)) {
            util::errLine(L"[ui][warn] template event dropped: " + event);
            ++dropped;
            continue;
        }
        if (util::iequals(event, L"DoAction") && !knownTemplateAction(value) &&
            !templateActions.count(util::toLower(value))) {
            util::errLine(L"[ui][warn] template custom action dropped: " + value);
            ++dropped;
            continue;
        }
        if ((util::iequals(event, L"NewDialog") || util::iequals(event, L"SpawnDialog") ||
             util::iequals(event, L"SpawnWaitDialog")) && !findDialog(value)) {
            util::errLine(L"[ui][warn] template event target missing: " + value);
            ++dropped;
            continue;
        }
        PublishDef p;
        p.event = event;
        p.value = value;
        p.condition = r.get(L"Condition");
        if (p.condition.empty()) p.condition = L"1";
        p.order = r.integer(L"Ordering");
        p.hasOrder = p.order != 0;
        c->publish.push_back(std::move(p));
    }

    for (const auto& r : readRows(db, L"ControlCondition")) {
        ControlDef* c = findControl(r.get(L"Dialog_"), r.get(L"Control_"));
        if (!c) { ++dropped; continue; }
        ConditionDef cc;
        cc.action = r.get(L"Action");
        cc.condition = r.get(L"Condition");
        if (!cc.action.empty() && !cc.condition.empty())
            c->conditions.push_back(std::move(cc));
    }

    for (const auto& r : readRows(db, L"RadioButton")) {
        std::wstring property = r.get(L"Property");
        ControlDef* ctl = nullptr;
        for (auto& d : spec.dialogs) {
            for (auto& c : d.controls)
                if (util::iequals(c.type, L"RadioButtonGroup") &&
                    util::iequals(c.property, property)) { ctl = &c; break; }
            if (ctl) break;
        }
        if (!ctl) { ++dropped; continue; }
        ListItemDef item;
        item.value = r.get(L"Value");
        item.text = r.get(L"Text");
        item.help = r.get(L"Help");
        ctl->items.push_back(std::move(item));
    }
    auto attachItems = [&](const wchar_t* table, const wchar_t* type) {
        for (const auto& r : readRows(db, table)) {
            std::wstring property = r.get(L"Property");
            ControlDef* ctl = nullptr;
            for (auto& d : spec.dialogs) {
                for (auto& c : d.controls)
                    if (util::iequals(c.type, type) &&
                        util::iequals(c.property, property)) { ctl = &c; break; }
                if (ctl) break;
            }
            if (!ctl) continue;
            ListItemDef item;
            item.value = r.get(L"Value");
            item.text = r.get(L"Text");
            ctl->items.push_back(std::move(item));
        }
    };
    attachItems(L"ComboBox", L"ComboBox");
    attachItems(L"ListBox", L"ListBox");
    attachItems(L"ListView", L"ListView");

    for (const auto& r : readRows(db, L"Error")) {
        ErrorDef e;
        e.id = r.integer(L"Error");
        e.text = r.get(L"Text");
        if (e.id > 0) spec.errors.push_back(std::move(e));
    }

    for (const auto& r : readRows(db, L"CheckBox")) {
        PropertyDef p;
        p.name = r.get(L"Property");
        p.value = r.get(L"Value");
        if (!p.name.empty()) spec.checkBoxes.push_back(std::move(p));
    }

    if (db.tableExists(L"Binary")) {
        msi::QueryResult names = db.query(L"SELECT `Name` FROM `Binary`");
        for (const auto& row : names.rows) {
            if (row.empty()) continue;
            std::wstring name = row[0];
            std::vector<unsigned char> data = db.readStream(
                L"SELECT `Data` FROM `Binary` WHERE `Name`=?", {name});
            if (data.empty()) continue;
            BinaryDef b;
            b.id = name;
            b.data = std::move(data);
            spec.binaries.push_back(std::move(b));
        }
    }

    for (const auto& r : readRows(db, L"Billboard")) {
        BillboardDef b;
        b.id = r.get(L"Billboard");
        b.feature = r.get(L"Feature_");
        b.action = r.get(L"Action");
        b.ordering = r.integer(L"Ordering", 1);
        if (!b.id.empty()) spec.billboards.push_back(std::move(b));
    }
    for (const auto& r : readRows(db, L"BBControl")) {
        for (auto& b : spec.billboards) {
            if (!util::iequals(b.id, r.get(L"Billboard_"))) continue;
            BillboardControlDef c;
            c.id = r.get(L"BBControl");
            c.type = r.get(L"Type");
            c.x = r.integer(L"X");
            c.y = r.integer(L"Y");
            c.width = r.integer(L"Width");
            c.height = r.integer(L"Height");
            c.attributes = r.integer(L"Attributes", 1);
            c.hasAttributes = true;
            c.text = r.get(L"Text");
            b.controls.push_back(std::move(c));
            break;
        }
    }

    for (const auto& r : readRows(db, L"Property")) {
        std::wstring name = r.get(L"Property");
        std::wstring value = r.get(L"Value");
        if (name.empty()) continue;
        if (util::iequals(name, L"DefaultUIFont")) {
            if (!value.empty()) { spec.font = value; spec.hasFont = true; }
            continue;
        }
        if (reservedProperty(name)) continue;
        spec.properties.push_back({name, value});
    }

    auto importSequence = [&](const wchar_t* table, std::vector<SequenceEntry>& out,
                              bool& present) {
        std::vector<TemplateRow> rows = readRows(db, table);
        present = !rows.empty();
        for (const auto& r : rows) {
            SequenceEntry e;
            e.action = r.get(L"Action");
            e.sequence = r.integer(L"Sequence");
            e.condition = r.get(L"Condition");
            e.hasCondition = !e.condition.empty();
            if (e.action.empty()) continue;
            bool isDialog = findDialog(e.action) != nullptr;
            if (!isDialog && !knownTemplateAction(e.action) &&
                !util::iequals(e.action, L"ExecuteAction") && e.sequence >= 0) {
                util::errLine(L"[ui][warn] template sequence action dropped: " + e.action);
                continue;
            }
            if ((util::iequals(e.action, L"FatalError") ||
                 util::iequals(e.action, L"UserExit") ||
                 util::iequals(e.action, L"ExitDialog")) && !isDialog)
                continue;
            out.push_back(std::move(e));
        }
    };
    importSequence(L"InstallUISequence", spec.sequence, spec.hasSequence);
    importSequence(L"AdminUISequence", spec.adminSequence, spec.hasAdminSequence);

    if (dropped)
        util::errLine(L"[ui][warn] UI template: " + std::to_wstring(dropped) +
                      L" row(s) could not be imported");
    util::outLine(L"UI template: " + msiPath + L" (" +
                  std::to_wstring(spec.dialogs.size()) + L" dialog(s), " +
                  std::to_wstring(spec.binaries.size()) + L" binary stream(s))");
    return spec;
}

}  // namespace pack
