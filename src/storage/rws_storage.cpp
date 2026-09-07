#include "rws_catalog.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/attach_info.hpp"

namespace duckdb {

namespace {

vector<string> SplitList(const string &raw) {
	vector<string> result;
	for (auto &part : StringUtil::Split(raw, ',')) {
		auto trimmed = RWSTrim(part);
		if (!trimmed.empty()) {
			result.push_back(trimmed);
		}
	}
	return result;
}

RWSAttachOptions ParseAttachOptions(AttachInfo &info, AttachOptions &attach_options) {
	RWSAttachOptions options;
	string project;
	string environment;
	bool project_from_option = false;

	for (auto &entry : attach_options.options) {
		auto key = StringUtil::Lower(entry.first);
		auto value = entry.second.ToString();
		if (key == "secret") {
			options.secret_name = value;
		} else if (key == "project") {
			project = value;
			project_from_option = true;
		} else if (key == "environment") {
			environment = value;
			project_from_option = true;
		} else if (key == "dataset_type") {
			auto dataset_type = StringUtil::Lower(value);
			if (dataset_type != "regular" && dataset_type != "raw") {
				throw InvalidInputException("rws ATTACH: DATASET_TYPE must be 'regular' or 'raw', got '%s'", value);
			}
			options.dataset_type = dataset_type;
		} else if (key == "default_schema") {
			options.default_schema = value;
		} else if (key == "subject_include") {
			if (value != "inactive" && value != "inactiveAndDeleted") {
				throw InvalidInputException(
				    "rws ATTACH: SUBJECT_INCLUDE must be 'inactive' or 'inactiveAndDeleted', got '%s'", value);
			}
			options.subject_include = value;
		} else if (key == "subject_status") {
			options.subject_status = entry.second.GetValue<bool>();
		} else if (key == "subject_links") {
			options.subject_links = entry.second.GetValue<bool>();
		} else if (key == "subject_key_type") {
			if (value != "SubjectName" && value != "SubjectUUID") {
				throw InvalidInputException(
				    "rws ATTACH: SUBJECT_KEY_TYPE must be 'SubjectName' or 'SubjectUUID', got '%s'", value);
			}
			options.subject_key_type = value;
		} else if (key == "studies") {
			options.study_filter = SplitList(value);
		} else if (key == "environments") {
			options.environment_filter = SplitList(value);
		} else if (key == "type" || key == "read_only" || key == "readonly") {
			// Handled by DuckDB itself; the catalog is read-only either way.
		} else {
			throw InvalidInputException("rws ATTACH: unknown option '%s'", entry.first);
		}
	}

	auto path = RWSTrim(info.path);
	if (!path.empty() && path != "*") {
		if (project_from_option) {
			throw InvalidInputException(
			    "rws ATTACH: give the study either in the path or through PROJECT/ENVIRONMENT, not both");
		}
		auto separator = path.find('/');
		if (separator == string::npos) {
			project = path;
		} else {
			project = path.substr(0, separator);
			environment = path.substr(separator + 1);
			if (environment.find('/') != string::npos) {
				throw InvalidInputException(
				    "rws ATTACH: path must be 'PROJECT/ENVIRONMENT'; use PROJECT/ENVIRONMENT options for names "
				    "containing '/'");
			}
		}
	}

	if (!project.empty()) {
		options.study_oid = RWSMakeStudyOID(project, environment);
	} else if (!environment.empty()) {
		throw InvalidInputException("rws ATTACH: ENVIRONMENT was given without PROJECT");
	}
	if (options.study_oid.empty()) {
		// Server mode has no single default schema; DuckDB still needs a name
		// to fall back on, and 'main' would be misleading, so keep _rws.
		if (options.default_schema == "main") {
			options.default_schema = "_rws";
		}
	}
	return options;
}

unique_ptr<Catalog> RWSAttach(optional_ptr<StorageExtensionInfo> storage_info, ClientContext &context,
                              AttachedDatabase &db, const string &name, AttachInfo &info, AttachOptions &options) {
	auto attach_options = ParseAttachOptions(info, options);
	auto connection = RWSGetConnection(context, attach_options.secret_name);
	return make_uniq<RWSCatalog>(db, std::move(connection), std::move(attach_options));
}

unique_ptr<TransactionManager> RWSCreateTransactionManager(optional_ptr<StorageExtensionInfo> storage_info,
                                                           AttachedDatabase &db, Catalog &catalog) {
	return make_uniq<RWSTransactionManager>(db);
}

} // namespace

void RWSRegisterStorageExtension(DatabaseInstance &db) {
	auto storage_extension = make_shared_ptr<StorageExtension>();
	storage_extension->attach = RWSAttach;
	storage_extension->create_transaction_manager = RWSCreateTransactionManager;
	StorageExtension::Register(DBConfig::GetConfig(db), "rws", std::move(storage_extension));
}

} // namespace duckdb
