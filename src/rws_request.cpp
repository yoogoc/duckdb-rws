#include "rws_request.hpp"
#include "rws_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"

#include <unordered_map>

namespace duckdb {

namespace {

//! Responses are cached per (connection identity, endpoint) so that one
//! download can serve every table derived from it. This matters because this
//! RWS deployment exposes no per-form endpoint: the clinical dataset for a
//! study has to be fetched whole, and the catalog derives every form table
//! from that single response.
struct CacheEntry {
	shared_ptr<void> payload;
	timestamp_t created;
};

mutex &CacheLock() {
	static mutex lock;
	return lock;
}

std::unordered_map<string, CacheEntry> &CacheMap() {
	static std::unordered_map<string, CacheEntry> cache;
	return cache;
}

int64_t CacheSeconds(ClientContext &context) {
	Value setting;
	if (context.TryGetCurrentSetting("rws_cache_seconds", setting) && !setting.IsNull()) {
		return setting.GetValue<int64_t>();
	}
	return 300;
}

template <class T>
shared_ptr<T> CacheLookup(ClientContext &context, const string &key, bool refresh) {
	auto ttl = CacheSeconds(context);
	if (refresh || ttl <= 0) {
		return nullptr;
	}
	lock_guard<mutex> guard(CacheLock());
	auto &cache = CacheMap();
	auto entry = cache.find(key);
	if (entry == cache.end()) {
		return nullptr;
	}
	auto age = Timestamp::GetCurrentTimestamp().value - entry->second.created.value;
	if (age > ttl * Interval::MICROS_PER_SEC) {
		cache.erase(entry);
		return nullptr;
	}
	return shared_ptr_cast<void, T>(entry->second.payload);
}

template <class T>
void CacheStore(ClientContext &context, const string &key, shared_ptr<T> payload) {
	if (CacheSeconds(context) <= 0) {
		return;
	}
	lock_guard<mutex> guard(CacheLock());
	CacheMap()[key] = CacheEntry {shared_ptr_cast<T, void>(std::move(payload)), Timestamp::GetCurrentTimestamp()};
}

Value VarcharOrNull(const string &value, bool present = true) {
	if (!present) {
		return Value(LogicalType::VARCHAR);
	}
	return Value(value);
}

Value BooleanOrNull(bool known, bool value) {
	return known ? Value::BOOLEAN(value) : Value(LogicalType::BOOLEAN);
}

//! Context attributes are either present with a value or absent. An absent
//! attribute is SQL NULL; it is never turned into an empty string.
Value Context(const string &value) {
	return value.empty() ? Value(LogicalType::VARCHAR) : Value(value);
}

void ValidateStart(const string &start) {
	if (start.empty()) {
		return;
	}
	// Accept YYYY-MM-DD and YYYY-MM-DDTHH:MM:SS[.fff][Z|±HH:MM]. No timezone is
	// inferred: the value is forwarded exactly as written.
	auto is_digits = [&](idx_t offset, idx_t count) {
		if (start.size() < offset + count) {
			return false;
		}
		for (idx_t i = 0; i < count; i++) {
			if (!StringUtil::CharacterIsDigit(start[offset + i])) {
				return false;
			}
		}
		return true;
	};
	bool valid =
	    is_digits(0, 4) && start.size() > 9 && start[4] == '-' && is_digits(5, 2) && start[7] == '-' && is_digits(8, 2);
	if (valid && start.size() > 10) {
		valid = start[10] == 'T' && is_digits(11, 2) && start.size() > 15 && start[13] == ':' && is_digits(14, 2);
		if (valid && start.size() > 16) {
			valid = start[16] == ':' && is_digits(17, 2);
		}
	}
	if (!valid) {
		throw InvalidInputException("rws: start must be an ISO 8601 timestamp such as '2026-01-31T00:00:00', got '%s'",
		                            start);
	}
}

string SubjectsPath(const RWSRequestSpec &spec) {
	string path = "studies/" + RWSEncodePathSegment(spec.study_oid) + "/subjects";
	vector<string> query;
	if (!spec.subject_include.empty()) {
		query.push_back("include=" + RWSEncodeQueryValue(spec.subject_include));
	}
	if (spec.subject_status) {
		query.push_back("status=all");
	}
	if (spec.subject_links) {
		query.push_back("links=all");
	}
	if (!spec.subject_key_type.empty()) {
		query.push_back("subjectKeyType=" + RWSEncodeQueryValue(spec.subject_key_type));
	}
	if (!query.empty()) {
		path += "?" + StringUtil::Join(query, "&");
	}
	return path;
}

string DatasetPath(const RWSRequestSpec &spec) {
	string path = "studies/" + RWSEncodePathSegment(spec.study_oid);
	if (!spec.subject_key.empty()) {
		path += "/subjects/" + RWSEncodePathSegment(spec.subject_key);
	}
	path += "/datasets/" + RWSEncodePathSegment(spec.dataset_type);
	if (!spec.start.empty()) {
		path += "?start=" + RWSEncodeQueryValue(spec.start);
	}
	return path;
}

void RequireStudy(const RWSRequestSpec &spec) {
	if (spec.study_oid.empty()) {
		throw InvalidInputException("rws: this table requires a study, pass project and environment");
	}
}

shared_ptr<vector<RWSStudy>> FetchStudies(ClientContext &context, const RWSConnection &connection,
                                          const RWSRequestSpec &spec) {
	auto key = connection.CacheIdentity() + "\x1e" + "studies";
	auto cached = CacheLookup<vector<RWSStudy>>(context, key, spec.refresh);
	if (cached) {
		return cached;
	}
	auto response = RWSHttpGet(context, connection, "studies");
	auto studies = make_shared_ptr<vector<RWSStudy>>(RWSParseStudies(response.body));
	CacheStore(context, key, studies);
	return studies;
}

shared_ptr<vector<RWSSubject>> FetchSubjects(ClientContext &context, const RWSConnection &connection,
                                             const RWSRequestSpec &spec) {
	RequireStudy(spec);
	auto path = SubjectsPath(spec);
	auto key = connection.CacheIdentity() + "\x1e" + path;
	auto cached = CacheLookup<vector<RWSSubject>>(context, key, spec.refresh);
	if (cached) {
		return cached;
	}
	auto response = RWSHttpGet(context, connection, path);
	auto subjects = make_shared_ptr<vector<RWSSubject>>(RWSParseSubjects(response.body));
	// The identifier system must never change silently underneath a query.
	if (!spec.subject_key_type.empty()) {
		for (auto &subject : *subjects) {
			if (!subject.subject_key_type.empty() && subject.subject_key_type != spec.subject_key_type) {
				throw IOException("rws: requested subject_key_type '%s' but the server returned '%s' — this "
				                  "environment does not support the requested identifier",
				                  spec.subject_key_type, subject.subject_key_type);
			}
		}
	}
	CacheStore(context, key, subjects);
	return subjects;
}

shared_ptr<RWSDataset> FetchDataset(ClientContext &context, const RWSConnection &connection,
                                    const RWSRequestSpec &spec) {
	RequireStudy(spec);
	auto path = DatasetPath(spec);
	auto key = connection.CacheIdentity() + "\x1e" + path;
	auto cached = CacheLookup<RWSDataset>(context, key, spec.refresh);
	if (cached) {
		return cached;
	}
	auto response = RWSHttpGet(context, connection, path);
	auto dataset = RWSParseDataset(response.body, spec.dataset_type, response.cv_last_updated);
	if (dataset->study_oid.empty()) {
		dataset->study_oid = spec.study_oid;
	}
	CacheStore(context, key, dataset);
	return dataset;
}

//! DuckDB compares identifiers case-insensitively, so collision detection has
//! to as well.
struct ColumnNamer {
	vector<string> names;
	std::unordered_map<string, idx_t> taken;

