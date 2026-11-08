#include "pack_ui.h"

#include "pack_util.h"
#include "pack_xml.h"
#include "xml.h"

#include <algorithm>
#include <vector>

namespace pack {

namespace {

const int kResourceLayout = 101;
const int kResourceEn = 102;
const int kResourceZh = 103;
const int kResourceLicenseEn = 104;
const int kResourceLicenseZh = 105;
const int kResourceBanner = 106;
const int kResourceBackground = 107;

bool loadResource(int name, std::vector<unsigned char>& out) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(name), RT_RCDATA);
    if (!res) return false;
    DWORD size = SizeofResource(inst, res);
    HGLOBAL mem = LoadResource(inst, res);
    if (!mem || !size) return false;
    const void* data = LockResource(mem);
    if (!data) return false;
    out.assign(static_cast<const unsigned char*>(data),
               static_cast<const unsigned char*>(data) + size);
    return true;
}

bool writeResource(int name, const std::wstring& path,
                   std::wstring& error) {
    std::vector<unsigned char> data;
    if (!loadResource(name, data)) {
        error = L"Embedded UI resource is missing: " + std::to_wstring(name);
        return false;
    }
    if (!util::path::writeFile(path, data)) {
        error = L"Cannot write " + path;
        return false;
    }
    return true;
}

ControlDef makeControl(const wchar_t* id, const wchar_t* type, int x, int y,
                       int w, int h, int attributes, const std::wstring& text,
                       const wchar_t* next) {
    ControlDef c;
    c.id = id;
    c.type = type;
    c.x = x; c.y = y; c.width = w; c.height = h;
    c.hasX = c.hasY = c.hasWidth = c.hasHeight = true;
    c.attributes = attributes;
    c.hasAttributes = true;
    c.text = text;
    c.hasText = !text.empty();
    c.next = next ? next : L"";
    c.hasNext = !c.next.empty();
    return c;
}

DialogDef makeDialog(const wchar_t* id, int attributes, const std::wstring& title,
                     const wchar_t* first, const wchar_t* def, const wchar_t* cancel,
                     int sequence) {
    DialogDef d;
    d.id = id;
    d.width = 370;
    d.height = 270;
    d.hasWidth = d.hasHeight = true;
    d.attributes = attributes;
    d.hasAttributes = true;
    d.title = title;
    d.hasTitle = true;
    d.first = first ? first : L"";
    d.hasFirst = !d.first.empty();
    d.def = def ? def : L"";
    d.hasDefault = !d.def.empty();
    d.cancel = cancel ? cancel : L"";
    d.hasCancel = !d.cancel.empty();
    d.sequence = sequence;
    d.hasSequence = sequence != 0;
    return d;
}

void addPublish(ControlDef& c, const wchar_t* event, const wchar_t* value,
                int order) {
    PublishDef p;
    p.event = event;
    p.value = value;
    p.condition = L"1";
    p.order = order;
    p.hasOrder = order > 0;
    c.publish.push_back(std::move(p));
}

// The historical --ui minimal wizard, kept byte-compatible with the old
// hard-coded tables so existing packages keep installing the same way.
void buildWelcome(DialogDef& d) {
    const std::wstring title = L"[ProductName] Setup";
    d.controls.push_back(makeControl(L"Title", L"Text", 20, 20, 330, 40, 196611,
                                     L"{\\DlgFont8}" + title, L"Description"));
    d.controls.push_back(makeControl(
        L"Description", L"Text", 20, 70, 330, 90, 196611,
        L"Welcome to the [ProductName] [ProductVersion] Setup. Click Install to "
        L"continue or Cancel to exit.", L"Cancel"));
    d.controls.push_back(makeControl(L"BottomLine", L"Line", 0, 234, 370, 0, 1, L"", nullptr));
    d.controls.push_back(makeControl(L"Next", L"PushButton", 236, 243, 56, 17, 3,
                                     L"&Install", L"Title"));
    d.controls.push_back(makeControl(L"Cancel", L"PushButton", 304, 243, 56, 17, 3,
                                     L"Cancel", L"Next"));
    addPublish(d.controls[3], L"EndDialog", L"Return", 1);
    addPublish(d.controls[4], L"EndDialog", L"Exit", 1);
}

void buildProgress(DialogDef& d) {
    const std::wstring title = L"[ProductName] Setup";
    d.controls.push_back(makeControl(L"Title", L"Text", 20, 20, 330, 40, 196611,
                                     L"{\\DlgFont8}" + title, L"Description"));
    d.controls.push_back(makeControl(
        L"Description", L"Text", 20, 70, 330, 90, 196611,
        L"Please wait while Setup installs [ProductName]...", L"Cancel"));
    d.controls.push_back(makeControl(L"BottomLine", L"Line", 0, 234, 370, 0, 1, L"", nullptr));
    d.controls.push_back(makeControl(L"ActionText", L"Text", 20, 175, 330, 12, 3, L"", nullptr));
    d.controls.push_back(makeControl(L"ProgressBar", L"ProgressBar", 20, 195, 330, 12,
                                     65537, L"Progress done", nullptr));
    d.controls.push_back(makeControl(L"Cancel", L"PushButton", 304, 243, 56, 17, 3,
                                     L"Cancel", L"Title"));
    addPublish(d.controls[5], L"EndDialog", L"Exit", 1);
    addPublish(d.controls[3], L"ActionText", L"[ActionText]", 1);
    addPublish(d.controls[4], L"SetProgress", L"[Progress]", 1);
}

