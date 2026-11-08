#pragma once

#include "util.h"

#include <string>
#include <utility>
#include <vector>

namespace xml {

// A minimal read-only DOM: elements, attributes and direct text. Everything
// the packer needs, without exposing MSXML to the rest of the code.
struct Node {
    std::wstring name;   // local name, without a namespace prefix
    std::wstring ns;     // namespace URI (empty when none)
    std::vector<std::pair<std::wstring, std::wstring>> attrs;
    std::wstring text;   // concatenated direct text/CDATA children
    std::vector<Node> children;

    bool has(const std::wstring& attrName) const;
    std::wstring attr(const std::wstring& attrName,
                      const std::wstring& dflt = L"") const;
    // Throws util::Error mentioning `ctx` when the attribute is missing.
    std::wstring require(const std::wstring& attrName,
                         const std::wstring& ctx) const;
    const Node* child(const std::wstring& childName) const;
    std::vector<const Node*> childList(const std::wstring& childName) const;
};

// Parses a UTF-8 / UTF-16 XML file. DTDs and external entities are disabled
// (XXE-safe); parse errors carry line / column information.
Node parseFile(const std::wstring& path);

// Parses an in-memory document. An XML declaration is stripped so MSXML does
// not try to switch encodings on a BSTR.
Node parseString(const std::wstring& xml, const std::wstring& origin);

}  // namespace xml
