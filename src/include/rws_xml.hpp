//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_xml.hpp
//
// A small namespace-aware pull parser for the ODM subset that Rave Web
// Services returns: elements, attributes and whitespace. Mixed content, DTDs
// and external entities are rejected rather than interpreted.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"

namespace duckdb {

//! Namespace URIs, matched by URI and never by prefix.
static constexpr const char *ODM_NS = "http://www.cdisc.org/ns/odm/v1.3";
static constexpr const char *MDSOL_NS = "http://www.mdsol.com/ns/odm/metadata";

struct XmlAttribute {
	//! Resolved namespace URI, empty for unprefixed attributes.
	string uri;
	string local_name;
	string value;
};

class XmlReader {
public:
	enum class Event { START_ELEMENT, END_ELEMENT, END_OF_DOCUMENT };

public:
	//! `source` is used only in error messages.
	XmlReader(const char *data, idx_t size, string source);

	//! Advances to the next element event. Returns false at end of document.
	bool Read();

	Event GetEvent() const {
		return event;
	}
	const string &LocalName() const {
		return local_name;
	}
	const string &NamespaceUri() const {
		return uri;
	}
	idx_t Depth() const {
		return depth;
	}
	//! True when the current START_ELEMENT was written as <tag/>; no matching
	//! END_ELEMENT event is produced for it.
	bool IsEmptyElement() const {
		return empty_element;
	}

	bool IsElement(const char *ns, const char *name) const {
		return event == Event::START_ELEMENT && uri == ns && local_name == name;
	}

	//! Attribute lookup on the current start element. `ns` may be empty for
	//! plain (unprefixed) attributes.
	bool TryGetAttribute(const char *ns, const char *name, string &result) const;
	string GetAttribute(const char *ns, const char *name) const;

	//! All attributes of the current start element, with prefixes resolved.
	const vector<XmlAttribute> &Attributes() const {
		return attributes;
	}

	//! Skips over the current element and everything nested inside it.
	void SkipElement();

	//! Reads the character data of the current element and consumes its end
	//! tag. Throws when the element has child elements: the ODM subset that
	//! carries text never mixes the two.
	string ReadTextContent();

private:
	void SkipWhitespace();
	void SkipMisc();
	void ParseStartElement();
	void ParseEndElement();
	string ParseAttributeValue();
	string ParseName();
	string Unescape(const string &raw) const;
	string ResolvePrefix(const string &prefix, bool is_element) const;
	[[noreturn]] void Fail(const string &message) const;

private:
	struct NamespaceBinding {
		idx_t depth;
		string prefix;
		string uri;
	};

	const char *data;
	idx_t size;
	idx_t pos = 0;
	string source;

	Event event = Event::END_OF_DOCUMENT;
	string local_name;
	string uri;
	bool empty_element = false;
	idx_t depth = 0;

	vector<XmlAttribute> attributes;
	vector<NamespaceBinding> namespace_stack;
	vector<string> element_stack;
	//! Set when a self-closing element still owes us its END_ELEMENT event.
	bool pending_close = false;
};

} // namespace duckdb
