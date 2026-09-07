#!/usr/bin/env bash
# Drives the rws extension against test/http/mock_rws.py. No credentials and no
# network access are needed; every assertion is about protocol handling.
set -uo pipefail
cd "$(dirname "$0")/../.."

DUCKDB=${DUCKDB:-./build/release/duckdb}
if [ ! -x "$DUCKDB" ]; then
	echo "duckdb binary not found at $DUCKDB (run 'make release' first)" >&2
	exit 1
fi

port_file=$(mktemp)
python3 test/http/mock_rws.py > "$port_file" 2>/dev/null &
server_pid=$!
cleanup() { kill "$server_pid" 2>/dev/null; wait "$server_pid" 2>/dev/null; rm -f "$port_file"; }
trap cleanup EXIT

for _ in $(seq 1 50); do
	PORT=$(head -1 "$port_file" 2>/dev/null)
	[ -n "${PORT:-}" ] && break
	sleep 0.1
done
if [ -z "${PORT:-}" ]; then
	echo "mock server did not start" >&2
	exit 1
fi

BASE="http://127.0.0.1:$PORT/RaveWebServices"
failures=0
run() { # run <name> <sql> <expected substring>
	local name=$1 sql=$2 expected=$3
	local out
	out=$("$DUCKDB" -init /dev/null -noheader -list -c "
		SET rws_cache_seconds = 0;
		CREATE SECRET rave (TYPE rws, BASE_URL '$BASE', USERNAME 'tester', PASSWORD 's3cret');
		$sql" 2>&1)
	if printf '%s' "$out" | grep -qF -- "$expected"; then
		printf 'ok   %s\n' "$name"
	else
		printf 'FAIL %s\n     expected to find: %s\n     got: %s\n' "$name" "$expected" "$(printf '%s' "$out" | head -5)"
		failures=$((failures + 1))
	fi
}

# Entity references, CDATA and a BOM all decode; the study OID splits correctly.
run "studies: entities and CDATA" \
	"SELECT study_oid || '|' || project_name || '|' || environment || '|' || study_name || '|' || study_description FROM rws_studies();" \
	"ACME & Co(Prod)|ACME & Co|Prod|ACME & Co (Prod)|café study"

# The ODM namespace bound to a non-default prefix must parse the same way.
run "subjects: namespace prefixes are irrelevant" \
	"SELECT subject_key || '|' || site_oid || '|' || coalesce(site_number,'-') || '|' || is_active FROM rws_subjects('ACME & Co','Prod') ORDER BY 1 LIMIT 1;" \
	"S-1|SITE-1|001|true"

run "subjects: absent attributes are NULL, not empty" \
	"SELECT is_active IS NULL AND site_number IS NULL FROM rws_subjects('ACME & Co','Prod') WHERE subject_key='S-2';" \
	"true"

run "subjects: links are captured" \
	"SELECT links[1] FROM rws_subjects('ACME & Co','Prod') WHERE subject_key='S-1';" \
	"https://example.invalid/s/1"

# An empty Value is data; IsNull="Yes" without a Value is SQL NULL.
run "wide form: empty string differs from NULL" \
	"SELECT '[' || EMPTY || ']' || '|' || coalesce(MISSING,'<null>') FROM rws_form('ACME & Co','Prod','VITAL') WHERE subject_key='S-1';" \
	"[]|<null>"

run "wide form: escaped text round-trips" \
	"SELECT NOTE FROM rws_form('ACME & Co','Prod','VITAL') WHERE subject_key='S-1';" \
	"a <b> & c"

# The column set is the union across records; a record without a column gets NULL.
run "wide form: union of item OIDs across records" \
	"SELECT count(*) FROM (DESCRIBE SELECT * FROM rws_form('ACME & Co','Prod','VITAL'));" \
	"16"

run "wide form: missing item is NULL for the other record" \
	"SELECT EXTRA IS NULL FROM rws_form('ACME & Co','Prod','VITAL') WHERE subject_key='S-1';" \
	"true"

run "long table: is_null only when the source says so" \
	"SELECT count(*) FILTER (WHERE is_null) || '|' || count(*) FILTER (WHERE is_null IS NULL) FROM rws_clinical_items('ACME & Co','Prod');" \
	"1|5"

run "sites derive from subject references" \
	"SELECT string_agg(site_oid, ',' ORDER BY site_oid) FROM rws_sites('ACME & Co','Prod');" \
	"SITE-1,SITE-2"

# A business error carried by a 200 response must fail the query.
run "business error under HTTP 200 fails" \
	"SELECT * FROM rws_clinical_items('BUSERR','Prod');" \
	"You do not have access to this study."

run "business error names its reason code" \
	"SELECT * FROM rws_clinical_items('BUSERR','Prod');" \
	"RWS00024"

# A DTD is where XML external entity attacks live; refuse the document.
run "DOCTYPE declarations are refused" \
	"SELECT * FROM rws_clinical_items('DOCTYPE','Prod');" \
	"document type declarations are not accepted"

run "truncated documents fail instead of returning partial rows" \
	"SELECT * FROM rws_clinical_items('TRUNC','Prod');" \
	"unclosed elements"

# A redirect must not carry the credentials to another host.
run "cross-origin redirects are not followed" \
	"SELECT * FROM rws_clinical_items('REDIR','Prod');" \
	"HTTP 302"

# 503 is retried; the third attempt succeeds.
run "transient 5xx is retried" \
	"SELECT count(*) FROM rws_clinical_items('FLAKY','Prod');" \
	"6"

run "unknown dataset_type is rejected before any request" \
	"SELECT * FROM rws_clinical_items('ACME & Co','Prod', dataset_type := 'doctype');" \
	"dataset_type must be"

# Wrong credentials produce an actionable message and never echo the password.
run "401 is reported without the password" \
	"CREATE SECRET wrong (TYPE rws, BASE_URL '$BASE', USERNAME 'tester', PASSWORD 'nope');
	 SELECT * FROM rws_studies(secret := 'wrong');" \
	"Invalid user name or password."

run "401 message does not leak the password" \
	"CREATE SECRET wrong2 (TYPE rws, BASE_URL '$BASE', USERNAME 'tester', PASSWORD 'hunter2');
	 SELECT * FROM rws_studies(secret := 'wrong2');" \
	"RWS00024"

run "missing endpoint reports the RWS reason code" \
	"SELECT * FROM rws_subjects('NOPE','Prod');" \
	"RWS URL does not exist"

# The catalog serves the same rows as the table functions.
run "ATTACH exposes forms and master tables" \
	"ATTACH 'ACME & Co/Prod' AS s (TYPE rws, SECRET rave);
	 SELECT string_agg(table_name, ',' ORDER BY table_name) FROM duckdb_tables() WHERE database_name='s';" \
	"VITAL,clinical_items,form_columns,forms,items,settings,sites,studies,study_events,subjects,tables"

run "ATTACH rows match the table function" \
	"ATTACH 'ACME & Co/Prod' AS s (TYPE rws, SECRET rave);
	 SELECT (SELECT count(*) FROM s.main.VITAL) = (SELECT count(*) FROM rws_form('ACME & Co','Prod','VITAL'));" \
	"true"

run "ATTACH refuses writes" \
	"ATTACH 'ACME & Co/Prod' AS s (TYPE rws, SECRET rave);
	 CREATE TABLE s.main.x AS SELECT 1;" \
	"read-only"

run "ATTACH server mode builds one schema per study" \
	"ATTACH '' AS all_studies (TYPE rws, SECRET rave);
	 SELECT string_agg(schema_name, ',' ORDER BY schema_name) FROM duckdb_schemas() WHERE database_name='all_studies';" \
	"ACME & Co__Prod,_rws"

if [ "$failures" -eq 0 ]; then
	echo "all mock protocol tests passed"
else
	echo "$failures mock protocol test(s) failed"
fi
exit $failures