	bool IsTaken(const string &candidate) const {
		return taken.find(StringUtil::Lower(candidate)) != taken.end();
	}
	void Reserve(const string &name) {
		taken[StringUtil::Lower(name)] = names.size();
		names.push_back(name);
	}
	//! Adds `preferred`, falling back to `fallback` and then to numbered
	//! suffixes. Returns the name actually used.
	string Add(const string &preferred, const string &fallback) {
		string candidate = preferred;
		if (candidate.empty() || IsTaken(candidate)) {
			candidate = fallback;
		}
		if (candidate.empty()) {
			candidate = "column";
		}
		if (IsTaken(candidate)) {
			auto base = candidate;
			for (idx_t suffix = 2;; suffix++) {
				candidate = base + "__" + to_string(suffix);
				if (!IsTaken(candidate)) {
					break;
				}
			}
		}
		Reserve(candidate);
		return candidate;
	}
};

//! Item OIDs are form-qualified ("AE.AETERM"). Strip the form prefix for
//! readability, but keep the full OID whenever stripping would be ambiguous.
string PreferredColumnName(const string &item_oid, const string &form_oid) {
	auto prefix = form_oid + ".";
	if (!form_oid.empty() && StringUtil::StartsWith(item_oid, prefix) && item_oid.size() > prefix.size()) {
		return item_oid.substr(prefix.size());
	}
	return item_oid;
}

const char *const FORM_CONTEXT_COLUMNS[] = {
    "record_id",       "study_oid",       "metadata_version_oid",   "subject_key",
    "site_oid",        "study_event_oid", "study_event_repeat_key", "form_oid",
    "form_repeat_key", "item_group_oid",  "item_group_repeat_key"};
constexpr idx_t FORM_CONTEXT_COLUMN_COUNT = sizeof(FORM_CONTEXT_COLUMNS) / sizeof(FORM_CONTEXT_COLUMNS[0]);

//! Resolves the wide-table column layout of one form. `names`/`types` receive
//! the complete layout (context columns followed by item columns); the extra
//! `column_names` output holds just the item columns, positionally aligned
//! with `shape.item_oids`, for the item-to-column mapping tables.
void BuildFormColumns(const RWSFormShape &shape, vector<string> &names, vector<LogicalType> &types,
                      vector<string> &column_names) {
	ColumnNamer namer;
	for (idx_t i = 0; i < FORM_CONTEXT_COLUMN_COUNT; i++) {
		namer.Reserve(FORM_CONTEXT_COLUMNS[i]);
	}
	types.push_back(LogicalType::BIGINT);
	for (idx_t i = 1; i < FORM_CONTEXT_COLUMN_COUNT; i++) {
		types.push_back(LogicalType::VARCHAR);
	}
	for (auto &item_oid : shape.item_oids) {
		column_names.push_back(namer.Add(PreferredColumnName(item_oid, shape.form_oid), item_oid));
		types.push_back(LogicalType::VARCHAR);
	}
	names = namer.names;
}

const RWSFormShape &GetFormShape(const RWSDataset &dataset, const string &form_oid) {
	auto entry = dataset.forms.find(form_oid);
	if (entry == dataset.forms.end()) {
		throw InvalidInputException("rws: form '%s' has no data in study '%s' (dataset '%s'). Forms present: %s",
		                            form_oid, dataset.study_oid, dataset.dataset_type,
		                            dataset.form_oids.empty() ? "none" : StringUtil::Join(dataset.form_oids, ", "));
	}
	return entry->second;
}

} // namespace

string RWSRequestSpec::CacheKey(const RWSConnection &connection) const {
	string key = connection.CacheIdentity();
	key += "\x1e" + to_string(static_cast<int>(kind));
	key += "\x1e" + study_oid;
	key += "\x1e" + form_oid;
	key += "\x1e" + dataset_type;
	key += "\x1e" + subject_key;
	key += "\x1e" + start;
	key += "\x1e" + subject_include;
	key += "\x1e" + string(subject_status ? "1" : "0");
	key += "\x1e" + string(subject_links ? "1" : "0");
	key += "\x1e" + subject_key_type;
	return key;
}

const vector<string> &RWSMasterTableNames() {
	static const vector<string> names = {"studies", "subjects",     "sites",          "forms",
	                                     "items",   "study_events", "clinical_items", "form_columns"};
	return names;
}

bool RWSMasterTableKind(const string &name, RWSTableKind &kind) {
	auto lower = StringUtil::Lower(name);
	if (lower == "studies") {
		kind = RWSTableKind::STUDIES;
	} else if (lower == "subjects") {
		kind = RWSTableKind::SUBJECTS;
	} else if (lower == "sites") {
		kind = RWSTableKind::SITES;
	} else if (lower == "forms") {
		kind = RWSTableKind::FORMS;
	} else if (lower == "items") {
		kind = RWSTableKind::ITEMS;
	} else if (lower == "study_events") {
		kind = RWSTableKind::STUDY_EVENTS;
	} else if (lower == "clinical_items") {
		kind = RWSTableKind::CLINICAL_ITEMS;
	} else if (lower == "form_columns") {
		kind = RWSTableKind::FORM_COLUMNS;
	} else {
		return false;
	}
	return true;
}

vector<string> RWSListForms(ClientContext &context, const RWSConnection &connection, const RWSRequestSpec &spec) {
	auto dataset = FetchDataset(context, connection, spec);
	return dataset->form_oids;
}

idx_t RWSClearCacheCount() {
	lock_guard<mutex> guard(CacheLock());
	return CacheMap().size();
}

idx_t RWSClearCache() {
	lock_guard<mutex> guard(CacheLock());
	auto removed = CacheMap().size();
	CacheMap().clear();
	return removed;
}

idx_t RWSInvalidateStudy(const RWSConnection &connection, const string &study_oid) {
	auto identity = connection.CacheIdentity();
	auto study_marker = study_oid.empty() ? string() : "studies/" + RWSEncodePathSegment(study_oid);
	lock_guard<mutex> guard(CacheLock());
	idx_t removed = 0;
	auto &cache = CacheMap();
	for (auto it = cache.begin(); it != cache.end();) {
		bool matches = StringUtil::StartsWith(it->first, identity) &&
		               (study_marker.empty() || it->first.find(study_marker) != string::npos);
		if (matches) {
			it = cache.erase(it);
			removed++;
		} else {
			++it;
		}
	}
	return removed;
}

void RWSResolveColumns(ClientContext &context, const RWSConnection &connection, const RWSRequestSpec &spec,
                       vector<string> &names, vector<LogicalType> &types) {
	names.clear();
	types.clear();
	auto add = [&](const char *name, LogicalType type) {
		names.emplace_back(name);
		types.push_back(std::move(type));
	};
	switch (spec.kind) {
	case RWSTableKind::SERVER_VERSION:
		add("base_url", LogicalType::VARCHAR);
		add("version", LogicalType::VARCHAR);
		break;
	case RWSTableKind::STUDIES:
		add("study_oid", LogicalType::VARCHAR);
		add("project_name", LogicalType::VARCHAR);
		add("environment", LogicalType::VARCHAR);
		add("study_name", LogicalType::VARCHAR);
		add("study_description", LogicalType::VARCHAR);
		add("protocol_name", LogicalType::VARCHAR);
		break;
	case RWSTableKind::SUBJECTS:
		add("study_oid", LogicalType::VARCHAR);
		add("metadata_version_oid", LogicalType::VARCHAR);
		add("subject_key", LogicalType::VARCHAR);
		add("subject_key_type", LogicalType::VARCHAR);
		add("subject_name", LogicalType::VARCHAR);
		add("site_oid", LogicalType::VARCHAR);
		add("site_number", LogicalType::VARCHAR);
		add("is_active", LogicalType::BOOLEAN);
		add("is_deleted", LogicalType::BOOLEAN);
		add("transaction_type", LogicalType::VARCHAR);
		add("links", LogicalType::LIST(LogicalType::VARCHAR));
		add("attributes", LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR));
		break;
	case RWSTableKind::SITES:
		add("study_oid", LogicalType::VARCHAR);
		add("site_oid", LogicalType::VARCHAR);
		add("site_number", LogicalType::VARCHAR);
		add("subject_count", LogicalType::BIGINT);
		break;
	case RWSTableKind::FORMS:
		add("study_oid", LogicalType::VARCHAR);
		add("form_oid", LogicalType::VARCHAR);
		add("table_name", LogicalType::VARCHAR);
		add("item_group_oids", LogicalType::LIST(LogicalType::VARCHAR));
		add("column_count", LogicalType::BIGINT);
		add("record_count", LogicalType::BIGINT);
		break;
	case RWSTableKind::STUDY_EVENTS:
		add("study_oid", LogicalType::VARCHAR);
		add("study_event_oid", LogicalType::VARCHAR);
		add("form_oids", LogicalType::LIST(LogicalType::VARCHAR));
		add("record_count", LogicalType::BIGINT);
		break;
	case RWSTableKind::ITEMS:
		add("study_oid", LogicalType::VARCHAR);
		add("form_oid", LogicalType::VARCHAR);
		add("item_group_oid", LogicalType::VARCHAR);
		add("item_oid", LogicalType::VARCHAR);
		add("column_name", LogicalType::VARCHAR);
		add("value_count", LogicalType::BIGINT);
		add("null_count", LogicalType::BIGINT);
		break;
	case RWSTableKind::CLINICAL_ITEMS:
		add("record_id", LogicalType::BIGINT);
		add("study_oid", LogicalType::VARCHAR);
		add("metadata_version_oid", LogicalType::VARCHAR);
		add("subject_key", LogicalType::VARCHAR);
		add("site_oid", LogicalType::VARCHAR);
		add("study_event_oid", LogicalType::VARCHAR);
		add("study_event_repeat_key", LogicalType::VARCHAR);
		add("form_oid", LogicalType::VARCHAR);
		add("form_repeat_key", LogicalType::VARCHAR);
		add("item_group_oid", LogicalType::VARCHAR);
		add("item_group_repeat_key", LogicalType::VARCHAR);
		add("item_oid", LogicalType::VARCHAR);
		add("value", LogicalType::VARCHAR);
		add("is_null", LogicalType::BOOLEAN);
		add("subject_transaction_type", LogicalType::VARCHAR);
		add("study_event_transaction_type", LogicalType::VARCHAR);
		add("form_transaction_type", LogicalType::VARCHAR);
		add("item_group_transaction_type", LogicalType::VARCHAR);
		add("item_transaction_type", LogicalType::VARCHAR);
		break;
	case RWSTableKind::FORM_COLUMNS:
		add("study_oid", LogicalType::VARCHAR);
		add("form_oid", LogicalType::VARCHAR);
		add("ordinal", LogicalType::BIGINT);
		add("column_name", LogicalType::VARCHAR);
		add("source_item_oid", LogicalType::VARCHAR);
		add("value_count", LogicalType::BIGINT);
		add("null_count", LogicalType::BIGINT);
		break;
	case RWSTableKind::FORM_WIDE: {
		if (spec.form_oid.empty()) {
			throw InvalidInputException("rws: form_oid is required for a form table");
		}
		auto dataset = FetchDataset(context, connection, spec);
		auto &shape = GetFormShape(*dataset, spec.form_oid);
		vector<string> column_names;
		BuildFormColumns(shape, names, types, column_names);
		break;
	}
	default:
		throw InternalException("rws: unhandled table kind");
	}
}

