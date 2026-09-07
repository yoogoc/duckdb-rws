#include "rws_common.hpp"

namespace duckdb {

static void AppendPercentEncoded(string &out, unsigned char c) {
	static const char *hex = "0123456789ABCDEF";
	out += '%';
	out += hex[c >> 4];
	out += hex[c & 0x0F];
}

//! Unreserved characters plus sub-delims and ':' / '@' form the pchar set that
//! may appear literally in a path segment. Study OIDs such as "STUDY(Prod)"
//! stay readable, while spaces and non-ASCII bytes are encoded.
string RWSEncodePathSegment(const string &segment) {
	string out;
	out.reserve(segment.size());
	for (auto ch : segment) {
		auto c = static_cast<unsigned char>(ch);
		bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		                  c == '.' || c == '_' || c == '~';
		bool sub_delim = c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' || c == '*' ||
		                 c == '+' || c == ',' || c == ';' || c == '=' || c == ':' || c == '@';
		if (unreserved || sub_delim) {
			out += ch;
		} else {
			AppendPercentEncoded(out, c);
		}
	}
	return out;
}

string RWSEncodeQueryValue(const string &value) {
	string out;
	out.reserve(value.size());
	for (auto ch : value) {
		auto c = static_cast<unsigned char>(ch);
		bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
		                  c == '.' || c == '_' || c == '~';
		if (unreserved) {
			out += ch;
		} else {
			AppendPercentEncoded(out, c);
		}
	}
	return out;
}

string RWSMakeStudyOID(const string &project, const string &environment) {
	if (project.empty()) {
		throw InvalidInputException("rws: project name must not be empty");
	}
	if (environment.empty()) {
		return project;
	}
	return project + "(" + environment + ")";
}

bool RWSSplitStudyOID(const string &study_oid, string &project, string &environment) {
	project = study_oid;
	environment = string();
	if (study_oid.size() < 3 || study_oid.back() != ')') {
		return false;
	}
	auto open = study_oid.rfind('(');
	if (open == string::npos || open == 0) {
		return false;
	}
	project = study_oid.substr(0, open);
	environment = study_oid.substr(open + 1, study_oid.size() - open - 2);
	return true;
}

} // namespace duckdb
