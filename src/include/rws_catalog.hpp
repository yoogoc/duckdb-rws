//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_catalog.hpp
//
// Read-only catalog exposing a Rave study (or a whole server) as an attached
// database. It is a naming layer over the same readers the table functions
// use: identical rows, identical validation.
//===----------------------------------------------------------------------===//

#pragma once

#include "rws_request.hpp"
#include "rws_functions.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "duckdb/transaction/transaction_manager.hpp"

namespace duckdb {

//! Options fixed for the lifetime of one attachment. Table-level parameters do
//! not exist in SQL, so anything a table function takes per call has to be
//! decided here or stay with the table functions.
struct RWSAttachOptions {
	string secret_name;
	//! Empty in server mode; otherwise the single study this database exposes.
	string study_oid;
	string dataset_type = "regular";
	string subject_include;
	bool subject_status = false;
	bool subject_links = false;
	string subject_key_type;
	string default_schema = "main";
	//! Server mode only: restricts which studies become schemas.
	vector<string> study_filter;
	vector<string> environment_filter;
};

class RWSCatalog;

class RWSTableEntry : public TableCatalogEntry {
public:
	//! Rebuilds an introspection table's rows. Called per query so that
	//! `_rws.tables` reflects the catalog as it stands now, not as it stood
	//! when the schema was first resolved.
	using ResultBuilder = std::function<shared_ptr<RWSResult>(ClientContext &)>;

	RWSTableEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateTableInfo &info, RWSRequestSpec spec,
	              ResultBuilder builder);

	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, column_t column_id) override;
	TableFunction GetScanFunction(ClientContext &context, unique_ptr<FunctionData> &bind_data) override;
	TableStorageInfo GetStorageInfo(ClientContext &context) override;

	const RWSRequestSpec &GetSpec() const {
		return spec;
	}

private:
	RWSRequestSpec spec;
	//! Set for catalog introspection tables, whose rows are local state.
	ResultBuilder builder;
};

class RWSSchemaEntry : public SchemaCatalogEntry {
public:
	RWSSchemaEntry(Catalog &catalog, CreateSchemaInfo &info, string study_oid, bool introspection);

