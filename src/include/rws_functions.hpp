//===----------------------------------------------------------------------===//
//                         DuckDB RWS extension
//
// rws_functions.hpp
//===----------------------------------------------------------------------===//

#pragma once

#include "rws_request.hpp"
#include "duckdb/function/table_function.hpp"

namespace duckdb {

class ExtensionLoader;

//! Bind data shared by the table functions and by catalog table scans.
struct RWSBindData : public TableFunctionData {
	RWSConnection connection;
	RWSRequestSpec spec;
	vector<string> names;
	vector<LogicalType> types;
	//! Result produced while binding. Reused by the first scan so that a plain
	//! query does not fetch twice.
	shared_ptr<RWSResult> bound_result;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

//! The scan shared by every RWS table. Rows are materialised up front because
//! the underlying response must be downloaded and validated in full regardless.
TableFunction RWSGetScanFunction();

void RWSRegisterTableFunctions(ExtensionLoader &loader);

} // namespace duckdb
