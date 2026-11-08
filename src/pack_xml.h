#pragma once

#include "pack_ui.h"
#include "util.h"
#include "xml.h"

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pack {

struct FeatureDef {
    std::wstring id, title, description, dirs;
    int          level = 1, display = 0, attributes = 0;
};

struct ShortcutPropertyDef {
    std::wstring key, value;
};

struct ShortcutDef {
    std::wstring id, name, dir, target, args, description, icon, wkdir, component;
    int          iconIndex = 0;
    bool         advertise = false;
    std::vector<ShortcutPropertyDef> properties;
};

struct RegistryDef {
    std::wstring id, root, key, name, value, type, component;
};

struct ServiceDef {
    std::wstring id, name, displayName, image, type, start, error, account,
                 password, args, dependencies, description, component;
};

struct EnvDef {
    std::wstring id, name, value, action, separator, system, component;
};

struct SignDef {
    std::wstring cert, password, timestamp, tool;
    bool         present = false;
};

struct PackDef {
    std::wstring name, version, manufacturer, productCode, upgradeCode;
    std::wstring platform, installDir, ui, largeFiles;
    int          language = 0;
    bool         hasLanguage = false;
    unsigned long long cabSize = 0;
    bool         hasCabSize = false;
    bool         keepEmptyDirs = true;
};

struct Manifest {
    PackDef                  pack;
    std::vector<FeatureDef>  features;
    std::vector<ShortcutDef> shortcuts;
    std::vector<RegistryDef> registry;
    std::vector<ServiceDef>  services;
    std::vector<EnvDef>      environment;
    SignDef                  sign;
    UiSpec                   ui;
    std::wstring             dir;      // directory of the manifest file
    std::wstring             path;
};

// Parses a pack manifest (XML only).
Manifest loadManifestXml(const std::wstring& path);

// Locale string table used by the built-in wizard and by <strings> sections.
struct LocaleStrings {
    std::map<std::wstring, std::wstring> map;   // lower(id) -> text
};

LocaleStrings loadLocaleStrings(const xml::Node& root);
std::wstring applyLocaleStrings(const std::wstring& text,
                                const LocaleStrings& strings,
                                const std::wstring& context);

// Converts a <ui> element (plus optional string table and base directory for
// file references) into the UI model.
UiSpec parseUiNode(const xml::Node& uiNode, const LocaleStrings& strings,
                   const std::wstring& baseDir);

// Imports the UI tables of an existing MSI (Dialog / Control / ControlEvent /
// ControlCondition / TextStyle / Binary / RadioButton / ... / InstallUISequence).
UiSpec loadUiTemplate(const std::wstring& msiPath);

// 1033 -> en, 2052 / 1028 -> zh-CN, everything else -> en.
std::wstring localeForLanguage(int lcid);

}  // namespace pack
