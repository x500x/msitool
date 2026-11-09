#pragma once

// ---------------------------------------------------------------------------
// Custom action editor model
//
// `CustomActionSpec` is the high-level, XML-shaped description of one
// CustomAction row: the XML manifest loader and the GUI editor both speak it,
// while the packer only ever sees the encoded `CustomActionDef` (Type / Source
// / Target / Binary stream). Keeping the two representations separate lets the
// editor show friendly type names, per-type fields, flags and sequence phases
// instead of raw MSI bit masks.
// ---------------------------------------------------------------------------

#include "util.h"
#include "pack_ui.h"
#include "xml.h"

#include <string>
#include <utility>
#include <vector>

namespace pack {

struct CustomActionSpec {
    std::wstring id;
    // One of kCaTypes[].id, or a decimal string for a raw MSI type.
    std::wstring type = L"setproperty";
    std::wstring binary;      // dll / exe: file embedded as a Binary stream
    std::wstring file;        // dllfile / exefile: installed File key
    std::wstring entry;       // DLL entry point
    std::wstring target;      // command line / Target column
    std::wstring args;        // legacy alias for target
    std::wstring property;    // setproperty / runcommand source property
    std::wstring directory;   // exedir / setdirectory source directory
    std::wstring value;       // setproperty / setdirectory Target
    std::wstring source;      // legacy alias for file / directory / property

    bool deferred = false, commit = false, rollback = false;
    bool asyncRun = false, continueOnError = false, taware = false;
    bool x64 = false, hideTarget = false, noImpersonate = false;

    // InstallExecuteSequence placement. 0 = the action is defined but not
    // scheduled (useful for UI DoAction events).
    int          sequence = 0;
    std::wstring condition;
};

struct CaType {
    const wchar_t* id;
    const wchar_t* label;
    int            base;      // low nibble + source bits of CustomAction.Type
};

struct CaPhase {
    const wchar_t* label;
    int            sequence;
};

struct CaIssue {
    int          row = -1;        // -1 = whole-list issue
    bool         error = false;   // false = warning
    std::wstring text;
};

// Type / phase tables used to build the editor's drop-downs.
const CaType*  caTypes();
int            caTypeCount();
const CaPhase* caPhases();
int            caPhaseCount();
// Display name for any type string (friendly name, or "原始类型 <n>").
std::wstring   caTypeLabel(const std::wstring& type);
// Fields relevant to a type (see caFieldMask()).
enum CaField {
    CaBinary = 1 << 0, CaEntry = 1 << 1, CaFile = 1 << 2,
    CaDirectory = 1 << 3, CaProperty = 1 << 4, CaValue = 1 << 5,
    CaTarget = 1 << 6
};
int caFieldMask(const std::wstring& type);

// Standard InstallExecuteSequence actions, used for the visual timeline and
// for sequence-number collision warnings.
std::vector<std::pair<std::wstring, int>> standardSequence();

// Parses a <customaction> element into the editor model.
CustomActionSpec parseCustomActionSpec(const xml::Node& node);

// Encodes the model into a CustomActionDef. `baseDir` resolves relative
// `binary` paths. Throws util::Error on missing / invalid fields.
CustomActionDef compileCustomAction(const CustomActionSpec& spec,
                                    const std::wstring& baseDir);

// Reads just <customactions> + <executeSequence> from an XML manifest so the
// GUI editor can load an existing configuration. Sequence rows are attached
// to the matching action id.
std::vector<CustomActionSpec> loadCustomActionSpecs(const std::wstring& path);

// Real-time validation of the whole list.
std::vector<CaIssue> lintCustomActions(const std::vector<CustomActionSpec>& rows,
                                       const std::wstring& baseDir = L"");

// Serialises the list as XML. With `document` it is a complete manifest that
// can be passed to --config; otherwise it is a fragment ready to paste into
// an existing <msitool> manifest.
std::wstring customActionsToXml(const std::vector<CustomActionSpec>& rows,
                                bool document);

}  // namespace pack