	void Scan(ClientContext &context, CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	void Scan(CatalogType type, const std::function<void(CatalogEntry &)> &callback) override;
	optional_ptr<CatalogEntry> LookupEntry(CatalogTransaction transaction, const EntryLookupInfo &lookup_info) override;

	optional_ptr<CatalogEntry> CreateIndex(CatalogTransaction transaction, CreateIndexInfo &info,
	                                       TableCatalogEntry &table) override;
	optional_ptr<CatalogEntry> CreateFunction(CatalogTransaction transaction, CreateFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateTable(CatalogTransaction transaction, BoundCreateTableInfo &info) override;
	optional_ptr<CatalogEntry> CreateView(CatalogTransaction transaction, CreateViewInfo &info) override;
	optional_ptr<CatalogEntry> CreateSequence(CatalogTransaction transaction, CreateSequenceInfo &info) override;
	optional_ptr<CatalogEntry> CreateTableFunction(CatalogTransaction transaction,
	                                               CreateTableFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCopyFunction(CatalogTransaction transaction,
	                                              CreateCopyFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreatePragmaFunction(CatalogTransaction transaction,
	                                                CreatePragmaFunctionInfo &info) override;
	optional_ptr<CatalogEntry> CreateCollation(CatalogTransaction transaction, CreateCollationInfo &info) override;
	optional_ptr<CatalogEntry> CreateType(CatalogTransaction transaction, CreateTypeInfo &info) override;
	void DropEntry(ClientContext &context, DropInfo &info) override;
	void Alter(CatalogTransaction transaction, AlterInfo &info) override;

	//! Drops the cached table set so the next lookup rebuilds it.
	void Invalidate();

	//! Resolves the table set for `_rws.tables`. Public because the catalog
	//! drives it; a schema that is mid-load simply stays empty.
	void LoadEntriesForIntrospection(ClientContext &context) {
		LoadEntries(context);
	}

	const string &StudyOID() const {
		return study_oid;
	}
	bool IsIntrospection() const {
		return introspection;
	}

private:
	//! Table set under construction, so that building it can call back into
	//! the catalog without holding this schema's lock.
	struct PendingEntries {
		vector<string> order;
		case_insensitive_map_t<unique_ptr<RWSTableEntry>> entries;
	};

	void LoadEntries(ClientContext &context);
	void AddTable(ClientContext &context, PendingEntries &pending, const string &name, const RWSRequestSpec &spec,
	              RWSTableEntry::ResultBuilder builder);

private:
	string study_oid;
	//! True for the `_rws` schema, whose tables describe the attachment itself.
	bool introspection;
	mutex load_lock;
	bool loaded = false;
	vector<string> entry_order;
	case_insensitive_map_t<unique_ptr<RWSTableEntry>> entries;
};

class RWSCatalog : public Catalog {
public:
	RWSCatalog(AttachedDatabase &db, RWSConnection connection, RWSAttachOptions options);
	~RWSCatalog() override;

	void Initialize(bool load_builtin) override;
	string GetCatalogType() override {
		return "rws";
	}
	string GetDefaultSchema() const override {
		return options.default_schema;
	}

	optional_ptr<CatalogEntry> CreateSchema(CatalogTransaction transaction, CreateSchemaInfo &info) override;
	void DropSchema(ClientContext &context, DropInfo &info) override;
	optional_ptr<SchemaCatalogEntry> LookupSchema(CatalogTransaction transaction, const EntryLookupInfo &schema_lookup,
	                                              OnEntryNotFound if_not_found) override;
	void ScanSchemas(ClientContext &context, std::function<void(SchemaCatalogEntry &)> callback) override;

	PhysicalOperator &PlanCreateTableAs(ClientContext &context, PhysicalPlanGenerator &planner, LogicalCreateTable &op,
	                                    PhysicalOperator &plan) override;
	PhysicalOperator &PlanInsert(ClientContext &context, PhysicalPlanGenerator &planner, LogicalInsert &op,
	                             optional_ptr<PhysicalOperator> plan) override;
	PhysicalOperator &PlanDelete(ClientContext &context, PhysicalPlanGenerator &planner, LogicalDelete &op,
	                             PhysicalOperator &plan) override;
	PhysicalOperator &PlanUpdate(ClientContext &context, PhysicalPlanGenerator &planner, LogicalUpdate &op,
	                             PhysicalOperator &plan) override;
	unique_ptr<LogicalOperator> BindCreateIndex(Binder &binder, CreateStatement &stmt, TableCatalogEntry &table,
	                                            unique_ptr<LogicalOperator> plan) override;

	DatabaseSize GetDatabaseSize(ClientContext &context) override;
	bool InMemory() override {
		return false;
	}
	string GetDBPath() override;

	const RWSConnection &GetConnection() const {
		return connection;
	}
	const RWSAttachOptions &GetOptions() const {
		return options;
	}
	//! Fills in the attachment-wide options on a spec.
	RWSRequestSpec MakeSpec(RWSTableKind kind, const string &study_oid) const;
	//! Rows describing this attachment, for the `_rws` schema.
	shared_ptr<RWSResult> BuildSettingsTable() const;
	//! In study mode the single data schema is resolved first, since that
	//! costs the one request the user's next query would make anyway. In
	//! server mode only schemas already resolved are listed, so that reading
	//! this table never fans out across every study on the server.
	shared_ptr<RWSResult> BuildTablesTable(ClientContext &context);
	//! Forgets cached schemas and responses for this attachment.
	void Refresh();

	//! Registered schemas, in creation order.
	vector<reference<RWSSchemaEntry>> LoadedSchemas();

private:
	void EnsureSchemas(ClientContext &context);
	void AddSchema(const string &name, const string &study_oid, bool introspection);

private:
	RWSConnection connection;
	RWSAttachOptions options;
	mutex schema_lock;
	bool schemas_loaded = false;
	vector<string> schema_order;
	case_insensitive_map_t<unique_ptr<RWSSchemaEntry>> schemas;
};

//! A placeholder transaction: RWS has no transactions and the catalog is
//! read-only, so nothing is tracked beyond DuckDB's own bookkeeping.
class RWSTransaction : public Transaction {
public:
	RWSTransaction(TransactionManager &manager, ClientContext &context) : Transaction(manager, context) {
	}
};

class RWSTransactionManager : public TransactionManager {
public:
	explicit RWSTransactionManager(AttachedDatabase &db) : TransactionManager(db) {
	}

	Transaction &StartTransaction(ClientContext &context) override;
	ErrorData CommitTransaction(ClientContext &context, Transaction &transaction) override;
	void RollbackTransaction(Transaction &transaction) override;
	void Checkpoint(ClientContext &context, bool force = false) override;

private:
	mutex transaction_lock;
	vector<unique_ptr<RWSTransaction>> transactions;
};

//! Registers the storage extension backing `ATTACH ... (TYPE rws)`.
void RWSRegisterStorageExtension(DatabaseInstance &db);

} // namespace duckdb
