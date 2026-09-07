# duckdb-rws

A DuckDB extension that reads Medidata Rave Web Services (RWS) as SQL tables —
either through table functions or by attaching a study as a read-only database.

Read-only. Nothing is ever written back to Rave.

## Build

```bash
git submodule update --init --recursive
VCPKG_TOOLCHAIN_PATH=/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake make release
```

Produces `build/release/duckdb` with the extension linked in, and
`build/release/extension/rws/rws.duckdb_extension` for loading into another
DuckDB 1.5.x build.

## Connect

Credentials live in a DuckDB secret, never in the query text:

```sql
CREATE SECRET rave (
    TYPE rws,
    BASE_URL 'https://example.mdsol.com',   -- /RaveWebServices is appended if absent
    USERNAME 'rave-user',
    PASSWORD '...'
);
```

The password is redacted in `duckdb_secrets()`. Secrets are temporary unless you
ask for persistence. `BASE_URL` may not embed credentials, and TLS verification
is on by default.

## Table functions

```sql
SELECT * FROM rws_version();                             -- server version
SELECT * FROM rws_studies();                             -- accessible studies

SELECT * FROM rws_subjects('MYSTUDY', 'Prod');           -- one row per subject
SELECT * FROM rws_sites('MYSTUDY', 'Prod');              -- sites the subjects reference
SELECT * FROM rws_forms('MYSTUDY', 'Prod');              -- forms present, with row/column counts
SELECT * FROM rws_study_events('MYSTUDY', 'Prod');
SELECT * FROM rws_items('MYSTUDY', 'Prod');              -- item catalogue with value counts

SELECT * FROM rws_form('MYSTUDY', 'Prod', 'VITAL');      -- wide table, one row per record
SELECT * FROM rws_form_columns('MYSTUDY', 'Prod', 'VITAL');
SELECT * FROM rws_clinical_items('MYSTUDY', 'Prod');     -- long table, one row per ItemData
```

Named parameters: `secret`, `timeout_seconds`, `max_retries`, `refresh`, and
where they apply `dataset_type` (`regular`/`raw`), `subject_key`, `start`,
`form_oid`, `include`, `status`, `links`, `subject_key_type`.

All values are `VARCHAR`; cast explicitly. An absent source attribute is SQL
`NULL`, an empty `Value=""` stays an empty string, and `is_null` is only true
where the source carried `IsNull="Yes"`.

## ATTACH

```sql
ATTACH 'MYSTUDY/Prod' AS study (TYPE rws, SECRET rave);
USE study;

SHOW TABLES;                    -- master tables plus one table per form
SELECT * FROM subjects;
SELECT subject_key, VSPULSE FROM VS WHERE VSPULSE IS NOT NULL;

SELECT * FROM _rws.settings;    -- the options this attachment is using
SELECT * FROM _rws.tables;      -- table-to-form mapping and column counts
```

Server mode attaches every accessible study, one schema per study/environment:

```sql
ATTACH '' AS rave_all (TYPE rws, SECRET rave, ENVIRONMENTS 'Prod');
SELECT * FROM rave_all."MYSTUDY__Prod".subjects;
```

ATTACH options: `SECRET`, `PROJECT`, `ENVIRONMENT`, `DATASET_TYPE`,
`DEFAULT_SCHEMA`, `SUBJECT_INCLUDE`, `SUBJECT_STATUS`, `SUBJECT_LINKS`,
`SUBJECT_KEY_TYPE`, `STUDIES`, `ENVIRONMENTS`.

The catalog is read-only: `INSERT`, `CREATE TABLE`, `CREATE VIEW`, `DROP`,
`ALTER` and `CREATE SCHEMA` all fail with an explicit error. To keep data, copy
it out: `CREATE TABLE local_vs AS SELECT * FROM study.main.VS;`

## Caching

One study is one HTTP request, and the response is cached in-process so that the
whole catalog — every form table and every derived master table — resolves from
it. `SET rws_cache_seconds = 300` controls the lifetime (`0` disables caching),
`SELECT * FROM rws_clear_cache()` drops everything, and
`SELECT * FROM rws_refresh_catalog('study')` drops just one attachment's
responses. A form that appears or disappears at the source needs `DETACH` +
`ATTACH`, so a bound query never changes shape underneath you.

## Tests

```bash
make test                      # SQL tests that need no network
./test/http/run_mock_tests.sh  # protocol tests against a local mock RWS
```

## Install elsewhere

`docs/INSTALL.md` covers installing the built extension into another DuckDB.
Extensions are version-locked: build against the same DuckDB version you run.

See `docs/DESIGN.md` for the data model, the endpoints this depends on, and the
deployment differences that shape it.
