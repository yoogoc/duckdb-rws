#include "rws_xml.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

XmlReader::XmlReader(const char *data, idx_t size, string source) : data(data), size(size), source(std::move(source)) {
	// Tolerate a UTF-8 BOM: RWS emits one on most endpoints.
	if (size >= 3 && static_cast<unsigned char>(data[0]) == 0xEF && static_cast<unsigned char>(data[1]) == 0xBB &&
	    static_cast<unsigned char>(data[2]) == 0xBF) {
		pos = 3;
	}
}

void XmlReader::Fail(const string &message) const {
	// Never quote document content back: it may carry subject data.
	throw IOException("rws: malformed XML from %s at byte %llu: %s", source, static_cast<uint64_t>(pos), message);
}

void XmlReader::SkipWhitespace() {
	while (pos < size && (data[pos] == ' ' || data[pos] == '\t' || data[pos] == '\r' || data[pos] == '\n')) {
		pos++;
	}
}

//! Skips character data, comments, processing instructions and CDATA sections.
//! The ODM subset we read carries all payload in attributes, so text nodes are
//! not surfaced; a DTD is refused outright.
void XmlReader::SkipMisc() {
	while (pos < size) {
		if (data[pos] != '<') {
			// Character data between elements.
			pos++;
			continue;
		}
		if (pos + 1 >= size) {
			Fail("truncated document");
		}
		if (data[pos + 1] == '?') {
			auto end = string(data + pos, size - pos).find("?>");
			if (end == string::npos) {
				Fail("unterminated processing instruction");
			}
			pos += end + 2;
			continue;
		}
		if (data[pos + 1] == '!') {
			if (size - pos >= 4 && memcmp(data + pos, "<!--", 4) == 0) {
				auto end = string(data + pos, size - pos).find("-->");
				if (end == string::npos) {
					Fail("unterminated comment");
				}
				pos += end + 3;
				continue;
			}
			if (size - pos >= 9 && memcmp(data + pos, "<![CDATA[", 9) == 0) {
				auto end = string(data + pos, size - pos).find("]]>");
				if (end == string::npos) {
					Fail("unterminated CDATA section");
				}
				pos += end + 3;
				continue;
			}
			if (size - pos >= 9 && memcmp(data + pos, "<!DOCTYPE", 9) == 0) {
				Fail("document type declarations are not accepted");
			}
			Fail("unsupported markup declaration");
		}
		return;
	}
}

string XmlReader::ParseName() {
	auto start = pos;
	while (pos < size) {
		auto c = data[pos];
		bool name_char = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
		                 c == '-' || c == '.' || c == ':' || static_cast<unsigned char>(c) >= 0x80;
		if (!name_char) {
			break;
		}
		pos++;
	}
	if (pos == start) {
		Fail("expected a name");
	}
	return string(data + start, pos - start);
}

string XmlReader::Unescape(const string &raw) const {
	if (raw.find('&') == string::npos) {
		return raw;
	}
	string out;
	out.reserve(raw.size());
	for (idx_t i = 0; i < raw.size(); i++) {
		if (raw[i] != '&') {
			out += raw[i];
			continue;
		}
		auto end = raw.find(';', i);
		if (end == string::npos) {
			Fail("unterminated entity reference");
		}
		auto entity = raw.substr(i + 1, end - i - 1);
		if (entity == "lt") {
			out += '<';
		} else if (entity == "gt") {
			out += '>';
		} else if (entity == "amp") {
			out += '&';
		} else if (entity == "quot") {
			out += '"';
		} else if (entity == "apos") {
			out += '\'';
		} else if (entity.size() > 1 && entity[0] == '#') {
			uint32_t code_point = 0;
			try {
				code_point = entity[1] == 'x' || entity[1] == 'X'
				                 ? static_cast<uint32_t>(std::stoul(entity.substr(2), nullptr, 16))
				                 : static_cast<uint32_t>(std::stoul(entity.substr(1), nullptr, 10));
			} catch (const std::exception &) {
				Fail("invalid numeric character reference");
			}
			char buffer[4];
			int length = 0;
			Utf8Proc::CodepointToUtf8(static_cast<int>(code_point), length, buffer);
			out.append(buffer, static_cast<size_t>(length));
		} else {
			// Refusing unknown entities also refuses external entity references.
			Fail("unsupported entity reference");
		}
		i = end;
	}
	return out;
}

