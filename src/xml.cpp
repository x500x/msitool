#include "xml.h"

#include <msxml6.h>

namespace xml {

namespace {

std::wstring bstrToString(BSTR b) {
    std::wstring s = b ? b : L"";
    if (b) SysFreeString(b);
    return s;
}

std::wstring parseErrorText(IXMLDOMDocument3* doc, const std::wstring& origin) {
    std::wstring msg = L"Cannot parse XML";
    if (!origin.empty()) msg += L" (" + origin + L")";
    IXMLDOMParseError* err = nullptr;
    if (doc && SUCCEEDED(doc->get_parseError(&err)) && err) {
        BSTR reason = nullptr;
        long line = 0, col = 0;
        err->get_reason(&reason);
        err->get_line(&line);
        err->get_linepos(&col);
        std::wstring r = bstrToString(reason);
        if (!r.empty())
            msg += util::fmt(L": line %ld, column %ld: ", line, col) + r;
        err->Release();
    }
    return msg;
}

Node convert(IXMLDOMNode* node) {
    Node out;
    BSTR name = nullptr;
    if (SUCCEEDED(node->get_baseName(&name))) out.name = bstrToString(name);
    if (out.name.empty()) {
        BSTR nodeName = nullptr;
        if (SUCCEEDED(node->get_nodeName(&nodeName))) out.name = bstrToString(nodeName);
    }
    BSTR ns = nullptr;
    if (SUCCEEDED(node->get_namespaceURI(&ns))) out.ns = bstrToString(ns);

    IXMLDOMNamedNodeMap* attrs = nullptr;
    if (SUCCEEDED(node->get_attributes(&attrs)) && attrs) {
        long count = 0;
        if (SUCCEEDED(attrs->get_length(&count))) {
            for (long i = 0; i < count; ++i) {
                IXMLDOMNode* a = nullptr;
                if (FAILED(attrs->get_item(i, &a)) || !a) continue;
                BSTR an = nullptr, av = nullptr;
                a->get_nodeName(&an);
                a->get_text(&av);
                out.attrs.emplace_back(bstrToString(an), bstrToString(av));
                a->Release();
            }
        }
        attrs->Release();
    }

    IXMLDOMNodeList* children = nullptr;
    if (SUCCEEDED(node->get_childNodes(&children)) && children) {
        long count = 0;
        if (SUCCEEDED(children->get_length(&count))) {
            for (long i = 0; i < count; ++i) {
                IXMLDOMNode* c = nullptr;
                if (FAILED(children->get_item(i, &c)) || !c) continue;
                DOMNodeType type = NODE_INVALID;
                c->get_nodeType(&type);
                if (type == NODE_ELEMENT) {
                    out.children.push_back(convert(c));
                } else if (type == NODE_TEXT || type == NODE_CDATA_SECTION) {
                    BSTR v = nullptr;
                    if (SUCCEEDED(c->get_text(&v))) out.text += bstrToString(v);
                }
                c->Release();
            }
        }
        children->Release();
    }
    return out;
}

Node finish(IXMLDOMDocument3* doc, const std::wstring& origin) {
    IXMLDOMElement* root = nullptr;
    if (FAILED(doc->get_documentElement(&root)) || !root)
        util::fail(parseErrorText(doc, origin));
    Node n = convert(root);
    root->Release();
    return n;
}

}  // namespace

bool Node::has(const std::wstring& attrName) const {
    for (const auto& a : attrs)
        if (util::iequals(a.first, attrName)) return true;
    return false;
}

std::wstring Node::attr(const std::wstring& attrName, const std::wstring& dflt) const {
    for (const auto& a : attrs)
        if (util::iequals(a.first, attrName)) return a.second;
    return dflt;
}

std::wstring Node::require(const std::wstring& attrName, const std::wstring& ctx) const {
    for (const auto& a : attrs)
        if (util::iequals(a.first, attrName)) return a.second;
    util::fail(L"<" + name + L"> is missing the required attribute '" + attrName +
               L"' (" + ctx + L")");
}

const Node* Node::child(const std::wstring& childName) const {
    for (const auto& c : children)
        if (util::iequals(c.name, childName)) return &c;
    return nullptr;
}

std::vector<const Node*> Node::childList(const std::wstring& childName) const {
    std::vector<const Node*> out;
    for (const auto& c : children)
        if (util::iequals(c.name, childName)) out.push_back(&c);
    return out;
}

namespace {

struct ComScope {
    HRESULT hr;
    ComScope() { hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); }
    ~ComScope() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
    bool ok() const { return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE; }
};

IXMLDOMDocument3* createDocument() {
    IXMLDOMDocument3* doc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_DOMDocument60, nullptr, CLSCTX_INPROC_SERVER,
                                  __uuidof(IXMLDOMDocument3),
                                  reinterpret_cast<void**>(&doc));
    if (FAILED(hr) || !doc) return nullptr;

    doc->put_async(VARIANT_FALSE);
    doc->put_resolveExternals(VARIANT_FALSE);
    doc->put_validateOnParse(VARIANT_FALSE);
    doc->put_preserveWhiteSpace(VARIANT_FALSE);

    // No DTD, no external entities: a manifest is data, not a document
    // type definition playground (XXE).
    VARIANT v;
    VariantInit(&v);
    V_VT(&v) = VT_BOOL;
    V_BOOL(&v) = VARIANT_TRUE;
    doc->setProperty(const_cast<BSTR>(L"ProhibitDTD"), v);
    VariantClear(&v);

    return doc;
}

}  // namespace

Node parseFile(const std::wstring& path) {
    ComScope com;
    if (!com.ok()) util::fail(L"Cannot initialise COM for the XML parser");
    IXMLDOMDocument3* doc = createDocument();
    if (!doc) util::fail(L"Cannot create the MSXML6 XML document");

    VARIANT src;
    VariantInit(&src);
    V_VT(&src) = VT_BSTR;
    V_BSTR(&src) = SysAllocString(path.c_str());
    VARIANT_BOOL ok = VARIANT_FALSE;
    doc->load(src, &ok);
    VariantClear(&src);

    std::wstring origin = util::path::fileName(path);
    Node n = finish(doc, origin);
    doc->Release();
    return n;
}

Node parseString(const std::wstring& xml, const std::wstring& origin) {
    ComScope com;
    if (!com.ok()) util::fail(L"Cannot initialise COM for the XML parser");
    IXMLDOMDocument3* doc = createDocument();
    if (!doc) util::fail(L"Cannot create the MSXML6 XML document");

    std::wstring body = xml;
    // loadXML() receives UTF-16: drop the declaration so MSXML does not try
    // to switch to the encoding named in it.
    size_t start = body.find_first_not_of(L" \t\r\n");
    if (start != std::wstring::npos && body.compare(start, 5, L"<?xml") == 0) {
        size_t end = body.find(L"?>", start);
        if (end != std::wstring::npos) body.erase(start, end + 2 - start);
    }

    BSTR b = SysAllocString(body.c_str());
    VARIANT_BOOL ok = VARIANT_FALSE;
    doc->loadXML(b, &ok);
    SysFreeString(b);

    Node n = finish(doc, origin);
    doc->Release();
    return n;
}

}  // namespace xml
