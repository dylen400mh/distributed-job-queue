# Prompt 3 — Database Schema & Migrations

## Todo

- [x] 1. Create db/migrations/V1__create_queues_table.sql
- [x] 2. Create db/migrations/V2__create_workers_table.sql
- [x] 3. Create db/migrations/V3__create_jobs_table.sql
- [x] 4. Create db/migrations/V4__create_job_events_table.sql
- [x] 5. Create db/migrations/V5__seed_default_queue.sql
- [x] 6. Create src/common/db/connection_pool.h + connection_pool.cc
- [x] 7. Create src/common/db/repository.h + repository.cc
- [x] 8. Create src/common/db/migrations.h + migrations.cc
- [x] 9. Update CMakeLists.txt — jq_db static library + test target
- [x] 10. Create tests/unit/db/migration_test.cc
- [x] 11. Update tests/CMakeLists.txt
- [x] 12. Verify full build: zero warnings, all targets compile
- [x] 13. Append to docs/activity.md and push to git

## Review

All tasks completed successfully. Database schema and migration infrastructure are in place.

**SQL migrations (db/migrations/):**
- V1: `queues` table + `pgcrypto` extension for `gen_random_uuid()`
- V2: `worker_status` enum + `workers` table + status/heartbeat indexes
- V3: `job_status` enum + `jobs` table + 4 partial indexes (scheduler, heartbeat recovery, queue stats, retry scheduling)
- V4: `job_events` append-only table + composite index on `(job_id, occurred_at)`
- V5: Seeds the `default` queue with `ON CONFLICT DO NOTHING`

**C++ db layer (src/common/db/):**
- `ConnectionPool` — thread-safe FIFO pool of `pqxx::connection`; `BorrowedConnection` RAII handle returns/reconnects on destruction
- `Repository` — base class exposing `Conn()` for subclasses (no raw SQL outside repositories)
- `RunMigrations()` — creates `schema_migrations` tracker table, discovers `V*.sql` files, applies pending migrations in individual transactions

**Tests:** 3 tests in `tests/unit/db/migration_test.cc` — auto-skip if DB unreachable; set `JQ_TEST_DB` env var to override connection string

**Fix:** libpqxx 7.10 deprecated `exec_params` and `exec1`; replaced with `exec(..., pqxx::params{})` and `exec(...).one_row()`