shared_ptr<RWSResult> RWSBuildResult(ClientContext &context, const RWSConnection &connection,
                                     const RWSRequestSpec &spec) {
	ValidateStart(spec.start);
	auto result = make_shared_ptr<RWSResult>();
	result->fetched_at = Timestamp::GetCurrentTimestamp();

	switch (spec.kind) {
	case RWSTableKind::SERVER_VERSION: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto response = RWSHttpGet(context, connection, "version");
		result->rows.push_back({Value(connection.base_url), Value(RWSTrim(response.body))});
		break;
	}
	case RWSTableKind::STUDIES: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto studies = FetchStudies(context, connection, spec);
		for (auto &study : *studies) {
			if (!spec.study_oid.empty() && study.study_oid != spec.study_oid) {
				continue;
			}
			result->rows.push_back({Value(study.study_oid), Value(study.project_name), Value(study.environment),
			                        Value(study.study_name), Value(study.study_description),
			                        Value(study.protocol_name)});
		}
		break;
	}
	case RWSTableKind::SUBJECTS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto subjects = FetchSubjects(context, connection, spec);
		for (auto &subject : *subjects) {
			vector<Value> links;
			for (auto &link : subject.links) {
				links.emplace_back(link);
			}
			vector<Value> keys;
			vector<Value> values;
			for (auto &attribute : subject.attributes) {
				keys.emplace_back(attribute.first);
				values.emplace_back(attribute.second);
			}
			result->rows.push_back(
			    {Context(subject.study_oid), Context(subject.metadata_version_oid), Context(subject.subject_key),
			     Context(subject.subject_key_type), Context(subject.subject_name), Context(subject.site_oid),
			     Context(subject.site_number), BooleanOrNull(subject.active_known, subject.active),
			     BooleanOrNull(subject.deleted_known, subject.deleted), Context(subject.transaction_type),
			     Value::LIST(LogicalType::VARCHAR, std::move(links)),
			     Value::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR, std::move(keys), std::move(values))});
		}
		break;
	}
	case RWSTableKind::SITES: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto subjects = FetchSubjects(context, connection, spec);
		// This deployment has no site endpoint and no clinical view metadata,
		// so the site list is what the subject list references.
		vector<string> order;
		std::unordered_map<string, std::pair<string, int64_t>> sites;
		for (auto &subject : *subjects) {
			if (subject.site_oid.empty()) {
				continue;
			}
			auto entry = sites.find(subject.site_oid);
			if (entry == sites.end()) {
				sites[subject.site_oid] = {subject.site_number, 1};
				order.push_back(subject.site_oid);
			} else {
				entry->second.second++;
				if (entry->second.first.empty()) {
					entry->second.first = subject.site_number;
				}
			}
		}
		for (auto &site_oid : order) {
			auto &entry = sites[site_oid];
			result->rows.push_back(
			    {Context(spec.study_oid), Context(site_oid), Context(entry.first), Value::BIGINT(entry.second)});
		}
		break;
	}
	case RWSTableKind::FORMS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		for (auto &form_oid : dataset->form_oids) {
			auto &shape = dataset->forms.at(form_oid);
			vector<Value> item_groups;
			for (auto &item_group_oid : shape.item_group_oids) {
				item_groups.emplace_back(item_group_oid);
			}
			result->rows.push_back({Value(dataset->study_oid), Value(form_oid), Value(form_oid),
			                        Value::LIST(LogicalType::VARCHAR, std::move(item_groups)),
			                        Value::BIGINT(NumericCast<int64_t>(shape.item_oids.size())),
			                        Value::BIGINT(NumericCast<int64_t>(shape.record_indexes.size()))});
		}
		break;
	}
	case RWSTableKind::STUDY_EVENTS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		vector<string> order;
		std::unordered_map<string, std::pair<vector<string>, int64_t>> events;
		for (auto &record : dataset->records) {
			auto entry = events.find(record.study_event_oid);
			if (entry == events.end()) {
				events[record.study_event_oid] = {{record.form_oid}, 1};
				order.push_back(record.study_event_oid);
			} else {
				entry->second.second++;
				auto &forms = entry->second.first;
				if (std::find(forms.begin(), forms.end(), record.form_oid) == forms.end()) {
					forms.push_back(record.form_oid);
				}
			}
		}
		for (auto &event_oid : order) {
			auto &entry = events[event_oid];
			vector<Value> forms;
			for (auto &form_oid : entry.first) {
				forms.emplace_back(form_oid);
			}
			result->rows.push_back({Value(dataset->study_oid), Value(event_oid),
			                        Value::LIST(LogicalType::VARCHAR, std::move(forms)), Value::BIGINT(entry.second)});
		}
		break;
	}
	case RWSTableKind::ITEMS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		for (auto &form_oid : dataset->form_oids) {
			if (!spec.form_oid.empty() && form_oid != spec.form_oid) {
				continue;
			}
			auto &shape = dataset->forms.at(form_oid);
			vector<string> names;
			vector<LogicalType> types;
			vector<string> column_names;
			BuildFormColumns(shape, names, types, column_names);

			struct ItemStat {
				string item_group_oid;
				int64_t value_count = 0;
				int64_t null_count = 0;
			};
			vector<ItemStat> stats(shape.item_oids.size());
			for (auto record_index : shape.record_indexes) {
				auto &record = dataset->records[record_index];
				for (auto &item : record.items) {
					auto &stat = stats[shape.item_index.at(item.item_oid)];
					if (stat.item_group_oid.empty()) {
						stat.item_group_oid = record.item_group_oid;
					}
					if (item.has_value) {
						stat.value_count++;
					}
					if (item.is_null_known && item.is_null) {
						stat.null_count++;
					}
				}
			}
			for (idx_t i = 0; i < shape.item_oids.size(); i++) {
				result->rows.push_back({Value(dataset->study_oid), Value(form_oid), Value(stats[i].item_group_oid),
				                        Value(shape.item_oids[i]), Value(column_names[i]),
				                        Value::BIGINT(stats[i].value_count), Value::BIGINT(stats[i].null_count)});
			}
		}
		break;
	}
	case RWSTableKind::FORM_COLUMNS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		for (auto &form_oid : dataset->form_oids) {
			if (!spec.form_oid.empty() && form_oid != spec.form_oid) {
				continue;
			}
			auto &shape = dataset->forms.at(form_oid);
			vector<string> names;
			vector<LogicalType> types;
			vector<string> column_names;
			BuildFormColumns(shape, names, types, column_names);
			vector<int64_t> value_counts(shape.item_oids.size(), 0);
			vector<int64_t> null_counts(shape.item_oids.size(), 0);
			for (auto record_index : shape.record_indexes) {
				for (auto &item : dataset->records[record_index].items) {
					auto position = shape.item_index.at(item.item_oid);
					if (item.has_value) {
						value_counts[position]++;
					}
					if (item.is_null_known && item.is_null) {
						null_counts[position]++;
					}
				}
			}
			for (idx_t i = 0; i < shape.item_oids.size(); i++) {
				result->rows.push_back({Value(dataset->study_oid), Value(form_oid),
				                        Value::BIGINT(NumericCast<int64_t>(FORM_CONTEXT_COLUMN_COUNT + i)),
				                        Value(column_names[i]), Value(shape.item_oids[i]),
				                        Value::BIGINT(value_counts[i]), Value::BIGINT(null_counts[i])});
			}
		}
		break;
	}
	case RWSTableKind::CLINICAL_ITEMS: {
		RWSResolveColumns(context, connection, spec, result->names, result->types);
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		for (idx_t record_index = 0; record_index < dataset->records.size(); record_index++) {
			auto &record = dataset->records[record_index];
			if (!spec.form_oid.empty() && record.form_oid != spec.form_oid) {
				continue;
			}
			for (auto &item : record.items) {
				result->rows.push_back(
				    {Value::BIGINT(NumericCast<int64_t>(record_index)), Context(record.study_oid),
				     Context(record.metadata_version_oid), Context(record.subject_key), Context(record.site_oid),
				     Context(record.study_event_oid), Context(record.study_event_repeat_key), Context(record.form_oid),
				     Context(record.form_repeat_key), Context(record.item_group_oid),
				     Context(record.item_group_repeat_key), Context(item.item_oid),
				     VarcharOrNull(item.value, item.has_value), BooleanOrNull(item.is_null_known, item.is_null),
				     VarcharOrNull(record.subject_transaction_type, !record.subject_transaction_type.empty()),
				     VarcharOrNull(record.study_event_transaction_type, !record.study_event_transaction_type.empty()),
				     VarcharOrNull(record.form_transaction_type, !record.form_transaction_type.empty()),
				     VarcharOrNull(record.item_group_transaction_type, !record.item_group_transaction_type.empty()),
				     VarcharOrNull(item.transaction_type, !item.transaction_type.empty())});
			}
		}
		break;
	}
	case RWSTableKind::FORM_WIDE: {
		auto dataset = FetchDataset(context, connection, spec);
		result->cv_last_updated = dataset->cv_last_updated;
		auto &shape = GetFormShape(*dataset, spec.form_oid);
		vector<string> column_names;
		BuildFormColumns(shape, result->names, result->types, column_names);
		for (auto record_index : shape.record_indexes) {
			auto &record = dataset->records[record_index];
			vector<Value> row;
			row.reserve(result->names.size());
			row.push_back(Value::BIGINT(NumericCast<int64_t>(record_index)));
			row.push_back(Context(record.study_oid));
			row.push_back(Context(record.metadata_version_oid));
			row.push_back(Context(record.subject_key));
			row.push_back(Context(record.site_oid));
			row.push_back(Context(record.study_event_oid));
			row.push_back(Context(record.study_event_repeat_key));
			row.push_back(Context(record.form_oid));
			row.push_back(Context(record.form_repeat_key));
			row.push_back(Context(record.item_group_oid));
			row.push_back(Context(record.item_group_repeat_key));
			row.resize(result->names.size(), Value(LogicalType::VARCHAR));
			vector<bool> assigned(shape.item_oids.size(), false);
			for (auto &item : record.items) {
				auto item_position = shape.item_index.at(item.item_oid);
				if (assigned[item_position]) {
					// Two values for one item OID in a single record cannot be
					// pivoted without picking a winner, so refuse instead.
					throw IOException("rws: item '%s' appears more than once in one record of form '%s'; the wide "
					                  "table cannot represent it — use rws_clinical_items instead",
					                  item.item_oid, spec.form_oid);
				}
				assigned[item_position] = true;
				if (item.has_value) {
					row[FORM_CONTEXT_COLUMN_COUNT + item_position] = Value(item.value);
				}
			}
			result->rows.push_back(std::move(row));
		}
		break;
	}
	default:
		throw InternalException("rws: unhandled table kind");
	}
	return result;
}

} // namespace duckdb
