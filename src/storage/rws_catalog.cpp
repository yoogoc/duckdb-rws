#include "rws_catalog.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"
#include "duckdb/parser/parsed_data/create_schema_info.hpp"
#include "duckdb/parser/parsed_data/create_table_info.hpp"
#include "duckdb/storage/storage_extension.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Table entry
//===--------------------------------------------------------------------===//

RWSTableEntry::RWSTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, RWSRequestSpec spec,
                             ResultBuilder builder)
    : TableCatalogEntry(catalog, schema, info), spec(std::move(spec)), builder(std::move(builder)) {
}

unique_ptr<BaseStatistics> RWSTableEntry::GetStatistics(ClientContext &context, column_t column_id) {
	// No statistics are available without downloading the study again.
	return nullptr;
}

TableStorageInfo RWSTableEntry::GetStorageInfo(ClientContext &context) {
	return TableStorageInfo();
}

TableFunction RWSTableEntry::GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) {
	auto &rws_catalog = catalog.Cast<RWSCatalog>();
	auto data = make_uniq<RWSBindData>();
	data->connection = rws_catalog.GetConnection();
	data->spec = spec;
	data->names = GetColumns().GetColumnNames();
	data->types = GetColumns().GetColumnTypes();
	// Introspection tables are rebuilt now so they describe the catalog as it
	// stands; data tables are fetched at execution, so a discarded plan costs
	// nothing beyond the column resolution already done.
	if (builder) {
		data->bound_result = builder(context);
	}
	bind_data = std::move(data);
	return RWSGetScanFunction();
}

//===--------------------------------------------------------------------===//
// Schema entry
//===--------------------------------------------------------------------===//

RWSSchemaEntry::RWSSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, string study_oid, bool introspection)
    : SchemaCatalogEntry(catalog, info), study_oid(std::move(study_oid)), introspection(introspection) {
}

void RWSSchemaEntry::AddTable(ClientContext &context, PendingEntries &pending, const string &name,
                              const RWSRequestSpec &spec, RWSTableEntry::ResultBuilder builder) {
	auto &rws_catalog = catalog.Cast<RWSCatalog>();
	vector<string> names;
	vector<LogicalType> types;
	if (builder) {
		auto sample = builder(context);
		names = sample->names;
		types = sample->types;
	} else {
		RWSResolveColumns(context, rws_catalog.GetConnection(), spec, names, types);
	}

	CreateTableInfo info;
	info.schema = this->name;
	info.table = name;
	for (idx_t i = 0; i < names.size(); i++) {
		info.columns.AddColumn(ColumnDefinition(names[i], types[i]));
	}
	pending.entries[name] = make_uniq<RWSTableEntry>(catalog, *this, info, spec, std::move(builder));
	pending.order.push_back(name);
}

void RWSSchemaEntry::LoadEntries(ClientContext &context) {
	{
		lock_guard<mutex> guard(load_lock);
		if (loaded) {
			return;
		}
	}
	// The table set is built without the lock held: resolving it calls back
	// into the catalog (the _rws tables describe the catalog itself), and a
	// re-entrant lock here would deadlock. A schema that is still loading
	// simply looks empty to those callbacks.
	auto &rws_catalog = catalog.Cast<RWSCatalog>();
	PendingEntries pending;

	if (introspection) {
		AddTable(context, pending, "settings", RWSRequestSpec(),
		         [&rws_catalog](ClientContext &) { return rws_catalog.BuildSettingsTable(); });
		AddTable(context, pending, "tables", RWSRequestSpec(),
		         [&rws_catalog](ClientContext &scan_context) { return rws_catalog.BuildTablesTable(scan_context); });
	} else {
		// One clinical dataset request populates every form table plus the
		// derived master tables, so the whole schema resolves in a single
		// round trip.
		for (auto &master_name : RWSMasterTableNames()) {
			RWSTableKind kind;
			if (!RWSMasterTableKind(master_name, kind)) {
				throw InternalException("rws: unknown master table '%s'", master_name);
			}
			AddTable(context, pending, master_name, rws_catalog.MakeSpec(kind, study_oid), nullptr);
		}

		auto form_spec = rws_catalog.MakeSpec(RWSTableKind::FORM_WIDE, study_oid);
		auto form_oids = RWSListForms(context, rws_catalog.GetConnection(), form_spec);
		for (auto &form_oid : form_oids) {
			auto spec = form_spec;
			spec.form_oid = form_oid;
			// A form OID that collides with a master table keeps its data but
			// gets a distinct name; the mapping is visible in _rws.tables.
			auto table_name = form_oid;
			if (pending.entries.find(table_name) != pending.entries.end()) {
				table_name = form_oid + "__form";
				for (idx_t suffix = 2; pending.entries.find(table_name) != pending.entries.end(); suffix++) {
					table_name = form_oid + "__form" + to_string(suffix);
				}
			}
			AddTable(context, pending, table_name, spec, nullptr);
		}
	}

	lock_guard<mutex> guard(load_lock);
	if (loaded) {
		// Another thread finished first; keep its entries so that catalog
		// pointers already handed out stay valid.
		return;
	}
	entries = std::move(pending.entries);
	entry_order = std::move(pending.order);
	loaded = true;
}