void buildFinish(DialogDef& d, const wchar_t* text) {
    const std::wstring title = L"[ProductName] Setup";
    d.controls.push_back(makeControl(L"Title", L"Text", 20, 20, 330, 40, 196611,
                                     L"{\\DlgFont8}" + title, L"Description"));
    d.controls.push_back(makeControl(L"Description", L"Text", 20, 70, 330, 90, 196611,
                                     text, L"Finish"));
    d.controls.push_back(makeControl(L"BottomLine", L"Line", 0, 234, 370, 0, 1, L"", nullptr));
    d.controls.push_back(makeControl(L"Finish", L"PushButton", 236, 243, 56, 17, 3,
                                     L"&Finish", L"Title"));
    addPublish(d.controls[3], L"EndDialog", L"Exit", 1);
}

}  // namespace

UiSpec builtinMinimalUi() {
    UiSpec spec;
    spec.base = L"minimal";
    spec.font = L"DlgFont8";
    spec.hasFont = true;

    TextStyleDef style;
    style.id = L"DlgFont8";
    style.face = L"Tahoma";
    style.size = 8;
    style.hasColor = true;
    style.color = 0;
    style.hasStyle = true;
    style.style = 0;
    spec.styles.push_back(style);

    DialogDef welcome = makeDialog(L"WelcomeDlg", 7, L"[ProductName] Setup",
                                   L"Next", L"Next", L"Cancel", 1290);
    buildWelcome(welcome);
    spec.dialogs.push_back(std::move(welcome));

    DialogDef progress = makeDialog(L"ProgressDlg", 5, L"[ProductName] Setup",
                                    L"Cancel", L"Cancel", L"Cancel", 1299);
    buildProgress(progress);
    spec.dialogs.push_back(std::move(progress));

    DialogDef exit = makeDialog(L"ExitDialog", 7, L"[ProductName] Setup",
                                L"Finish", L"Finish", L"Finish", -1);
    buildFinish(exit, L"[ProductName] [ProductVersion] setup is complete.");
    spec.dialogs.push_back(std::move(exit));

    DialogDef userExit = makeDialog(L"UserExit", 7, L"[ProductName] Setup",
                                    L"Finish", L"Finish", L"Finish", -2);
    buildFinish(userExit,
                L"[ProductName] setup was cancelled. Your system has not been modified.");
    spec.dialogs.push_back(std::move(userExit));

    DialogDef fatal = makeDialog(L"FatalError", 7, L"[ProductName] Setup",
                                 L"Finish", L"Finish", L"Finish", -3);
    buildFinish(fatal, L"[ProductName] setup ended prematurely because of an error.");
    spec.dialogs.push_back(std::move(fatal));

    for (const auto& d : spec.dialogs) {
        SequenceEntry e;
        e.action = d.id;
        e.sequence = d.sequence;
        spec.sequence.push_back(e);
    }
    auto addAction = [&](const wchar_t* name, int seq) {
        SequenceEntry e;
        e.action = name;
        e.sequence = seq;
        spec.sequence.push_back(std::move(e));
    };
    addAction(L"CostInitialize", 800);
    addAction(L"FileCost", 900);
    addAction(L"CostFinalize", 1000);
    addAction(L"ExecuteAction", 1300);
    spec.hasSequence = true;
    return spec;
}

UiSpec builtinWizardUi(const std::wstring& locale, const std::wstring& tempDir) {
    const bool zh = util::iequals(locale, L"zh-CN");
    std::wstring error;

    const std::wstring layoutPath = util::path::join(tempDir, L"wizard.layout.xml");
    const std::wstring localePath = util::path::join(tempDir, L"wizard.strings.xml");
    const std::wstring licensePath = util::path::join(tempDir, L"license.rtf");

    if (!writeResource(kResourceLayout, layoutPath, error) ||
        !writeResource(zh ? kResourceZh : kResourceEn, localePath, error) ||
        !writeResource(zh ? kResourceLicenseZh : kResourceLicenseEn, licensePath, error) ||
        !writeResource(kResourceBanner, util::path::join(tempDir, L"banner.bmp"), error) ||
        !writeResource(kResourceBackground,
                       util::path::join(tempDir, L"background.bmp"), error))
        util::fail(error);

    xml::Node layout = xml::parseFile(layoutPath);
    if (!util::iequals(layout.name, L"msitool"))
        util::fail(L"Embedded wizard layout has an invalid root element");
    const xml::Node* ui = layout.child(L"ui");
    if (!ui) util::fail(L"Embedded wizard layout has no <ui> element");

    LocaleStrings strings = loadLocaleStrings(xml::parseFile(localePath));
    UiSpec spec = parseUiNode(*ui, strings, tempDir);
    spec.locale = zh ? L"zh-CN" : L"en";
    return spec;
}

bool dumpBuiltinUi(const std::wstring& outDir, std::wstring& error) {
    if (!util::path::makeDirs(outDir)) {
        error = L"Cannot create " + outDir;
        return false;
    }
    struct Res {
        int name;
        const wchar_t* file;
    } resources[] = {
        {kResourceLayout, L"wizard.layout.xml"},
        {kResourceEn, L"wizard.en.xml"},
        {kResourceZh, L"wizard.zh-CN.xml"},
        {kResourceLicenseEn, L"license.en.rtf"},
        {kResourceLicenseZh, L"license.zh-CN.rtf"},
        {kResourceBanner, L"banner.bmp"},
        {kResourceBackground, L"background.bmp"},
    };
    for (const Res& r : resources) {
        if (!writeResource(r.name, util::path::join(outDir, r.file), error)) return false;
    }
    return true;
}

}  // namespace pack
