#include "pack_ui.h"

#include "pack_util.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

namespace pack {

namespace {

const wchar_t* kControlTypes[] = {
    L"Text", L"PushButton", L"Line", L"Bitmap", L"Icon", L"ProgressBar",
    L"PathEdit", L"Edit", L"CheckBox", L"RadioButtonGroup", L"ScrollableText",
    L"DirectoryList", L"DirectoryCombo", L"VolumeSelectCombo", L"ListView",
    L"ListBox", L"ComboBox", L"Billboard", L"GroupBox", L"Hyperlink",
    L"SelectionTree", L"MaskedEdit", L"VolumeCostList",
};

const wchar_t* kEvents[] = {
    L"EndDialog", L"NewDialog", L"SpawnDialog", L"SpawnWaitDialog",
    L"SetTargetPath", L"SetProperty", L"ActionText", L"SetProgress",
    L"DoAction", L"Reinstall", L"ReinstallMode", L"Remove", L"Reset",
    L"DirectoryListUp", L"DirectoryListOpen", L"DirectoryListNew",
    L"EnableRollback", L"AddLocal", L"AddSource", L"PublishProduct",
    L"Ignore", L"SetInstallMode", L"RMShutdownAndRestart", L"RMShutdown",
    L"RMRestart", L"MsiRMFilesInUse", L"Communication", L"SelectionBrowse",
    L"SelectionDescription", L"SelectionPath", L"SelectionSize",
};

const wchar_t* kStandardActions[] = {
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

bool inList(const std::wstring& value, const wchar_t* const* list, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (util::iequals(value, list[i])) return true;
    return false;
}

bool isControlType(const std::wstring& t) {
    return inList(t, kControlTypes, sizeof(kControlTypes) / sizeof(kControlTypes[0]));
}

bool isEventName(const std::wstring& e) {
    // "[Property]" is the documented "set this property" form.
    if (e.size() >= 3 && e.front() == L'[' && e.back() == L']') return true;
    return inList(e, kEvents, sizeof(kEvents) / sizeof(kEvents[0]));
}

bool isStandardAction(const std::wstring& a) {
    return inList(a, kStandardActions, sizeof(kStandardActions) / sizeof(kStandardActions[0]));
}

int effectiveAttributes(const ControlDef& c) {
    if (c.hasAttributes) return c.attributes;
    int a = 0;
    if (c.visible) a |= 1;
    if (c.enabled) a |= 2;
    if (c.sunken) a |= 4;
    if (c.indirect) a |= 8;
    return a;
}

std::wstring lower(const std::wstring& s) { return util::toLower(s); }

}  // namespace

DialogDef* UiSpec::findDialog(const std::wstring& id) {
    for (auto& d : dialogs)
        if (util::iequals(d.id, id)) return &d;
    return nullptr;
}

const DialogDef* UiSpec::findDialog(const std::wstring& id) const {
    for (const auto& d : dialogs)
        if (util::iequals(d.id, id)) return &d;
    return nullptr;
}

ControlDef* UiSpec::findControl(const std::wstring& dialogId, const std::wstring& id) {
    DialogDef* d = findDialog(dialogId);
    if (!d) return nullptr;
    for (auto& c : d->controls)
        if (util::iequals(c.id, id)) return &c;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Table requirements
// ---------------------------------------------------------------------------

UiTables uiTablesFor(const UiSpec& spec) {
    UiTables t;
    t.dialog = !spec.dialogs.empty();
    t.textStyle = !spec.styles.empty();
    t.binary = !spec.binaries.empty();
    t.error = !spec.errors.empty();
    t.billboard = !spec.billboards.empty();
    t.installSequence = spec.hasSequence || !spec.dialogs.empty();
    t.adminSequence = spec.hasAdminSequence;
    if (!spec.checkBoxes.empty()) t.checkBox = true;
    for (const auto& d : spec.dialogs) {
        if (d.controls.empty()) continue;
        t.control = true;
        for (const auto& c : d.controls) {
            if (!c.publish.empty()) t.controlEvent = true;
            if (!c.conditions.empty()) t.controlCondition = true;
            if (util::iequals(c.type, L"RadioButtonGroup")) t.radioButton = true;
            if (util::iequals(c.type, L"CheckBox")) {
                t.checkBox = true;
            }
            if (util::iequals(c.type, L"ComboBox")) t.comboBox = true;
            if (util::iequals(c.type, L"ListBox")) t.listBox = true;
            if (util::iequals(c.type, L"ListView")) t.listView = true;
        }
    }
    return t;
}

std::wstring uiTableNames(const UiTables& t) {
    std::wstring s;
    auto add = [&](bool on, const wchar_t* name) {
        if (!on) return;
        if (!s.empty()) s += L", ";
        s += name;
    };
    add(t.dialog, L"Dialog");
    add(t.control, L"Control");
    add(t.controlEvent, L"ControlEvent");
    add(t.controlCondition, L"ControlCondition");
    add(t.textStyle, L"TextStyle");
    add(t.binary, L"Binary");
    add(t.radioButton, L"RadioButton");
    add(t.checkBox, L"CheckBox");
    add(t.comboBox, L"ComboBox");
    add(t.listBox, L"ListBox");
    add(t.listView, L"ListView");
    add(t.error, L"Error");
    add(t.billboard, L"Billboard");
    add(t.installSequence, L"InstallUISequence");
    add(t.adminSequence, L"AdminUISequence");
    return s;
}

// ---------------------------------------------------------------------------
// Merge (XML delta on top of a built-in base / template)
// ---------------------------------------------------------------------------

namespace {

void mergeControl(ControlDef& dst, const ControlDef& src) {
    if (!src.id.empty()) dst.id = src.id;
    if (!src.type.empty()) dst.type = src.type;
    if (src.hasX) { dst.x = src.x; dst.hasX = true; }
    if (src.hasY) { dst.y = src.y; dst.hasY = true; }
    if (src.hasWidth) { dst.width = src.width; dst.hasWidth = true; }
    if (src.hasHeight) { dst.height = src.height; dst.hasHeight = true; }
    if (src.hasAttributes) { dst.attributes = src.attributes; dst.hasAttributes = true; }
    if (src.hasVisible) { dst.visible = src.visible; dst.hasVisible = true; }
    if (src.hasEnabled) { dst.enabled = src.enabled; dst.hasEnabled = true; }
    if (src.hasSunken) { dst.sunken = src.sunken; dst.hasSunken = true; }
    if (src.hasIndirect) { dst.indirect = src.indirect; dst.hasIndirect = true; }
    if (src.hasProperty) { dst.property = src.property; dst.hasProperty = true; }
    if (src.hasText) { dst.text = src.text; dst.hasText = true; }
    if (src.hasBinary) { dst.binary = src.binary; dst.hasBinary = true; }
    if (src.hasNext) { dst.next = src.next; dst.hasNext = true; }
    if (src.hasHelp) { dst.help = src.help; dst.hasHelp = true; }

    for (const auto& p : src.publish) {
        auto it = std::find_if(dst.publish.begin(), dst.publish.end(),
                               [&](const PublishDef& x) {
                                   return util::iequals(x.event, p.event) &&
                                          util::iequals(x.value, p.value);
                               });
        if (it != dst.publish.end()) *it = p;
        else dst.publish.push_back(p);
    }
    for (const auto& c : src.conditions) {
        auto it = std::find_if(dst.conditions.begin(), dst.conditions.end(),
                               [&](const ConditionDef& x) {
                                   return util::iequals(x.action, c.action);
                               });
        if (it != dst.conditions.end()) *it = c;
        else dst.conditions.push_back(c);
    }
    if (!src.items.empty()) dst.items = src.items;
}

void mergeDialog(DialogDef& dst, const DialogDef& src) {
    if (src.hasTitle) { dst.title = src.title; dst.hasTitle = true; }
    if (src.hasWidth) { dst.width = src.width; dst.hasWidth = true; }
    if (src.hasHeight) { dst.height = src.height; dst.hasHeight = true; }
    if (src.hasAttributes) { dst.attributes = src.attributes; dst.hasAttributes = true; }
    if (src.hasFirst) { dst.first = src.first; dst.hasFirst = true; }
    if (src.hasDefault) { dst.def = src.def; dst.hasDefault = true; }
    if (src.hasCancel) { dst.cancel = src.cancel; dst.hasCancel = true; }
    if (src.hasSequence) { dst.sequence = src.sequence; dst.hasSequence = true; }
    if (src.hasCondition) { dst.condition = src.condition; dst.hasCondition = true; }

    for (const auto& c : src.controls) {
        auto it = std::find_if(dst.controls.begin(), dst.controls.end(),
                               [&](const ControlDef& x) {
                                   return util::iequals(x.id, c.id);
                               });
        if (it != dst.controls.end()) mergeControl(*it, c);
        else dst.controls.push_back(c);
    }
}

}  // namespace

void mergeUi(UiSpec& base, const UiSpec& overlay) {
    if (!overlay.base.empty()) base.base = overlay.base;
    if (!overlay.locale.empty()) base.locale = overlay.locale;
    if (overlay.hasFont) { base.font = overlay.font; base.hasFont = true; }
    if (overlay.hasTemplate) { base.templateMsi = overlay.templateMsi; base.hasTemplate = true; }

    for (const auto& s : overlay.styles) {
        auto it = std::find_if(base.styles.begin(), base.styles.end(),
                               [&](const TextStyleDef& x) { return util::iequals(x.id, s.id); });
        if (it != base.styles.end()) *it = s;
        else base.styles.push_back(s);
    }
    for (const auto& d : overlay.dialogs) {
        auto it = std::find_if(base.dialogs.begin(), base.dialogs.end(),
                               [&](const DialogDef& x) { return util::iequals(x.id, d.id); });
        if (it != base.dialogs.end()) mergeDialog(*it, d);
        else base.dialogs.push_back(d);
    }
    if (overlay.hasSequence) { base.sequence = overlay.sequence; base.hasSequence = true; }
    if (overlay.hasAdminSequence) {
        base.adminSequence = overlay.adminSequence;
        base.hasAdminSequence = true;
    }
    for (const auto& p : overlay.properties) {
        auto it = std::find_if(base.properties.begin(), base.properties.end(),
                               [&](const PropertyDef& x) { return util::iequals(x.name, p.name); });
        if (it != base.properties.end()) *it = p;
        else base.properties.push_back(p);
    }
    for (const auto& b : overlay.binaries) {
        auto it = std::find_if(base.binaries.begin(), base.binaries.end(),
                               [&](const BinaryDef& x) { return util::iequals(x.id, b.id); });
        if (it != base.binaries.end()) *it = b;
        else base.binaries.push_back(b);
    }
    for (const auto& e : overlay.errors) {
        auto it = std::find_if(base.errors.begin(), base.errors.end(),
                               [&](const ErrorDef& x) { return x.id == e.id; });
        if (it != base.errors.end()) *it = e;
        else base.errors.push_back(e);
    }
    for (const auto& b : overlay.billboards) {
        auto it = std::find_if(base.billboards.begin(), base.billboards.end(),
                               [&](const BillboardDef& x) { return util::iequals(x.id, b.id); });
        if (it != base.billboards.end()) *it = b;
        else base.billboards.push_back(b);
    }
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------

void validateUi(const UiSpec& spec, const std::vector<std::wstring>& customActions,
                std::vector<UiIssue>& issues) {
    auto error = [&](const std::wstring& msg) { issues.push_back({true, msg}); };
    auto warn = [&](const std::wstring& msg) { issues.push_back({false, msg}); };

    std::set<std::wstring> actionIds;
    for (const auto& a : spec.customActions) actionIds.insert(lower(a.id));
    for (const auto& a : customActions) actionIds.insert(lower(a));

    std::set<std::wstring> styleIds;
    for (const auto& s : spec.styles) {
        if (s.id.empty()) { error(L"<textstyle> without an id"); continue; }
        if (!styleIds.insert(lower(s.id)).second)
            error(L"duplicate text style: " + s.id);
        if (s.face.empty()) error(L"text style " + s.id + L" has no face name");
    }
    if (!spec.font.empty() && !styleIds.count(lower(spec.font)))
        error(L"DefaultUIFont '" + spec.font + L"' is not defined in TextStyle");

    std::set<std::wstring> binaryIds;
    for (const auto& b : spec.binaries) {
        if (b.id.empty()) { error(L"<binary> without an id"); continue; }
        if (!binaryIds.insert(lower(b.id)).second)
            error(L"duplicate binary id: " + b.id);
        if (b.data.empty()) warn(L"binary " + b.id + L" is empty");
    }

    std::set<std::wstring> dialogIds;
    for (const auto& d : spec.dialogs) {
        if (d.id.empty()) { error(L"<dialog> without an id"); continue; }
        if (!dialogIds.insert(lower(d.id)).second) {
            error(L"duplicate dialog id: " + d.id);
            continue;
        }
        if (d.controls.empty()) error(L"dialog " + d.id + L" has no controls");
        if (d.width <= 0 || d.height <= 0)
            error(L"dialog " + d.id + L" has an invalid size");

        std::set<std::wstring> controlIds;
        for (const auto& c : d.controls) {
            if (c.id.empty()) { error(L"dialog " + d.id + L": <control> without an id"); continue; }
            if (!controlIds.insert(lower(c.id)).second)
                error(L"dialog " + d.id + L": duplicate control " + c.id);
            if (!isControlType(c.type))
                error(L"dialog " + d.id + L": unsupported control type '" + c.type +
                      L"' on " + c.id);
            if ((c.width <= 0 || c.height <= 0) &&
                !util::iequals(c.type, L"Line"))
                warn(L"dialog " + d.id + L": control " + c.id +
                     L" has a zero / negative size");
            if (c.hasBinary && !c.binary.empty() && !binaryIds.count(lower(c.binary)))
                error(L"dialog " + d.id + L": control " + c.id +
                      L" references unknown binary '" + c.binary + L"'");
            if ((util::iequals(c.type, L"Bitmap") || util::iequals(c.type, L"Icon")) &&
                !c.hasText && !c.hasBinary)
                error(L"dialog " + d.id + L": " + c.type + L" control " + c.id +
                      L" needs text (binary name) or a binary attribute");
            if (util::iequals(c.type, L"RadioButtonGroup")) {
                if (!c.hasProperty || c.property.empty())
                    error(L"dialog " + d.id + L": RadioButtonGroup " + c.id +
                          L" needs a property");
                if (c.items.empty())
                    error(L"dialog " + d.id + L": RadioButtonGroup " + c.id +
                          L" has no <item> rows");
            }
            if (util::iequals(c.type, L"CheckBox") &&
                (!c.hasProperty || c.property.empty()))
                error(L"dialog " + d.id + L": CheckBox " + c.id +
                      L" needs a property");
            if ((util::iequals(c.type, L"ListBox") || util::iequals(c.type, L"ComboBox") ||
                 util::iequals(c.type, L"ListView")) && !c.items.empty() &&
                (!c.hasProperty || c.property.empty()))
                error(L"dialog " + d.id + L": " + c.type + L" " + c.id +
                      L" needs a property for its items");

            for (const auto& p : c.publish) {
                if (p.event.empty())
                    error(L"dialog " + d.id + L": empty event on " + c.id);
                else if (!isEventName(p.event))
                    error(L"dialog " + d.id + L": unknown event '" + p.event +
                          L"' on " + c.id);
                if (util::iequals(p.event, L"NewDialog") ||
                    util::iequals(p.event, L"SpawnDialog") ||
                    util::iequals(p.event, L"SpawnWaitDialog")) {
                    if (p.value.empty())
                        error(L"dialog " + d.id + L": " + p.event + L" on " + c.id +
                              L" needs a target dialog");
                } else if (util::iequals(p.event, L"EndDialog")) {
                    if (!util::iequals(p.value, L"Return") &&
                        !util::iequals(p.value, L"Exit") &&
                        !util::iequals(p.value, L"Suspend") &&
                        !util::iequals(p.value, L"Resume") &&
                        !util::iequals(p.value, L"Ignore") &&
                        !util::iequals(p.value, L"Retry") &&
                        !util::iequals(p.value, L"ErrorNo") &&
                        !util::iequals(p.value, L"ErrorYes") &&
                        !util::iequals(p.value, L"ErrorAbort") &&
                        !util::iequals(p.value, L"ErrorCancel") &&
                        !util::iequals(p.value, L"ErrorIgnore") &&
                        !util::iequals(p.value, L"ErrorOk") &&
                        !util::iequals(p.value, L"ErrorRetry") &&
                        !util::iequals(p.value, L"ErrorFatal"))
                        error(L"dialog " + d.id + L": invalid EndDialog value '" +
                              p.value + L"' on " + c.id);
                } else if (util::iequals(p.event, L"DoAction")) {
                    if (!isStandardAction(p.value) && !actionIds.count(lower(p.value)))
                        error(L"dialog " + d.id + L": DoAction '" + p.value +
                              L"' is neither a standard action nor a defined custom "
                              L"action");
                } else if (util::iequals(p.event, L"SetTargetPath")) {
                    if (p.value.empty())
                        error(L"dialog " + d.id + L": SetTargetPath on " + c.id +
                              L" needs a property argument");
                }
            }
            for (const auto& cc : c.conditions) {
                if (!util::iequals(cc.action, L"enable") &&
                    !util::iequals(cc.action, L"disable") &&
                    !util::iequals(cc.action, L"show") &&
                    !util::iequals(cc.action, L"hide") &&
                    !util::iequals(cc.action, L"default"))
                    error(L"dialog " + d.id + L": invalid ControlCondition action '" +
                          cc.action + L"' on " + c.id);
                if (cc.condition.empty())
                    error(L"dialog " + d.id + L": empty ControlCondition on " + c.id);
            }
        }

        for (const auto& c : d.controls) {
            if (c.hasNext && !c.next.empty() && !controlIds.count(lower(c.next)))
                error(L"dialog " + d.id + L": control " + c.id +
                      L" points at unknown Control_Next '" + c.next + L"'");
        }
        if (d.hasFirst && !d.first.empty() && !controlIds.count(lower(d.first)))
            error(L"dialog " + d.id + L": Control_First '" + d.first + L"' does not exist");
        if (d.hasDefault && !d.def.empty() && !controlIds.count(lower(d.def)))
            error(L"dialog " + d.id + L": Control_Default '" + d.def + L"' does not exist");
        if (d.hasCancel && !d.cancel.empty() && !controlIds.count(lower(d.cancel)))
            error(L"dialog " + d.id + L": Control_Cancel '" + d.cancel + L"' does not exist");

        // Windows Installer requires the Control_Next chain to be a single
        // loop; a fork or a second loop makes the dialog fail with error 2810.
        std::unordered_map<std::wstring, std::wstring> nextOf;
        std::unordered_map<std::wstring, int> indegree;
        for (const auto& c : d.controls)
            if (c.hasNext && !c.next.empty())
                nextOf[lower(c.id)] = lower(c.next);
        for (const auto& kv : nextOf) indegree[kv.second]++;
        for (const auto& kv : indegree)
            if (kv.second > 1)
                error(L"dialog " + d.id + L": Control_Next chain has a fork at '" +
                      kv.first + L"'");
        if (!nextOf.empty()) {
            std::wstring start = nextOf.begin()->first;
            if (d.hasFirst && nextOf.count(lower(d.first))) start = lower(d.first);
            std::set<std::wstring> seen;
            std::wstring cur = start;
            bool closed = false;
            for (size_t guard = 0; guard <= nextOf.size(); ++guard) {
                if (!seen.insert(cur).second) break;
                auto it = nextOf.find(cur);
                if (it == nextOf.end()) break;
                cur = it->second;
                if (cur == start) { closed = true; break; }
            }
            if (!closed)
                error(L"dialog " + d.id + L": Control_Next chain is not a single loop "
                      L"(Windows Installer error 2810)");
            else if (seen.size() != nextOf.size())
                error(L"dialog " + d.id + L": Control_Next chain is disconnected");
        }
    }

    // Sequences.
    auto checkSequence = [&](const std::vector<SequenceEntry>& seq,
                             const wchar_t* name, bool present) {
        if (!present) return;
        std::set<std::wstring> actions;
        bool hasCostInit = false, hasCostFinal = false, hasExecute = false;
        for (const auto& e : seq) {
            if (e.action.empty()) { error(std::wstring(name) + L": action without a name"); continue; }
            if (!actions.insert(lower(e.action)).second)
                error(std::wstring(name) + L": duplicate action " + e.action);
            if (util::iequals(e.action, L"CostInitialize")) hasCostInit = true;
            if (util::iequals(e.action, L"CostFinalize")) hasCostFinal = true;
            if (util::iequals(e.action, L"ExecuteAction")) hasExecute = true;
            bool isDialog = dialogIds.count(lower(e.action)) != 0;
            if (!isDialog && !isStandardAction(e.action) &&
                !actionIds.count(lower(e.action)) &&
                !util::iequals(e.action, L"ExecuteAction") &&
                !util::iequals(e.action, L"FatalError") &&
                !util::iequals(e.action, L"UserExit") &&
                !util::iequals(e.action, L"ExitDialog") &&
                e.sequence >= 0)
                error(std::wstring(name) + L": unknown action '" + e.action + L"'");
            if ((util::iequals(e.action, L"FatalError") ||
                 util::iequals(e.action, L"UserExit") ||
                 util::iequals(e.action, L"ExitDialog")) &&
                !dialogIds.count(lower(e.action)))
                error(std::wstring(name) + L": termination dialog '" + e.action +
                      L"' is not defined");
        }
        if (!spec.dialogs.empty()) {
            if (!hasCostInit || !hasCostFinal || !hasExecute)
                error(std::wstring(name) + L": sequence must contain CostInitialize, "
                      L"CostFinalize and ExecuteAction");
        }
    };
    checkSequence(spec.sequence, L"InstallUISequence", spec.hasSequence);
    checkSequence(spec.adminSequence, L"AdminUISequence", spec.hasAdminSequence);

    for (const auto& b : spec.billboards) {
        if (b.id.empty()) error(L"<billboard> without an id");
        for (const auto& c : b.controls)
            if (c.id.empty()) error(L"billboard " + b.id + L": control without an id");
    }

    static const wchar_t* reserved[] = {
        L"ProductName", L"ProductVersion", L"ProductCode", L"UpgradeCode",
        L"Manufacturer", L"ProductLanguage", L"ALLUSERS", L"INSTALLLEVEL",
        L"INSTALLFOLDER",
    };
    for (const auto& p : spec.properties) {
        for (const wchar_t* r : reserved)
            if (util::iequals(p.name, r))
                error(L"<ui> must not override the reserved property " + p.name);
    }
}

// ---------------------------------------------------------------------------
// Database writer
// ---------------------------------------------------------------------------

void applyUi(MSIHANDLE db, const UiSpec& spec, const std::wstring& tempDir,
             const std::wstring& productName) {
    (void)productName;
    if (spec.empty()) return;

    // Styles.
    for (const auto& s : spec.styles) {
        insertRow(db, L"TextStyle",
                  {L"TextStyle", L"FaceName", L"Size", L"Color", L"StyleBits"},
                  {S(s.id), S(s.face), I(s.size),
                   s.hasColor ? I(s.color) : Null(),
                   s.hasStyle ? I(s.style) : Null()});
    }

    // Binary streams: MsiRecordSetStream needs a file path, so the bytes are
    // staged in the packer's temp directory and removed with it.
    for (size_t i = 0; i < spec.binaries.size(); ++i) {
        const BinaryDef& b = spec.binaries[i];
        std::wstring file = util::path::join(
            tempDir, L"ui_bin_" + std::to_wstring(i + 1) + L".bin");
        if (!util::path::writeFile(file, b.data))
            util::fail(L"Cannot stage UI binary " + b.id + L": " + file);
        insertStreamRow(db, L"Binary", L"Name", L"Data", b.id, file);
    }

    // Dialogs + controls.
    for (const auto& d : spec.dialogs) {
        std::wstring first = d.first;        if (first.empty() && !d.controls.empty()) first = d.controls.front().id;
        insertRow(db, L"Dialog",
                  {L"Dialog", L"HCentering", L"VCentering", L"Width", L"Height",
                   L"Attributes", L"Title", L"Control_First", L"Control_Default",
                   L"Control_Cancel"},
                  {S(d.id), I(50), I(50), I(d.width), I(d.height),
                   I(d.attributes), d.hasTitle ? S(d.title) : Null(), S(first),
                   d.hasDefault && !d.def.empty() ? S(d.def) : Null(),
                   d.hasCancel && !d.cancel.empty() ? S(d.cancel) : Null()});

        for (const auto& c : d.controls) {
            std::wstring text;
            if (c.hasBinary) text = c.binary;
            else if (c.hasText) text = c.text;
            insertRow(db, L"Control",
                      {L"Dialog_", L"Control", L"Type", L"X", L"Y", L"Width",
                       L"Height", L"Attributes", L"Property", L"Text",
                       L"Control_Next", L"Help"},
                      {S(d.id), S(c.id), S(c.type), I(c.x), I(c.y), I(c.width),
                       I(c.height), I(effectiveAttributes(c)),
                       c.hasProperty && !c.property.empty() ? S(c.property) : Null(),
                       text.empty() ? Null() : S(text),
                       c.hasNext && !c.next.empty() ? S(c.next) : Null(),
                       c.hasHelp && !c.help.empty() ? S(c.help) : Null()});

            int order = 0;
            for (const auto& p : c.publish) {
                int o = p.hasOrder ? p.order : ++order;
                insertRow(db, L"ControlEvent",
                          {L"Dialog_", L"Control_", L"Event", L"Argument",
                           L"Condition", L"Ordering"},
                          {S(d.id), S(c.id), S(p.event), S(p.value),
                           S(p.condition.empty() ? L"1" : p.condition), I(o)});
            }
            for (const auto& cc : c.conditions) {
                std::wstring action = cc.action;
                if (util::iequals(action, L"enable")) action = L"Enable";
                else if (util::iequals(action, L"disable")) action = L"Disable";
                else if (util::iequals(action, L"show")) action = L"Show";
                else if (util::iequals(action, L"hide")) action = L"Hide";
                else if (util::iequals(action, L"default")) action = L"Default";
                insertRow(db, L"ControlCondition",
                          {L"Dialog_", L"Control_", L"Action", L"Condition"},
                          {S(d.id), S(c.id), S(action), S(cc.condition)});
            }

            if (util::iequals(c.type, L"RadioButtonGroup")) {
                int n = 0;
                for (const auto& item : c.items) {
                    ++n;
                    insertRow(db, L"RadioButton",
                              {L"Property", L"Order", L"Value", L"X", L"Y",
                               L"Width", L"Height", L"Text", L"Help"},
                              {S(c.property), I(n), S(item.value), I(c.x), I(c.y),
                               I(c.width), I(c.height),
                               item.text.empty() ? Null() : S(item.text),
                               item.help.empty() ? Null() : S(item.help)});
                }
            } else if (util::iequals(c.type, L"ComboBox") ||
                       util::iequals(c.type, L"ListBox") ||
                       util::iequals(c.type, L"ListView")) {
                const wchar_t* table = util::iequals(c.type, L"ComboBox") ? L"ComboBox"
                                       : util::iequals(c.type, L"ListBox") ? L"ListBox"
                                                                           : L"ListView";
                int n = 0;
                for (const auto& item : c.items) {
                    ++n;
                    insertRow(db, table, {L"Property", L"Order", L"Value", L"Text"},
                              {S(c.property), I(n), S(item.value),
                               item.text.empty() ? Null() : S(item.text)});
                }
            }
        }
    }

    for (const auto& e : spec.errors)
        insertRow(db, L"Error", {L"Error", L"Text"},
                  {I(e.id), e.text.empty() ? Null() : S(e.text)});

    // CheckBox table: control property -> value when checked. Default "1"
    // so a plain <control type="CheckBox"> just works.
    std::map<std::wstring, std::wstring> checkValues;
    for (const auto& cb : spec.checkBoxes) checkValues[lower(cb.name)] = cb.value;
    for (const auto& d : spec.dialogs) {
        for (const auto& c : d.controls) {
            if (!util::iequals(c.type, L"CheckBox")) continue;
            if (!c.hasProperty || c.property.empty()) continue;
            auto it = checkValues.find(lower(c.property));
            insertRow(db, L"CheckBox", {L"Property", L"Value"},
                      {S(c.property),
                       S(it == checkValues.end() ? std::wstring(L"1") : it->second)});
        }
    }

    for (const auto& b : spec.billboards) {
        insertRow(db, L"Billboard",
                  {L"Billboard", L"Feature_", L"Action", L"Ordering"},
                  {S(b.id), S(b.feature.empty() ? std::wstring(L"MainFeature") : b.feature),
                   b.action.empty() ? Null() : S(b.action),
                   b.ordering ? I(b.ordering) : Null()});
        for (const auto& c : b.controls)
            insertRow(db, L"BBControl",
                      {L"Billboard_", L"BBControl", L"Type", L"X", L"Y", L"Width",
                       L"Height", L"Attributes", L"Text"},
                      {S(b.id), S(c.id), S(c.type), I(c.x), I(c.y), I(c.width),
                       I(c.height), I(c.hasAttributes ? c.attributes : 1),
                       c.text.empty() ? Null() : S(c.text)});
    }

    // Properties: DefaultUIFont plus any UI-scoped property (the install-dir
    // property, ARPNOREPAIR, ...). Later duplicates win.
    std::vector<PropertyDef> props = spec.properties;
    if (!spec.font.empty()) {
        auto it = std::find_if(props.begin(), props.end(),
                               [&](const PropertyDef& p) {
                                   return util::iequals(p.name, L"DefaultUIFont");
                               });
        if (it == props.end()) props.push_back({L"DefaultUIFont", spec.font});
        else it->value = spec.font;
    }
    std::set<std::wstring> seen;
    for (const auto& p : props) {
        if (p.name.empty()) continue;
        if (!seen.insert(lower(p.name)).second) continue;
        insertRow(db, L"Property", {L"Property", L"Value"}, {S(p.name), S(p.value)});
    }

    auto writeSequence = [&](const std::vector<SequenceEntry>& seq,
                             const wchar_t* table) {
        for (const auto& e : seq)
            insertRow(db, table, {L"Action", L"Condition", L"Sequence"},
                      {S(e.action),
                       e.hasCondition && !e.condition.empty() ? S(e.condition) : Null(),
                       I(e.sequence)});
    };
    if (spec.hasSequence) writeSequence(spec.sequence, L"InstallUISequence");
    if (spec.hasAdminSequence) writeSequence(spec.adminSequence, L"AdminUISequence");
}

}  // namespace pack