void RWSSchemaEntry::Invalidate() {
	lock_guard<mutex> guard(load_lock);
	entries.clear();
	entry_order.clear();
	loaded = false;
}

void RWSSchemaEntry::Scan(ClientContext &context, CatalogType type,
                          const std::function<void(CatalogEntry &)> &callback) {
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	LoadEntries(context);
	lock_guard<mutex> guard(load_lock);
	for (auto &entry_name : entry_order) {
		callback(*entries[entry_name]);
	}
}

void RWSSchemaEntry::Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) {
	// Without a client context there is no way to reach the server; only what
	// has already been resolved can be listed.
	if (type != CatalogType::TABLE_ENTRY) {
		return;
	}
	lock_guard<mutex> guard(load_lock);
	for (auto &entry_name : entry_order) {
		callback(*entries[entry_name]);
	}
}

optional_ptr<CatalogEntry> RWSSchemaEntry::LookupEntry(CatalogTransaction transaction,
                                                       const EntryLookupInfo &lookup_info) {
	if (lookup_info.GetCatalogType() != CatalogType::TABLE_ENTRY) {
		return nullptr;
	}
	if (!transaction.HasContext()) {
		return nullptr;
	}
	LoadEntries(transaction.GetContext());
	lock_guard<mutex> guard(load_lock);
	auto entry = entries.find(lookup_info.GetEntryName());
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

[[noreturn]] static void ThrowReadOnly(const string &what) {
	throw NotImplementedException("rws: the attached catalog is read-only, %s is not supported", what);
}

optional_ptr<CatalogEntry> RWSSchemaEntry::CreateIndex(CatalogTransaction, CreateIndexInfo &, TableCatalogEntry &) {
	ThrowReadOnly("CREATE INDEX");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateFunction(CatalogTransaction, CreateFunctionInfo &) {
	ThrowReadOnly("CREATE FUNCTION");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateTable(CatalogTransaction, BoundCreateTableInfo &) {
	ThrowReadOnly("CREATE TABLE");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateView(CatalogTransaction, CreateViewInfo &) {
	ThrowReadOnly("CREATE VIEW");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateSequence(CatalogTransaction, CreateSequenceInfo &) {
	ThrowReadOnly("CREATE SEQUENCE");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateTableFunction(CatalogTransaction, CreateTableFunctionInfo &) {
	ThrowReadOnly("CREATE MACRO");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateCopyFunction(CatalogTransaction, CreateCopyFunctionInfo &) {
	ThrowReadOnly("CREATE COPY FUNCTION");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreatePragmaFunction(CatalogTransaction, CreatePragmaFunctionInfo &) {
	ThrowReadOnly("CREATE PRAGMA FUNCTION");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateCollation(CatalogTransaction, CreateCollationInfo &) {
	ThrowReadOnly("CREATE COLLATION");
}
optional_ptr<CatalogEntry> RWSSchemaEntry::CreateType(CatalogTransaction, CreateTypeInfo &) {
	ThrowReadOnly("CREATE TYPE");
}
void RWSSchemaEntry::DropEntry(ClientContext &, DropInfo &) {
	ThrowReadOnly("DROP");
}
void RWSSchemaEntry::Alter(CatalogTransaction, AlterInfo &) {
	ThrowReadOnly("ALTER");
}

//===--------------------------------------------------------------------===//
// Catalog
//===--------------------------------------------------------------------===//

RWSCatalog::RWSCatalog(AttachedDatabase &db, RWSConnection connection, RWSAttachOptions options)
    : Catalog(db), connection(std::move(connection)), options(std::move(options)) {
}

RWSCatalog::~RWSCatalog() = default;

void RWSCatalog::Initialize(bool load_builtin) {
	// Schemas resolve on first use: ATTACH itself performs no request.
}

RWSRequestSpec RWSCatalog::MakeSpec(RWSTableKind kind, const string &study_oid) const {
	RWSRequestSpec spec;
	spec.kind = kind;
	spec.secret_name = options.secret_name;
	spec.study_oid = study_oid;
	spec.dataset_type = options.dataset_type;
	spec.subject_include = options.subject_include;
	spec.subject_status = options.subject_status;
	spec.subject_links = options.subject_links;
	spec.subject_key_type = options.subject_key_type;
	return spec;
}

void RWSCatalog::AddSchema(const string &name, const string &schema_study_oid, bool introspection) {
	CreateSchemaInfo info;
	info.schema = name;
	schemas[name] = make_uniq<RWSSchemaEntry>(*this, info, schema_study_oid, introspection);
	schema_order.push_back(name);
}

void RWSCatalog::EnsureSchemas(ClientContext &context) {
	lock_guard<mutex> guard(schema_lock);
	if (schemas_loaded) {
		return;
	}
	AddSchema("_rws", options.study_oid, true);
	if (!options.study_oid.empty()) {
		AddSchema(options.default_schema, options.study_oid, false);
		schemas_loaded = true;
		return;
	}

	// Server mode: one schema per accessible study/environment.
	auto spec = MakeSpec(RWSTableKind::STUDIES, string());
	auto studies = RWSBuildResult(context, connection, spec);
	case_insensitive_map_t<string> claimed;
	for (auto &row : studies->rows) {
		auto study_oid = row[0].ToString();
		auto project = row[1].ToString();
		auto environment = row[2].ToString();
		if (!options.study_filter.empty()) {
			bool match = false;
			for (auto &wanted : options.study_filter) {
				if (StringUtil::CIEquals(wanted, project) || StringUtil::CIEquals(wanted, study_oid)) {
					match = true;
					break;
				}
			}
			if (!match) {
				continue;
			}
		}
		if (!options.environment_filter.empty()) {
			bool match = false;
			for (auto &wanted : options.environment_filter) {
				if (StringUtil::CIEquals(wanted, environment)) {
					match = true;
					break;
				}
			}
			if (!match) {
				continue;
			}
		}
		auto schema_name = environment.empty() ? project : project + "__" + environment;
		auto existing = claimed.find(schema_name);
		if (existing != claimed.end()) {
			// Schema names end up in user SQL, so a collision is an error
			// rather than a silently renamed schema.
			throw CatalogException("rws: studies '%s' and '%s' both map to schema '%s'; narrow the attachment with "
			                       "STUDIES or ENVIRONMENTS",
			                       existing->second, study_oid, schema_name);
		}
		claimed[schema_name] = study_oid;
		AddSchema(schema_name, study_oid, false);
	}
	schemas_loaded = true;
}

optional_ptr<CatalogEntry> RWSCatalog::CreateSchema(CatalogTransaction, CreateSchemaInfo &) {
	ThrowReadOnly("CREATE SCHEMA");
}

void RWSCatalog::DropSchema(ClientContext &, DropInfo &) {
	ThrowReadOnly("DROP SCHEMA");
}

optional_ptr<SchemaCatalogEntry> RWSCatalog::LookupSchema(CatalogTransaction transaction,
                                                          const EntryLookupInfo &schema_lookup,
                                                          OnEntryNotFound if_not_found) {
	if (!transaction.HasContext()) {
		return nullptr;
	}
	EnsureSchemas(transaction.GetContext());
	auto schema_name = schema_lookup.GetEntryName();
	if (schema_name == DEFAULT_SCHEMA) {
		schema_name = options.default_schema;
	}
	lock_guard<mutex> guard(schema_lock);
	auto entry = schemas.find(schema_name);
	if (entry != schemas.end()) {
		return entry->second.get();
	}
	if (if_not_found == OnEntryNotFound::RETURN_NULL) {
		return nullptr;
	}
	throw CatalogException(schema_lookup.GetErrorContext(), "schema '%s' does not exist in attached rws database '%s'",
	                       schema_name, GetName());
}

void RWSCatalog::ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) {
	EnsureSchemas(context);
	lock_guard<mutex> guard(schema_lock);
	for (auto &schema_name : schema_order) {
		callback(*schemas[schema_name]);
	}
}

vector<reference<RWSSchemaEntry>> RWSCatalog::LoadedSchemas() {
	lock_guard<mutex> guard(schema_lock);
	vector<reference<RWSSchemaEntry>> result;
	for (auto &schema_name : schema_order) {
		result.push_back(*schemas[schema_name]);
	}
	return result;
}

void RWSCatalog::Refresh() {
	// Only the response cache is dropped. Catalog entries stay in place
	// because already-bound plans point at them; a form that appears or
	// disappears in the source therefore needs DETACH + ATTACH, which is
	// visible to the user rather than silently changing a bound query.
	RWSInvalidateStudy(connection, options.study_oid);
}

PhysicalOperator &RWSCatalog::PlanCreateTableAs(ClientContext &, PhysicalPlanGenerator &, LogicalCreateTable &,
                                                PhysicalOperator &) {
	ThrowReadOnly("CREATE TABLE AS");
}
PhysicalOperator &RWSCatalog::PlanInsert(ClientContext &, PhysicalPlanGenerator &, LogicalInsert &,
                                         optional_ptr<PhysicalOperator>) {
	ThrowReadOnly("INSERT");
}
PhysicalOperator &RWSCatalog::PlanDelete(ClientContext &, PhysicalPlanGenerator &, LogicalDelete &,
                                         PhysicalOperator &) {
	ThrowReadOnly("DELETE");
}
PhysicalOperator &RWSCatalog::PlanUpdate(ClientContext &, PhysicalPlanGenerator &, LogicalUpdate &,
                                         PhysicalOperator &) {
	ThrowReadOnly("UPDATE");
}
unique_ptr<LogicalOperator> RWSCatalog::BindCreateIndex(Binder &, CreateStatement &, TableCatalogEntry &,
                                                        unique_ptr<LogicalOperator>) {
	ThrowReadOnly("CREATE INDEX");
}

DatabaseSize RWSCatalog::GetDatabaseSize(ClientContext &context) {
	// Nothing is stored locally, so every figure would be an invention.
	DatabaseSize size;
	return size;
}

string RWSCatalog::GetDBPath() {
	return options.study_oid.empty() ? connection.base_url : connection.base_url + "/studies/" + options.study_oid;
}

shared_ptr<RWSResult> RWSCatalog::BuildSettingsTable() const {
	auto result = make_shared_ptr<RWSResult>();
	result->names = {"key", "value"};
	result->types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto add = [&](const char *key, const string &value) {
		result->rows.push_back({Value(key), value.empty() ? Value(LogicalType::VARCHAR) : Value(value)});
	};
	add("base_url", connection.base_url);
	add("username", connection.username);
	add("secret", options.secret_name);
	add("mode", options.study_oid.empty() ? "server" : "study");
	add("study_oid", options.study_oid);
	add("dataset_type", options.dataset_type);
	add("default_schema", options.default_schema);
	add("subject_include", options.subject_include);
	add("subject_status", options.subject_status ? "true" : "false");
	add("subject_links", options.subject_links ? "true" : "false");
	add("subject_key_type", options.subject_key_type);
	add("studies_filter", StringUtil::Join(options.study_filter, ", "));
	add("environments_filter", StringUtil::Join(options.environment_filter, ", "));
	// The password lives only in the secret and is never surfaced here.
	return result;
}

shared_ptr<RWSResult> RWSCatalog::BuildTablesTable(ClientContext &context) {
	auto result = make_shared_ptr<RWSResult>();
	result->names = {"schema_name", "table_name", "study_oid", "form_oid", "column_count"};
	result->types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                 LogicalType::BIGINT};
	EnsureSchemas(context);
	for (auto &schema_reference : LoadedSchemas()) {
		auto &schema = schema_reference.get();
		// In study mode the one data schema is resolved here: it costs the
		// same single request the next query would make. In server mode only
		// what is already resolved is listed, so reading this table never
		// fans out across every study on the server.
		if (!options.study_oid.empty() && !schema.IsIntrospection()) {
			schema.LoadEntriesForIntrospection(context);
		}
		schema.Scan(CatalogType::TABLE_ENTRY, [&](CatalogEntry &entry) {
			auto &table = entry.Cast<RWSTableEntry>();
			auto &spec = table.GetSpec();
			result->rows.push_back({Value(schema.name), Value(table.name),
			                        spec.study_oid.empty() ? Value(LogicalType::VARCHAR) : Value(spec.study_oid),
			                        spec.form_oid.empty() ? Value(LogicalType::VARCHAR) : Value(spec.form_oid),
			                        Value::BIGINT(NumericCast<int64_t>(table.GetColumns().LogicalColumnCount()))});
		});
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Transaction manager
//===--------------------------------------------------------------------===//

Transaction &RWSTransactionManager::StartTransaction(ClientContext &context) {
	auto transaction = make_uniq<RWSTransaction>(*this, context);
	auto &result = *transaction;
	lock_guard<mutex> guard(transaction_lock);
	transactions.push_back(std::move(transaction));
	return result;
}

ErrorData RWSTransactionManager::CommitTransaction(ClientContext &context, Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase_at(i);
			break;
		}
	}
	return ErrorData();
}

void RWSTransactionManager::RollbackTransaction(Transaction &transaction) {
	lock_guard<mutex> guard(transaction_lock);
	for (idx_t i = 0; i < transactions.size(); i++) {
		if (transactions[i].get() == &transaction) {
			transactions.erase_at(i);
			break;
		}
	}
}

void RWSTransactionManager::Checkpoint(ClientContext &context, bool force) {
	// Nothing is stored locally; a checkpoint is a no-op rather than an error.
}

} // namespace duckdb
