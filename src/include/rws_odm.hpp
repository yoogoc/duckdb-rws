//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_odm.hpp
//
// In-memory models for the ODM documents this extension reads, plus their
// parsers. Every value is kept as the source text; nothing is coerced.
//===----------------------------------------------------------------------===//

#pragma once

#include "rws_common.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {

struct RWSStudy {
	string study_oid;
	string study_name;
	string study_description;
	string protocol_name;
	//! Derived: protocol name when the server supplies one, else the OID prefix.
	string project_name;
	//! Derived from the OID suffix, empty when the OID carries none.
	string environment;
};

struct RWSSubject {
	string study_oid;
	string metadata_version_oid;
	string subject_key;
	string subject_key_type;
	string subject_name;
	string site_oid;
	string site_number;
	string transaction_type;
	bool active_known = false;
	bool active = false;
	bool deleted_known = false;
	bool deleted = false;
	vector<string> links;
	//! Every mdsol-namespaced attribute on SubjectData, verbatim.
	vector<pair<string, string>> attributes;
};

struct RWSItemValue {
	string item_oid;
	string value;
	bool has_value = false;
	//! Only set when the source carried an explicit IsNull marker.
	bool is_null_known = false;
	bool is_null = false;
	string transaction_type;
};

//! One ItemGroupData instance: the finest grain the clinical view exposes and
//! the row grain of both the long and the wide table.
struct RWSRecord {
	string study_oid;
	string metadata_version_oid;
	string subject_key;
	string site_oid;
	string study_event_oid;
	string study_event_repeat_key;
	string form_oid;
	string form_repeat_key;
	string item_group_oid;
	string item_group_repeat_key;
	string subject_transaction_type;
	string study_event_transaction_type;
	string form_transaction_type;
	string item_group_transaction_type;
	vector<RWSItemValue> items;
};

//! The column shape of one form, derived from the data because this deployment
//! exposes no metadata endpoint to derive it from.
struct RWSFormShape {
	string form_oid;
	//! Item OIDs in order of first appearance in the document.
	vector<string> item_oids;
	unordered_map<string, idx_t> item_index;
	vector<string> item_group_oids;
	vector<idx_t> record_indexes;
};

struct RWSDataset {
	string study_oid;
	string dataset_type;
	//! X-MWS-CV-Last-Updated as returned, verbatim.
	string cv_last_updated;
	timestamp_t fetched_at;
	vector<RWSRecord> records;
	//! Form OIDs in order of first appearance.
	vector<string> form_oids;
	unordered_map<string, RWSFormShape> forms;
	vector<string> study_event_oids;
	//! Site OIDs seen on SiteRef, in order of first appearance.
	vector<string> site_oids;
};

vector<RWSStudy> RWSParseStudies(const string &xml);
vector<RWSSubject> RWSParseSubjects(const string &xml);
shared_ptr<RWSDataset> RWSParseDataset(const string &xml, const string &dataset_type, const string &cv_last_updated);

} // namespace duckdb
