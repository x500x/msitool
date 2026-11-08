#pragma once

#include "util.h"

#include <msi.h>
#include <msiquery.h>

#include <string>
#include <vector>

namespace pack {

// ---------------------------------------------------------------------------
// Small MSI row helpers shared by the packer and the UI generator.
// ---------------------------------------------------------------------------

struct Cell {
    bool         isNull = true;
    bool         isInt = false;
    int          i = 0;
    std::wstring s;
};

Cell S(std::wstring v);
Cell I(int v);
Cell Null();

void execSql(MSIHANDLE db, const std::wstring& sql);
void insertRow(MSIHANDLE db, const std::wstring& table,
               const std::vector<std::wstring>& cols,
               const std::vector<Cell>& cells);
void insertStreamRow(MSIHANDLE db, const std::wstring& table,
                     const std::wstring& nameCol, const std::wstring& dataCol,
                     const std::wstring& name, const std::wstring& file);

// ---------------------------------------------------------------------------
// Text / identifier helpers
// ---------------------------------------------------------------------------

std::wstring newGuid();
std::wstring normalizeGuid(std::wstring s);
std::wstring toIdentifier(std::wstring s, size_t maxLen = 72);
std::wstring sanitizeName(std::wstring name);
std::wstring normRel(std::wstring s);

bool parseBool(const std::wstring& s, bool dflt);
int  parseInt(const std::wstring& s, int dflt);
std::vector<std::wstring> splitList(const std::wstring& s);

// "16", "16M", "512K", "1G" (1024 based) -> bytes.
unsigned long long parseSize(const std::wstring& text);

// ---------------------------------------------------------------------------
// Standard Windows Installer folder keys
// ---------------------------------------------------------------------------

struct StandardDir {
    const wchar_t* key;
    const wchar_t* parent;
    const wchar_t* def;
};

const StandardDir* findStandardDir(const std::wstring& name);

}  // namespace pack
