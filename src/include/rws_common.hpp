//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_common.hpp
//
// Connection parameters, secret lookup and URL helpers shared by every
// component of the extension.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

class ExtensionLoader;

//! Everything needed to talk to one Rave Web Services instance.
//! The password is only ever read from a secret and never rendered back out.
struct RWSConnection {
	//! Service root, e.g. https://example.mdsol.com/RaveWebServices (no trailing slash)
	string base_url;
	string username;
	string password;
	bool verify_ssl = true;
	uint64_t timeout_seconds = 300;
	uint64_t max_retries = 2;

	//! Identity of this connection for cache partitioning. Never contains the password.
	string CacheIdentity() const {
		return base_url + "\x1f" + username;
	}
};

//! Resolves a secret into connection parameters. An empty name looks up the
//! default secret registered for the 'rws' type.
RWSConnection RWSGetConnection(ClientContext &context, const string &secret_name);

void RWSRegisterSecretType(ExtensionLoader &loader);

//! StringUtil::Trim mutates in place; this returns a trimmed copy.
inline string RWSTrim(const string &value) {
	auto copy = value;
	StringUtil::Trim(copy);
	return copy;
}

//! Percent-encodes one URL path segment (RFC 3986 pchar set).
string RWSEncodePathSegment(const string &segment);
//! Percent-encodes a query string value.
string RWSEncodeQueryValue(const string &value);

//! Builds the canonical Rave study OID from a project and environment.
//! An empty environment yields the bare project name.
string RWSMakeStudyOID(const string &project, const string &environment);
//! Splits "PROJECT(ENV)" back into its parts. Returns false when the OID does
//! not carry an environment suffix, leaving environment empty.
bool RWSSplitStudyOID(const string &study_oid, string &project, string &environment);

} // namespace duckdb
