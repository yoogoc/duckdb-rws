//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_client.hpp
//
// Minimal HTTPS client for Rave Web Services GET endpoints.
//===----------------------------------------------------------------------===//

#pragma once

#include "rws_common.hpp"

namespace duckdb {

struct RWSHttpResponse {
	int status = 0;
	string body;
	string content_type;
	//! X-MWS-CV-Last-Updated, verbatim. A clinical-view refresh hint, not a CDC position.
	string cv_last_updated;
};

//! Performs an authenticated GET against `path_and_query` (relative to the
//! service root, without a leading slash) and returns the full response body.
//!
//! Throws for transport failures, non-2xx responses and RWS business errors
//! returned with a 200 status. Error messages carry the endpoint, the RWS
//! reason code and the server message, never the credentials or the body.
RWSHttpResponse RWSHttpGet(ClientContext &context, const RWSConnection &connection, const string &path_and_query);

} // namespace duckdb