string XmlReader::ParseAttributeValue() {
	if (pos >= size || (data[pos] != '"' && data[pos] != '\'')) {
		Fail("expected a quoted attribute value");
	}
	auto quote = data[pos++];
	auto start = pos;
	while (pos < size && data[pos] != quote) {
		if (data[pos] == '<') {
			Fail("'<' is not allowed inside an attribute value");
		}
		pos++;
	}
	if (pos >= size) {
		Fail("unterminated attribute value");
	}
	auto raw = string(data + start, pos - start);
	pos++;
	return Unescape(raw);
}

string XmlReader::ResolvePrefix(const string &prefix, bool is_element) const {
	if (prefix.empty() && !is_element) {
		// Unprefixed attributes are in no namespace.
		return string();
	}
	if (prefix == "xml") {
		return "http://www.w3.org/XML/1998/namespace";
	}
	for (auto it = namespace_stack.rbegin(); it != namespace_stack.rend(); ++it) {
		if (it->prefix == prefix) {
			return it->uri;
		}
	}
	if (prefix.empty()) {
		return string();
	}
	Fail("undeclared namespace prefix");
}

void XmlReader::ParseStartElement() {
	pos++; // '<'
	auto qualified = ParseName();
	attributes.clear();
	empty_element = false;

	auto element_depth = element_stack.size() + 1;

	struct RawAttribute {
		string qualified;
		string value;
	};
	vector<RawAttribute> raw_attributes;

	while (true) {
		SkipWhitespace();
		if (pos >= size) {
			Fail("unterminated start tag");
		}
		if (data[pos] == '>') {
			pos++;
			break;
		}
		if (data[pos] == '/') {
			pos++;
			if (pos >= size || data[pos] != '>') {
				Fail("expected '>' after '/'");
			}
			pos++;
			empty_element = true;
			break;
		}
		auto attribute_name = ParseName();
		SkipWhitespace();
		if (pos >= size || data[pos] != '=') {
			Fail("expected '=' in attribute");
		}
		pos++;
		SkipWhitespace();
		auto attribute_value = ParseAttributeValue();

		if (attribute_name == "xmlns") {
			namespace_stack.push_back({element_depth, string(), attribute_value});
		} else if (StringUtil::StartsWith(attribute_name, "xmlns:")) {
			namespace_stack.push_back({element_depth, attribute_name.substr(6), attribute_value});
		} else {
			raw_attributes.push_back({attribute_name, attribute_value});
		}
	}

	auto split = [](const string &name, string &prefix, string &local) {
		auto colon = name.find(':');
		if (colon == string::npos) {
			prefix.clear();
			local = name;
		} else {
			prefix = name.substr(0, colon);
			local = name.substr(colon + 1);
		}
	};

	string prefix;
	string element_local;
	split(qualified, prefix, element_local);
	uri = ResolvePrefix(prefix, true);
	local_name = element_local;

	for (auto &raw : raw_attributes) {
		string attribute_prefix;
		string attribute_local;
		split(raw.qualified, attribute_prefix, attribute_local);
		attributes.push_back({ResolvePrefix(attribute_prefix, false), attribute_local, std::move(raw.value)});
	}

	element_stack.push_back(uri + "\x1f" + local_name);
	depth = element_stack.size();
	event = Event::START_ELEMENT;
	pending_close = empty_element;
}

