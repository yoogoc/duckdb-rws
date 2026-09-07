#include "rws_common.hpp"

#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

static constexpr const char *RWS_SECRET_TYPE = "rws";

static string NormalizeBaseUrl(const string &raw) {
	auto url = RWSTrim(raw);
	if (url.empty()) {
		throw InvalidInputException("rws secret: BASE_URL is required");
	}
	auto lower = StringUtil::Lower(url);
	if (!StringUtil::StartsWith(lower, "http://") && !StringUtil::StartsWith(lower, "https://")) {
		throw InvalidInputException("rws secret: BASE_URL must start with http:// or https://");
	}
	// Credentials embedded in the URL would leak through error messages and logs.
	auto authority_start = url.find("://") + 3;
	auto authority_end = url.find('/', authority_start);
	auto authority =
	    url.substr(authority_start, authority_end == string::npos ? string::npos : authority_end - authority_start);
	if (authority.find('@') != string::npos) {
		throw InvalidInputException("rws secret: BASE_URL must not embed credentials, use USERNAME/PASSWORD");
	}
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	// Accept both the service root and the bare host: RWS always lives under /RaveWebServices.
	if (!StringUtil::EndsWith(StringUtil::Lower(url), "/ravewebservices")) {
		url += "/RaveWebServices";
	}
	return url;
}

static unique_ptr<BaseSecret> CreateRWSSecret(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	if (scope.empty()) {
		scope.push_back("rws://");
	}
	auto secret = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	for (const auto &named_param : input.options) {
		auto lower_name = StringUtil::Lower(named_param.first);
		if (lower_name == "base_url" || lower_name == "url" || lower_name == "endpoint") {
			secret->secret_map["base_url"] = Value(NormalizeBaseUrl(named_param.second.ToString()));
		} else if (lower_name == "username" || lower_name == "user") {
			secret->secret_map["username"] = Value(named_param.second.ToString());
		} else if (lower_name == "password") {
			secret->secret_map["password"] = Value(named_param.second.ToString());
		} else if (lower_name == "auth_type") {
			auto auth = StringUtil::Lower(named_param.second.ToString());
			if (auth != "basic") {
				throw InvalidInputException("rws secret: AUTH_TYPE '%s' is not supported, only 'basic' is implemented",
				                            auth);
			}
			secret->secret_map["auth_type"] = Value(auth);
		} else if (lower_name == "verify_ssl") {
			secret->secret_map["verify_ssl"] = Value::BOOLEAN(named_param.second.GetValue<bool>());
		} else if (lower_name == "timeout_seconds") {
			secret->secret_map["timeout_seconds"] = Value::BIGINT(named_param.second.GetValue<int64_t>());
		} else {
			throw InvalidInputException("rws secret: unknown parameter '%s'", named_param.first);
		}
	}
	if (secret->secret_map.find("base_url") == secret->secret_map.end()) {
		throw InvalidInputException("rws secret: BASE_URL is required");
	}
	secret->redact_keys = {"password"};
	return std::move(secret);
}

void RWSRegisterSecretType(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = RWS_SECRET_TYPE;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction function = {RWS_SECRET_TYPE, "config", CreateRWSSecret};
	function.named_parameters["base_url"] = LogicalType::VARCHAR;
	function.named_parameters["url"] = LogicalType::VARCHAR;
	function.named_parameters["endpoint"] = LogicalType::VARCHAR;
	function.named_parameters["username"] = LogicalType::VARCHAR;
	function.named_parameters["user"] = LogicalType::VARCHAR;
	function.named_parameters["password"] = LogicalType::VARCHAR;
	function.named_parameters["auth_type"] = LogicalType::VARCHAR;
	function.named_parameters["verify_ssl"] = LogicalType::BOOLEAN;
	function.named_parameters["timeout_seconds"] = LogicalType::BIGINT;
	loader.RegisterFunction(function);
}

RWSConnection RWSGetConnection(ClientContext &context, const string &secret_name) {
	auto &secret_manager = SecretManager::Get(context);
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);

	unique_ptr<SecretEntry> entry;
	if (secret_name.empty()) {
		auto match = secret_manager.LookupSecret(transaction, "rws://", RWS_SECRET_TYPE);
		if (match.HasMatch()) {
			entry = std::move(match.secret_entry);
		}
		if (!entry) {
			throw InvalidInputException("no 'rws' secret found. Create one with:\n"
			                            "  CREATE SECRET (TYPE rws, BASE_URL 'https://host/RaveWebServices', "
			                            "USERNAME '...', PASSWORD '...');");
		}
	} else {
		entry = secret_manager.GetSecretByName(transaction, secret_name);
		if (!entry) {
			throw InvalidInputException("secret '%s' does not exist", secret_name);
		}
		if (entry->secret->GetType() != RWS_SECRET_TYPE) {
			throw InvalidInputException("secret '%s' is of type '%s', expected 'rws'", secret_name,
			                            entry->secret->GetType());
		}
	}

	const auto &kv_secret = dynamic_cast<const KeyValueSecret &>(*entry->secret);
	RWSConnection connection;
	Value value;
	if (!kv_secret.TryGetValue("base_url", value)) {
		throw InvalidInputException("rws secret '%s' has no BASE_URL", entry->secret->GetName());
	}
	connection.base_url = value.ToString();
	if (kv_secret.TryGetValue("username", value)) {
		connection.username = value.ToString();
	}
	if (kv_secret.TryGetValue("password", value)) {
		connection.password = value.ToString();
	}
	if (kv_secret.TryGetValue("verify_ssl", value) && !value.IsNull()) {
		connection.verify_ssl = value.GetValue<bool>();
	}
	if (kv_secret.TryGetValue("timeout_seconds", value) && !value.IsNull()) {
		auto timeout = value.GetValue<int64_t>();
		if (timeout <= 0) {
			throw InvalidInputException("rws secret: TIMEOUT_SECONDS must be positive");
		}
		connection.timeout_seconds = NumericCast<uint64_t>(timeout);
	}
	return connection;
}

} // namespace duckdb
