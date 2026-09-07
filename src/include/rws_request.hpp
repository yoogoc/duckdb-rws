//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_request.hpp
//
// One request specification and one materialised result, shared by the table
// functions and by the ATTACH catalog so both produce identical tables.
//===----------------------------------------------------------------------===//

#pragma once

#include "rws_common.hpp"
#include "rws_odm.hpp"

namespace duckdb {

enum class RWSTableKind : uint8_t {
	STUDIES,
	SUBJECTS,
	SITES,
	FORMS,
	STUDY_EVENTS,
	ITEMS,
	CLINICAL_ITEMS,
	FORM_WIDE,
	FORM_COLUMNS,
	SERVER_VERSION
};

//! Everything that determines which rows a table produces. Two specs that
//! compare equal must always produce the same table.
struct RWSRequestSpec {
	RWSTableKind kind = RWSTableKind::STUDIES;
	string secret_name;
	//! The study this table is scoped to, e.g. "STUDY(Prod)". Empty for
	//! server-wide tables.
	string study_oid;
	string form_oid;
	string dataset_type = "regular";
	//! Restricts the clinical dataset to one subject, using the subject-scoped
	//! endpoint.
	string subject_key;
	//! ISO 8601 lower bound handed to the server as `start`.
	string start;
	//! Subject list options, passed through verbatim after validation.
	string subject_include;
	bool subject_status = false;
	bool subject_links = false;
	string subject_key_type;
	//! Bypasses the metadata/response cache for this execution.
	bool refresh = false;

	//! Stable identity used for caching and for catalog bookkeeping.
	string CacheKey(const RWSConnection &connection) const;
};

//! A fully materialised table. Every RWS table is small enough relative to the
//! response that has to be downloaded and validated in full anyway.
struct RWSResult {
	vector<string> names;
	vector<LogicalType> types;
	vector<vector<Value>> rows;
	//! Source freshness hints, surfaced through rws_scan_history.
	string cv_last_updated;
	timestamp_t fetched_at = timestamp_t(0);
};

//! Resolves the column layout of a table without materialising its rows.
//! For dynamic tables this still needs the underlying response, which is
//! served from the cache.
void RWSResolveColumns(ClientContext &context, const RWSConnection &connection, const RWSRequestSpec &spec,
                       vector<string> &names, vector<LogicalType> &types);

//! Builds the complete table.
shared_ptr<RWSResult> RWSBuildResult(ClientContext &context, const RWSConnection &connection,
                                     const RWSRequestSpec &spec);

//! Lists the forms present in a study's clinical dataset, in first-appearance
//! order. Used by the catalog to enumerate tables.
vector<string> RWSListForms(ClientContext &context, const RWSConnection &connection, const RWSRequestSpec &spec);

//! Drops every cached response. Returns the number of entries removed.
idx_t RWSClearCache();
//! Number of cached responses currently held.
idx_t RWSClearCacheCount();
//! Drops cached responses belonging to one study (or all studies of a
//! connection when `study_oid` is empty).
idx_t RWSInvalidateStudy(const RWSConnection &connection, const string &study_oid);

//! Names of the tables the catalog exposes for a study, excluding form tables.
const vector<string> &RWSMasterTableNames();
//! Maps a master table name to its kind. Returns false for unknown names.
bool RWSMasterTableKind(const string &name, RWSTableKind &kind);

} // namespace duckdb