void XmlReader::ParseEndElement() {
	pos += 2; // '</'
	auto qualified = ParseName();
	SkipWhitespace();
	if (pos >= size || data[pos] != '>') {
		Fail("expected '>' in end tag");
	}
	pos++;

	if (element_stack.empty()) {
		Fail("end tag without a matching start tag");
	}
	auto colon = qualified.find(':');
	auto prefix = colon == string::npos ? string() : qualified.substr(0, colon);
	auto closing_local = colon == string::npos ? qualified : qualified.substr(colon + 1);
	auto closing_uri = ResolvePrefix(prefix, true);
	if (element_stack.back() != closing_uri + "\x1f" + closing_local) {
		Fail("mismatched end tag");
	}
	local_name = closing_local;
	uri = closing_uri;
	depth = element_stack.size();
	element_stack.pop_back();
	while (!namespace_stack.empty() && namespace_stack.back().depth > element_stack.size()) {
		namespace_stack.pop_back();
	}
	attributes.clear();
	empty_element = false;
	event = Event::END_ELEMENT;
}

bool XmlReader::Read() {
	if (pending_close) {
		// Emit the implicit end event of a self-closing element.
		pending_close = false;
		local_name = string();
		uri = string();
		auto &top = element_stack.back();
		auto separator = top.find('\x1f');
		uri = top.substr(0, separator);
		local_name = top.substr(separator + 1);
		depth = element_stack.size();
		element_stack.pop_back();
		while (!namespace_stack.empty() && namespace_stack.back().depth > element_stack.size()) {
			namespace_stack.pop_back();
		}
		attributes.clear();
		empty_element = false;
		event = Event::END_ELEMENT;
		return true;
	}

	SkipMisc();
	if (pos >= size) {
		if (!element_stack.empty()) {
			Fail("document ended with unclosed elements");
		}
		event = Event::END_OF_DOCUMENT;
		return false;
	}
	if (pos + 1 < size && data[pos + 1] == '/') {
		ParseEndElement();
	} else {
		ParseStartElement();
	}
	return true;
}

bool XmlReader::TryGetAttribute(const char *ns, const char *name, string &result) const {
	for (auto &attribute : attributes) {
		if (attribute.local_name == name && attribute.uri == ns) {
			result = attribute.value;
			return true;
		}
	}
	return false;
}

string XmlReader::GetAttribute(const char *ns, const char *name) const {
	string result;
	TryGetAttribute(ns, name, result);
	return result;
}

string XmlReader::ReadTextContent() {
	if (event != Event::START_ELEMENT) {
		Fail("text content requested outside a start element");
	}
	if (pending_close) {
		Read(); // <tag/> holds no text
		return string();
	}
	string out;
	while (true) {
		auto start = pos;
		while (pos < size && data[pos] != '<') {
			pos++;
		}
		if (pos > start) {
			out += Unescape(string(data + start, pos - start));
		}
		if (pos >= size) {
			Fail("unterminated element");
		}
		if (pos + 1 >= size) {
			Fail("truncated document");
		}
		if (data[pos + 1] == '!') {
			if (size - pos >= 9 && memcmp(data + pos, "<![CDATA[", 9) == 0) {
				auto end = string(data + pos, size - pos).find("]]>");
				if (end == string::npos) {
					Fail("unterminated CDATA section");
				}
				out.append(data + pos + 9, end - 9);
				pos += end + 3;
				continue;
			}
			if (size - pos >= 4 && memcmp(data + pos, "<!--", 4) == 0) {
				auto end = string(data + pos, size - pos).find("-->");
				if (end == string::npos) {
					Fail("unterminated comment");
				}
				pos += end + 3;
				continue;
			}
			Fail("unsupported markup declaration inside element content");
		}
		if (data[pos + 1] == '?') {
			auto end = string(data + pos, size - pos).find("?>");
			if (end == string::npos) {
				Fail("unterminated processing instruction");
			}
			pos += end + 2;
			continue;
		}
		break;
	}
	if (data[pos + 1] != '/') {
		Fail("unexpected child element where character data was expected");
	}
	Read(); // consumes the matching end tag
	return out;
}

void XmlReader::SkipElement() {
	if (event != Event::START_ELEMENT) {
		return;
	}
	auto target_depth = depth;
	while (Read()) {
		if (event == Event::END_ELEMENT && depth == target_depth) {
			return;
		}
	}
	Fail("unterminated element");
}

} // namespace duckdb
