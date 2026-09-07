#include "rws_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

unique_ptr<FunctionData> RWSBindData::Copy() const {
	auto copy = make_uniq<RWSBindData>();
	copy->connection = connection;
	copy->spec = spec;
	copy->names = names;
	copy->types = types;
	copy->bound_result = bound_result;
	return std::move(copy);
}

bool RWSBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<RWSBindData>();
	return spec.CacheKey(connection) == other.spec.CacheKey(other.connection) && names == other.names;
}

namespace {

struct RWSGlobalState : public GlobalTableFunctionState {
	shared_ptr<RWSResult> result;
	idx_t offset = 0;
	vector<column_t> column_ids;

	idx_t MaxThreads() const override {
		return 1;
	}
};

unique_ptr<GlobalTableFunctionState> RWSInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<RWSBindData>();
	auto state = make_uniq<RWSGlobalState>();
	state->column_ids = input.column_ids;

	// Reuse the rows produced during binding when they are still the ones this
	// scan needs; otherwise fetch again (the response cache usually absorbs it).
	auto result = bind_data.bound_result;
	if (!result) {
		result = RWSBuildResult(context, bind_data.connection, bind_data.spec);
	}
	if (result->names != bind_data.names) {
		throw IOException("rws: the shape of '%s' changed between binding and execution — re-run the query so it can "
		                  "be bound against the current columns",
		                  bind_data.spec.form_oid.empty() ? bind_data.spec.study_oid : bind_data.spec.form_oid);
	}
	state->result = std::move(result);
	return std::move(state);
}

void RWSScan(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<RWSGlobalState>();
	auto &rows = state.result->rows;
	idx_t count = 0;
	while (state.offset < rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = rows[state.offset];
		for (idx_t out_index = 0; out_index < state.column_ids.size(); out_index++) {
			auto column_id = state.column_ids[out_index];
			if (IsRowIdColumnId(column_id)) {
				output.SetValue(out_index, count, Value::BIGINT(NumericCast<int64_t>(state.offset)));
			} else {
				output.SetValue(out_index, count, row[column_id]);
			}
		}
		state.offset++;
		count++;
	}
	output.SetCardinality(count);
}

unique_ptr<NodeStatistics> RWSCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<RWSBindData>();
	if (!bind_data.bound_result) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(bind_data.bound_result->rows.size(), bind_data.bound_result->rows.size());
}

//===--------------------------------------------------------------------===//
// Argument handling
//===--------------------------------------------------------------------===//

string RequireVarchar(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw InvalidInputException("rws: %s must not be NULL", what);
	}
	auto text = value.ToString();
	if (RWSTrim(text).empty()) {
		throw InvalidInputException("rws: %s must not be empty", what);
	}
	return text;
}

void ApplyNamedParameters(TableFunctionBindInput &input, RWSRequestSpec &spec) {
	for (auto &entry : input.named_parameters) {
		auto name = StringUtil::Lower(entry.first);
		auto &value = entry.second;
		if (name == "secret") {
			spec.secret_name = value.ToString();
		} else if (name == "dataset_type") {
			auto dataset_type = StringUtil::Lower(value.ToString());
			if (dataset_type != "regular" && dataset_type != "raw") {
				throw InvalidInputException("rws: dataset_type must be 'regular' or 'raw', got '%s'", dataset_type);
			}
			spec.dataset_type = dataset_type;
		} else if (name == "subject_key") {
			spec.subject_key = value.ToString();
		} else if (name == "start") {
			spec.start = value.ToString();
		} else if (name == "study_oid") {
			spec.study_oid = value.ToString();
		} else if (name == "form_oid") {
			spec.form_oid = value.ToString();
		} else if (name == "include") {
			auto include = value.ToString();
			if (include != "inactive" && include != "inactiveAndDeleted") {
				throw InvalidInputException(
				    "rws: include must be 'inactive' or 'inactiveAndDeleted', got '%s' — RWS ignores unknown values "
				    "silently, so they are rejected here",
				    include);
			}
			spec.subject_include = include;
		} else if (name == "status") {
			spec.subject_status = value.GetValue<bool>();
		} else if (name == "links") {
			spec.subject_links = value.GetValue<bool>();
		} else if (name == "subject_key_type") {
			auto key_type = value.ToString();
			if (key_type != "SubjectName" && key_type != "SubjectUUID") {
				throw InvalidInputException("rws: subject_key_type must be 'SubjectName' or 'SubjectUUID', got '%s'",
				                            key_type);
			}
			spec.subject_key_type = key_type;
		} else if (name == "refresh") {
			spec.refresh = value.GetValue<bool>();
		} else if (name == "timeout_seconds" || name == "max_retries") {
			// Handled after the connection is resolved.
		} else {
			throw InvalidInputException("rws: unknown named parameter '%s'", entry.first);
		}
	}
}

void ApplyConnectionOverrides(TableFunctionBindInput &input, RWSConnection &connection) {
	for (auto &entry : input.named_parameters) {
		auto name = StringUtil::Lower(entry.first);
		if (name == "timeout_seconds") {
			auto seconds = entry.second.GetValue<int64_t>();
			if (seconds <= 0) {
				throw InvalidInputException("rws: timeout_seconds must be positive");
			}
			connection.timeout_seconds = NumericCast<uint64_t>(seconds);
		} else if (name == "max_retries") {
			auto retries = entry.second.GetValue<int64_t>();
			if (retries < 0) {
				throw InvalidInputException("rws: max_retries must not be negative");
			}
			connection.max_retries = NumericCast<uint64_t>(retries);
		}
	}
}

