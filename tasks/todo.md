# Prompt 6 — jq-server gRPC Server & JobService

## Notes

- DB schema: jobs (UUID PK, BYTEA payload, job_status enum, SMALLINT priority, etc.), job_events (BIGSERIAL, from_status nullable), queues (TEXT PK)
- Health server: plain POSIX TCP sockets (Boost.Asio not needed for this minimal case)
- Kafka payload serialization: serialize proto `JobEvent` message as bytes (FR-037)
- `pqxx::bytes` is `std::basic_string<std::byte>` — requires explicit cast to/from uint8_t
- Build SQL with `txn.quote()` (not `exec_params`) to avoid pqxx 7.10 deprecation warnings

## Todo

- [x] 1. Add `IKafkaProducer` interface to `src/common/kafka/kafka_producer.h`; make `KafkaProducer` implement it
- [x] 2. Create `src/server/db/job_repository.h` — `JobRow`, `JobEventRow`, `IJobRepository` interface
- [x] 3. Create `src/server/db/job_repository.cc` — concrete `JobRepository` with all DB operations
- [x] 4. Create `src/server/grpc/job_service_impl.h` and `job_service_impl.cc` — all 6 RPCs
- [x] 5. Create `src/server/grpc/worker_service_impl.h` — UNIMPLEMENTED stubs
- [x] 6. Create `src/server/grpc/admin_service_impl.h` — UNIMPLEMENTED stubs
- [x] 7. Create `src/server/grpc/server.h` and `server.cc` — `GrpcServer` with Start/Stop
- [x] 8. Create `src/server/health/health_server.h` and `health_server.cc` — POSIX TCP; /healthz + /readyz
- [x] 9. Replace `src/server/main.cc` — full startup: flags, config, logger, metrics, health, migrations, signal handlers, gRPC server
- [x] 10. Create `tests/unit/server/job_service_test.cc` — 15 RPC tests with mock repo + mock producer
- [x] 11. Update `CMakeLists.txt` — add `jq_server_db`, `jq_server_grpc`, `jq_server_health` libs; test target
- [x] 12. Verify full build and all unit tests pass
- [x] 13. Append to docs/activity.md and push to git

## Review

### Summary of Changes

**New library — `jq_server_db`:**
- `JobRow`, `JobEventRow` structs mirroring DB columns; timestamps as Unix epoch seconds
- `IJobRepository` interface for mock injection in unit tests
- `JobRepository` (concrete): `QueueExists`, `GetQueueMaxRetries`, `InsertJob` (job + initial event in one transaction), `FindJobById`, `TransitionJobStatus` (update + event in one transaction, handles `started_at`/`completed_at` per status), `ResetJobForRetry`, `ListJobs`, `ListJobEvents`
- Uses `txn.quote()` throughout to avoid pqxx 7.10 deprecation warnings; handles `pqxx::bytes` / `std::byte` correctly

**New library — `jq_server_grpc`:**
- `JobServiceImpl` — all 6 JobService RPCs with correct gRPC status codes per design-notes.md conventions:
  - `SubmitJob`: checks queue exists (NOT_FOUND), inserts job + event, publishes `job.submitted`
  - `CancelJob`: FAILED_PRECONDITION if not PENDING/ASSIGNED; transitions to DEAD_LETTERED with reason CANCELLED
  - `GetJobStatus`: maps DB row → proto Job
  - `ListJobs`: `limit+1` trick for pagination; `next_page_token` is a base-10 offset string
  - `GetJobLogs`: returns ordered `job_events` rows as proto `JobEvent` messages
  - `RetryJob`: FAILED_PRECONDITION if not FAILED/DEAD_LETTERED; calls `ResetJobForRetry`
- `WorkerServiceImpl`, `AdminServiceImpl` — header-only UNIMPLEMENTED stubs
- `GrpcServer` — registers all three services; `Start()` blocks; `Stop()` drains with 30s deadline
- `IKafkaProducer` interface added to `kafka_producer.h`; `KafkaProducer` implements it

**New library — `jq_server_health`:**
- POSIX TCP accept loop in a background thread
- `GET /healthz` — always 200 OK
- `GET /readyz` — 200 if DB `SELECT 1` succeeds and `RedisClient::IsConnected()` is true; 503 otherwise

**Updated `src/server/main.cc`:**
- Full startup sequence: parse flags → load/validate config → init logger → start metrics → build DB pool + Kafka producer + Redis client → dry-run connectivity test (FR-046) → run migrations → start health server → register signal handlers → start gRPC server (blocking) → graceful shutdown

**Tests — 32/32 pass (no external services required):**
- 15 `JobServiceTest` tests: SubmitJob (3), CancelJob (3), GetJobStatus (2), ListJobs (2), GetJobLogs (1), RetryJob (4) — all via `MockJobRepository` + `MockKafkaProducer`
- 5 `DistributedLockTest`, 4 `KafkaDeliveryReportTest`, 8 `ConfigTest`

**Build:** Full clean build, zero errors.
