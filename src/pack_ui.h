#pragma once

#include "util.h"

#include <msi.h>

#include <string>
#include <vector>

namespace pack {

// ---------------------------------------------------------------------------
// Installer UI model
//
// The model mirrors the Windows Installer tables but keeps everything in one
// tree (dialog -> control -> publish/condition) so the XML loader, the
// built-in wizard and the merge/validation code share one representation.
// ---------------------------------------------------------------------------

struct TextStyleDef {
    std::wstring id;
    std::wstring face;
    int          size = 8;
    int          color = 0;
    int          style = 0;
    bool         hasColor = false;
    bool         hasStyle = false;
};

struct ListItemDef {
    std::wstring value;
    std::wstring text;
    std::wstring help;
};

struct PublishDef {
    std::wstring event;
    std::wstring value;
    std::wstring condition = L"1";
    int          order = 0;
    bool         hasOrder = false;
};

struct ConditionDef {
    std::wstring action;      // enable / disable / show / hide
    std::wstring condition;
};

struct ControlDef {
    std::wstring id;
    std::wstring type;

    int  x = 0, y = 0, width = 0, height = 0;
    bool hasX = false, hasY = false, hasWidth = false, hasHeight = false;

    int  attributes = 3;              // Visible | Enabled
    bool hasAttributes = false;
    bool visible = true;
    bool enabled = true;
    bool sunken = false;
    bool indirect = false;
    bool hasVisible = false, hasEnabled = false, hasSunken = false, hasIndirect = false;

    std::wstring property, text, binary, next, help;
    bool hasProperty = false, hasText = false, hasBinary = false,
         hasNext = false, hasHelp = false;

    std::vector<PublishDef>   publish;
    std::vector<ConditionDef> conditions;
    std::vector<ListItemDef>  items;   // RadioButtonGroup / ListBox / ComboBox / ListView
};

struct DialogDef {
    std::wstring id;
    std::wstring title;
    std::wstring first, def, cancel;
    std::wstring condition;
    int          width = 370, height = 270;
    int          attributes = 7;       // Visible | Modal | Minimize
    int          sequence = 0;
    bool         hasTitle = false, hasWidth = false, hasHeight = false,
                 hasAttributes = false, hasFirst = false, hasDefault = false,
                 hasCancel = false, hasSequence = false, hasCondition = false;
    std::vector<ControlDef> controls;
};

struct SequenceEntry {
    std::wstring action;
    std::wstring condition;
    int          sequence = 0;
    bool         hasCondition = false;
};

struct PropertyDef {
    std::wstring name;
    std::wstring value;
};

struct BinaryDef {
    std::wstring                id;
    std::vector<unsigned char>  data;
};

struct ErrorDef {
    int          id = 0;
    std::wstring text;
};

struct BillboardControlDef {
    std::wstring id, type, text;
    int          x = 0, y = 0, width = 0, height = 0;
    int          attributes = 1;
    bool         hasAttributes = false;
};

struct BillboardDef {
    std::wstring id, feature, action;
    int          ordering = 1;
    std::vector<BillboardControlDef> controls;
};

struct UiIssue {
    bool         error = false;
    std::wstring message;
};

// A CustomAction row. `type` is the already encoded MSI type; `source` and
// `target` match the CustomAction table columns.
struct CustomActionDef {
    std::wstring id;
    int          type = 0;
    std::wstring source;
    std::wstring target;
    std::wstring binaryFile;   // file to embed as a Binary stream
    bool         hasBinary = false;
};

struct UiSpec {
    std::wstring base;          // "", "minimal", "wizard"
    std::wstring locale;        // en / zh-CN
    std::wstring font = L"DlgFont8";
    bool         hasFont = false;
    std::wstring templateMsi;
    bool         hasTemplate = false;

    std::vector<TextStyleDef>  styles;
    std::vector<DialogDef>     dialogs;
    std::vector<SequenceEntry> sequence;
    std::vector<SequenceEntry> adminSequence;
    std::vector<PropertyDef>   properties;
    std::vector<BinaryDef>     binaries;
    std::vector<ErrorDef>      errors;
    std::vector<BillboardDef>  billboards;
    std::vector<PropertyDef>   checkBoxes;   // CheckBox table: Property -> Value
    std::vector<CustomActionDef> customActions;  // imported UI-template actions
    bool hasSequence = false;
    bool hasAdminSequence = false;

    bool empty() const {
        return dialogs.empty() && styles.empty() && binaries.empty() &&
               errors.empty() && billboards.empty() &&
               !hasSequence && !hasAdminSequence;
    }

    DialogDef*       findDialog(const std::wstring& id);
    const DialogDef* findDialog(const std::wstring& id) const;
    ControlDef*      findControl(const std::wstring& dialogId, const std::wstring& id);
};

struct UiTables {
    bool dialog = false;
    bool control = false;
    bool controlEvent = false;
    bool controlCondition = false;
    bool textStyle = false;
    bool binary = false;
    bool radioButton = false;
    bool checkBox = false;
    bool comboBox = false;
    bool listBox = false;
    bool listView = false;
    bool error = false;
    bool billboard = false;
    bool installSequence = false;
    bool adminSequence = false;
};

UiTables uiTablesFor(const UiSpec& spec);
std::wstring uiTableNames(const UiTables& t);

// Built-in specs.
UiSpec builtinMinimalUi();
// Extracts the embedded wizard resources into `tempDir` and parses them for
// the selected locale (en / zh-CN).
UiSpec builtinWizardUi(const std::wstring& locale, const std::wstring& tempDir);
// Writes the embedded wizard layout, locales, EULA and bitmaps to `outDir`.
bool dumpBuiltinUi(const std::wstring& outDir, std::wstring& error);

// Overlay `overlay` onto `base` (same-id rows are merged, new rows appended).
void mergeUi(UiSpec& base, const UiSpec& overlay);

// Semantic checks. Errors must abort before the database is written; warnings
// are printed by the caller.
void validateUi(const UiSpec& spec, const std::vector<std::wstring>& customActions,
                std::vector<UiIssue>& issues);

// Creates the UI rows (tables must exist already).
void applyUi(MSIHANDLE db, const UiSpec& spec, const std::wstring& tempDir,
             const std::wstring& productName);

}  // namespace pack