template <RWSTableKind KIND, idx_t POSITIONAL_ARGUMENTS>
unique_ptr<FunctionData> RWSBind(ClientContext &context, TableFunctionBindInput &input,
                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto bind_data = make_uniq<RWSBindData>();
	bind_data->spec.kind = KIND;

	if (POSITIONAL_ARGUMENTS >= 2) {
		auto project = RequireVarchar(input.inputs[0], "project");
		auto environment = RequireVarchar(input.inputs[1], "environment");
		bind_data->spec.study_oid = RWSMakeStudyOID(project, environment);
	}
	if (POSITIONAL_ARGUMENTS >= 3) {
		bind_data->spec.form_oid = RequireVarchar(input.inputs[2], "form_oid");
	}
	ApplyNamedParameters(input, bind_data->spec);

	bind_data->connection = RWSGetConnection(context, bind_data->spec.secret_name);
	ApplyConnectionOverrides(input, bind_data->connection);

	bind_data->bound_result = RWSBuildResult(context, bind_data->connection, bind_data->spec);
	bind_data->names = bind_data->bound_result->names;
	bind_data->types = bind_data->bound_result->types;
	names = bind_data->names;
	return_types = bind_data->types;
	return std::move(bind_data);
}

void AddCommonParameters(TableFunction &function) {
	function.named_parameters["secret"] = LogicalType::VARCHAR;
	function.named_parameters["timeout_seconds"] = LogicalType::BIGINT;
	function.named_parameters["max_retries"] = LogicalType::BIGINT;
	function.named_parameters["refresh"] = LogicalType::BOOLEAN;
}

void AddDatasetParameters(TableFunction &function) {
	function.named_parameters["dataset_type"] = LogicalType::VARCHAR;
	function.named_parameters["subject_key"] = LogicalType::VARCHAR;
	function.named_parameters["start"] = LogicalType::VARCHAR;
}

} // namespace

TableFunction RWSGetScanFunction() {
	TableFunction function("rws_scan", {}, RWSScan, nullptr, RWSInitGlobal);
	function.cardinality = RWSCardinality;
	function.projection_pushdown = true;
	return function;
}

void RWSRegisterTableFunctions(ExtensionLoader &loader) {
	{
		TableFunction function("rws_version", {}, RWSScan, RWSBind<RWSTableKind::SERVER_VERSION, 0>, RWSInitGlobal);
		function.projection_pushdown = true;
		AddCommonParameters(function);
		loader.RegisterFunction(function);
	}
	{
		TableFunction function("rws_studies", {}, RWSScan, RWSBind<RWSTableKind::STUDIES, 0>, RWSInitGlobal);
		function.projection_pushdown = true;
		AddCommonParameters(function);
		function.named_parameters["study_oid"] = LogicalType::VARCHAR;
		loader.RegisterFunction(function);
	}
	struct StudyScopedFunction {
		const char *name;
		RWSTableKind kind;
		bool dataset_based;
		bool subject_options;
	};
	static const StudyScopedFunction study_functions[] = {
	    {"rws_subjects", RWSTableKind::SUBJECTS, false, true},
	    {"rws_sites", RWSTableKind::SITES, false, true},
	    {"rws_forms", RWSTableKind::FORMS, true, false},
	    {"rws_study_events", RWSTableKind::STUDY_EVENTS, true, false},
	    {"rws_items", RWSTableKind::ITEMS, true, false},
	    {"rws_clinical_items", RWSTableKind::CLINICAL_ITEMS, true, false},
	};
	for (auto &entry : study_functions) {
		TableFunction function(entry.name, {LogicalType::VARCHAR, LogicalType::VARCHAR}, RWSScan, nullptr,
		                       RWSInitGlobal);
		function.projection_pushdown = true;
		AddCommonParameters(function);
		if (entry.dataset_based) {
			AddDatasetParameters(function);
			function.named_parameters["form_oid"] = LogicalType::VARCHAR;
		}
		if (entry.subject_options) {
			function.named_parameters["include"] = LogicalType::VARCHAR;
			function.named_parameters["status"] = LogicalType::BOOLEAN;
			function.named_parameters["links"] = LogicalType::BOOLEAN;
			function.named_parameters["subject_key_type"] = LogicalType::VARCHAR;
		}
		switch (entry.kind) {
		case RWSTableKind::SUBJECTS:
			function.bind = RWSBind<RWSTableKind::SUBJECTS, 2>;
			break;
		case RWSTableKind::SITES:
			function.bind = RWSBind<RWSTableKind::SITES, 2>;
			break;
		case RWSTableKind::FORMS:
			function.bind = RWSBind<RWSTableKind::FORMS, 2>;
			break;
		case RWSTableKind::STUDY_EVENTS:
			function.bind = RWSBind<RWSTableKind::STUDY_EVENTS, 2>;
			break;
		case RWSTableKind::ITEMS:
			function.bind = RWSBind<RWSTableKind::ITEMS, 2>;
			break;
		case RWSTableKind::CLINICAL_ITEMS:
			function.bind = RWSBind<RWSTableKind::CLINICAL_ITEMS, 2>;
			break;
		default:
			throw InternalException("rws: unexpected study scoped function");
		}
		loader.RegisterFunction(function);
	}
	{
		TableFunction function("rws_form", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR}, RWSScan,
		                       RWSBind<RWSTableKind::FORM_WIDE, 3>, RWSInitGlobal);
		function.projection_pushdown = true;
		AddCommonParameters(function);
		AddDatasetParameters(function);
		loader.RegisterFunction(function);
	}
	{
		TableFunction function("rws_form_columns", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
		                       RWSScan, RWSBind<RWSTableKind::FORM_COLUMNS, 3>, RWSInitGlobal);
		function.projection_pushdown = true;
		AddCommonParameters(function);
		AddDatasetParameters(function);
		loader.RegisterFunction(function);
	}
}

} // namespace duckdb
