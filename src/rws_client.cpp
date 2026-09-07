#include "rws_client.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/http_exception.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/settings.hpp"

#include <chrono>
#include <thread>

#define CPPHTTPLIB_OPENSSL_SUPPORT
#ifdef __APPLE__
#define CPPHTTPLIB_USE_CERTS_FROM_MACOSX_KEYCHAIN
#endif
#include "httplib.hpp"

namespace duckdb {

namespace {

using RWSHttpClient = duckdb_httplib_openssl::Client;

struct ParsedUrl {
	string origin; // scheme://host[:port]
	string prefix; // path prefix of the service root, e.g. /RaveWebServices
};

ParsedUrl SplitBaseUrl(const string &base_url) {
	auto scheme_end = base_url.find("://");
	if (scheme_end == string::npos) {
		throw InvalidInputException("rws: malformed BASE_URL");
	}
	auto authority_end = base_url.find('/', scheme_end + 3);
	ParsedUrl result;
	if (authority_end == string::npos) {
		result.origin = base_url;
		result.prefix = "";
	} else {
		result.origin = base_url.substr(0, authority_end);
		result.prefix = base_url.substr(authority_end);
	}
	return result;
}

//! RWS answers some failures with an XML <Response .../> envelope, both under a
//! 4xx status and under 200. Pull out the parts that are safe to surface.
bool ExtractRWSError(const string &body, string &reason_code, string &message) {
	auto envelope = body.find("<Response");
	if (envelope == string::npos || envelope > 512) {
		return false;
	}
	auto read_attribute = [&](const char *name, string &out) {
		string needle = string(name) + "=\"";
		auto pos = body.find(needle, envelope);
		if (pos == string::npos) {
			return false;
		}
		pos += needle.size();
		auto end = body.find('"', pos);
		if (end == string::npos) {
			return false;
		}
		out = body.substr(pos, end - pos);
		return true;
	};
	string successful;
	if (read_attribute("IsTransactionSuccessful", successful) && successful == "1") {
		return false;
	}
	read_attribute("ReasonCode", reason_code);
	read_attribute("ErrorClientResponseMessage", message);
	return !reason_code.empty() || !message.empty();
}

//! Never let a response body reach an error message: it may hold subject data.
string DescribeFailure(const string &endpoint, int status, const string &body) {
	string reason_code;
	string message;
	if (ExtractRWSError(body, reason_code, message)) {
		return StringUtil::Format("rws request to '%s' failed (HTTP %d, %s): %s", endpoint, status,
		                          reason_code.empty() ? "no reason code" : reason_code,
		                          message.empty() ? "no server message" : message);
	}
	return StringUtil::Format("rws request to '%s' failed with HTTP %d", endpoint, status);
}

bool IsRetryableStatus(int status) {
	switch (status) {
	case 408:
	case 429:
	case 500:
	case 502:
	case 503:
	case 504:
		return true;
	default:
		return false;
	}
}

} // namespace

RWSHttpResponse RWSHttpGet(ClientContext &context, const RWSConnection &connection, const string &path_and_query) {
	auto &db_config = DBConfig::GetConfig(context);
	if (!Settings::Get<EnableExternalAccessSetting>(db_config)) {
		throw PermissionException("rws: external access is disabled (enable_external_access = false)");
	}

	auto parsed = SplitBaseUrl(connection.base_url);
	auto path = parsed.prefix + "/" + path_and_query;
	auto endpoint = parsed.origin + path;

	RWSHttpClient client(parsed.origin);
	client.set_follow_location(false); // a redirect must not carry the credentials elsewhere
	client.set_keep_alive(true);
	client.enable_server_certificate_verification(connection.verify_ssl);
	auto timeout = static_cast<time_t>(connection.timeout_seconds);
	client.set_connection_timeout(timeout < 30 ? timeout : 30, 0);
	client.set_read_timeout(timeout, 0);
	client.set_write_timeout(timeout, 0);
	if (!connection.username.empty()) {
		client.set_basic_auth(connection.username, connection.password);
	}

	uint64_t attempt = 0;
	while (true) {
		if (context.interrupted) {
			throw InterruptException();
		}
		auto response = client.Get(path.c_str());
		if (!response) {
			// Transport-level failure. httplib error names carry no credentials.
			if (attempt < connection.max_retries) {
				attempt++;
				std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
				continue;
			}
			throw IOException("rws request to '%s' failed: %s", endpoint,
			                  duckdb_httplib_openssl::to_string(response.error()));
		}
		auto status = response->status;
		if (status >= 200 && status < 300) {
			string reason_code;
			string message;
			if (ExtractRWSError(response->body, reason_code, message)) {
				throw IOException(DescribeFailure(endpoint, status, response->body));
			}
			RWSHttpResponse result;
			result.status = status;
			result.body = std::move(response->body);
			result.content_type = response->get_header_value("Content-Type");
			result.cv_last_updated = response->get_header_value("X-MWS-CV-Last-Updated");
			return result;
		}
		if (status == 501) {
			throw NotImplementedException(
			    "%s. This Rave deployment does not implement that endpoint or option — the incremental 'start' "
			    "parameter and the Biostats Gateway datasets are the usual cases",
			    DescribeFailure(endpoint, status, response->body));
		}
		if (status == 401 || status == 403) {
			throw IOException(DescribeFailure(endpoint, status, response->body) +
			                  " — check the secret's USERNAME/PASSWORD and the account's study access");
		}
		if (IsRetryableStatus(status) && attempt < connection.max_retries) {
			attempt++;
			uint64_t wait_ms = 200 * attempt;
			auto retry_after = response->get_header_value("Retry-After");
			if (!retry_after.empty()) {
				try {
					auto seconds = std::stoul(retry_after);
					if (seconds <= 60) {
						wait_ms = seconds * 1000;
					}
				} catch (...) {
					// A date-formatted Retry-After is ignored; the backoff below still applies.
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
			continue;
		}
		throw IOException(DescribeFailure(endpoint, status, response->body));
	}
}

} // namespace duckdb
