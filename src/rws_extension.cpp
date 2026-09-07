#define DUCKDB_EXTENSION_MAIN

#include "rws_extension.hpp"
#include "rws_common.hpp"
#include "rws_functions.hpp"
#include "rws_catalog.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/attached_database.hpp"

namespace duckdb {

namespace {

//! rws_clear_cache() — drops every cached response held by this process.
struct ClearCacheBindData : public TableFunctionData {
	idx_t removed = 0;
	string catalog_name;
};

struct ClearCacheState : public GlobalTableFunctionState {
	bool emitted = false;
};

unique_ptr<FunctionData> ClearCacheBind(ClientContext &context, TableFunctionBindInput &input,
                                        vector<LogicalType> &return_types, vector<string> &names) {
	names = {"removed_entries"};
	return_types = {LogicalType::BIGINT};
	auto data = make_uniq<ClearCacheBindData>();
	data->removed = RWSClearCache();
	return std::move(data);
}

unique_ptr<GlobalTableFunctionState> ClearCacheInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<ClearCacheState>();
}

void ClearCacheScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ClearCacheState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	auto &bind_data = input.bind_data->Cast<ClearCacheBindData>();
	output.SetValue(0, 0, Value::BIGINT(NumericCast<int64_t>(bind_data.removed)));
	output.SetCardinality(1);
	state.emitted = true;
}

//! rws_refresh_catalog(name) — drops the cached responses behind one attached
//! rws database so the next query re-reads the study.
unique_ptr<FunctionData> RefreshCatalogBind(ClientContext &context, TableFunctionBindInput &input,
                                            vector<LogicalType> &return_types, vector<string> &names) {
	names = {"catalog_name", "removed_entries"};
	return_types = {LogicalType::VARCHAR, LogicalType::BIGINT};

	auto catalog_name = input.inputs[0].ToString();
	auto database = DatabaseManager::Get(context).GetDatabase(context, catalog_name);
	if (!database) {
		throw InvalidInputException("rws: no database named '%s' is attached", catalog_name);
	}
	auto &catalog = database->GetCatalog();
	if (catalog.GetCatalogType() != "rws") {
		throw InvalidInputException("rws: database '%s' is not an rws catalog", catalog_name);
	}
	auto &rws_catalog = catalog.Cast<RWSCatalog>();
	auto before = RWSClearCacheCount();
	rws_catalog.Refresh();
	auto data = make_uniq<ClearCacheBindData>();
	data->catalog_name = catalog_name;
	data->removed = before - RWSClearCacheCount();
	return std::move(data);
}

void RefreshCatalogScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<ClearCacheState>();
	if (state.emitted) {
		output.SetCardinality(0);
		return;
	}
	auto &bind_data = input.bind_data->Cast<ClearCacheBindData>();
	output.SetValue(0, 0, Value(bind_data.catalog_name));
	output.SetValue(1, 0, Value::BIGINT(NumericCast<int64_t>(bind_data.removed)));
	output.SetCardinality(1);
	state.emitted = true;
}

void LoadInternal(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();
	auto &config = DBConfig::GetConfig(db);

	config.AddExtensionOption(
	    "rws_cache_seconds",
	    "How long a Rave Web Services response may be reused within this process. 0 disables caching, which makes "
	    "every table re-download its study.",
	    LogicalType::BIGINT, Value::BIGINT(300));

	RWSRegisterSecretType(loader);
	RWSRegisterTableFunctions(loader);
	RWSRegisterStorageExtension(db);

	TableFunction clear_cache("rws_clear_cache", {}, ClearCacheScan, ClearCacheBind, ClearCacheInit);
	loader.RegisterFunction(clear_cache);

	TableFunction refresh_catalog("rws_refresh_catalog", {LogicalType::VARCHAR}, RefreshCatalogScan,
	                              RefreshCatalogBind, ClearCacheInit);
	loader.RegisterFunction(refresh_catalog);

	loader.SetDescription("Read Medidata Rave Web Services studies as DuckDB tables");
}

} // namespace

void RwsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string RwsExtension::Name() {
	return "rws";
}

std::string RwsExtension::Version() const {
#ifdef EXT_VERSION_RWS
	return EXT_VERSION_RWS;
#else
	return "";
#endif
}

} // namespace duckdb

extern "C" {
DUCKDB_CPP_EXTENSION_ENTRY(rws, loader) {
	duckdb::LoadInternal(loader);
}
}
