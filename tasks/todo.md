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

---

# Prompt 7 — Scheduler Loop, WorkerService & WorkerRegistry

## Notes

- `RedisClient` is NOT thread-safe — each thread creates its own instance from `RedisConfig`
- `grpc::ServerWriter<>` lifetime is tied to the `StreamJobs` RPC handler frame; use `shared_ptr<StreamHandle>` with an `active` flag + mutex to prevent dangling writes
- `workers.worker_id` is UUID — use `gen_random_uuid()` server-side if client sends empty/non-UUID
- TTL is per-queue in `queues.ttl_seconds`; check expiry by joining `jobs` with `queues`
- Retry backoff: `min(base_delay_s * 2^retry_count, max_delay_s) + rand_jitter`
- Keep `IJobRepository` extended (not a separate interface) to avoid extra indirection

## Todo

- [x] 1. Extend `IJobRepository` + `JobRepository` with 5 new methods: `FetchPendingBatch`, `SetJobRetry`, `FetchExpiredTtlJobs`, `FetchJobsForWorker`, `StoreJobResult`
- [x] 2. Create `src/server/db/worker_repository.h/.cc` — `WorkerRow`, `IWorkerRepository`, `WorkerRepository`
- [x] 3. Create `src/server/scheduler/worker_registry.h/.cc` — `StreamHandle`, `WorkerInfo`, `WorkerRegistry` (AssignJob, RegisterStream, RemoveWorker, DecrementActiveCount, GetWorkerStats)
- [x] 4. Create `src/server/scheduler/scheduler.h/.cc` — `Scheduler` with scheduling loop thread + heartbeat monitor thread, retry logic, TTL expiry, metrics
- [x] 5. Replace `src/server/grpc/worker_service_impl.h` with proper class header; create `worker_service_impl.cc` — all 5 WorkerService RPCs
- [x] 6. Update `src/server/grpc/server.h/.cc` — add `WorkerRegistry` + `Scheduler` ownership; start/stop scheduler in `Start()`/`Stop()`
- [x] 7. Update `CMakeLists.txt` — add `jq_server_scheduler` lib; add `worker_repository.cc` + `worker_service_impl.cc`; updated `jq_server_grpc` deps
- [x] 8. Update `tests/CMakeLists.txt` — add `scheduler_unit_tests` executable
- [x] 9. Update `tests/unit/server/job_service_test.cc` — add `MOCK_METHOD` entries for the 5 new `IJobRepository` methods
- [x] 10. Create `tests/unit/server/scheduler_test.cc` — unit tests: backoff formula, WorkerRegistry::AssignJob selection logic, heartbeat timeout detection
- [x] 11. Verify full build and all unit tests pass (12 new tests; 44/44 non-DB tests pass)
- [x] 12. Append to docs/activity.md and push to git

## Review

### Summary of Changes

**Extended `IJobRepository` / `JobRepository` (5 new methods):**
- `FetchPendingBatch(batch_size)` — query `WHERE status='PENDING' AND (not_before IS NULL OR not_before <= now())` ordered by priority DESC, created_at ASC
- `SetJobRetry(job_id, new_retry_count, not_before_epoch)` — resets to PENDING with backoff timestamp via `to_timestamp(epoch)`
- `FetchExpiredTtlJobs()` — JOINs jobs with queues on `ttl_seconds`; finds PENDING jobs whose age exceeds queue TTL
- `FetchJobsForWorker(worker_id)` — fetches ASSIGNED/RUNNING jobs for a given worker (used on heartbeat timeout)
- `StoreJobResult(job_id, success, result, error_message, worker_id)` — RUNNING → DONE or FAILED transition with result storage

**New library — `jq_server_db` (extended):**
- `src/server/db/worker_repository.h/.cc` — `WorkerRow` struct; `IWorkerRepository` interface; `WorkerRepository`: `UpsertWorker` (INSERT ... ON CONFLICT DO UPDATE), `FindWorkerById`, `UpdateWorkerHeartbeat`, `SetWorkerStatus`, `FetchStaleWorkers` (ONLINE workers with last_heartbeat older than timeout)

**New library — `jq_server_scheduler`:**
- `src/server/scheduler/worker_registry.h/.cc` — `StreamHandle` (`std::function<bool(const JobAssignment&)>` write callback + mutex + active flag); `WorkerInfo`; `WorkerRegistry`: thread-safe assignment with per-worker concurrency limit, active_job_count tracking, rollback on write failure
  - Uses `std::function` callback instead of raw `grpc::ServerWriter*` because `ServerWriter` is marked `final` (cannot subclass for tests)
- `src/server/scheduler/scheduler.h/.cc` — Two threads: `RunLoop()` (500ms scheduling cycle: fetch PENDING → Redis lock → PENDING→ASSIGNED → Kafka event → stream to worker; plus TTL expiry) and `HeartbeatMonitor()` (10s cycle: detect stale workers, mark OFFLINE, apply retry/dead-letter)
  - `CalculateRetryNotBefore()` exposed as free function for unit testing
  - Each thread creates its own `RedisClient` (not thread-safe)

**Updated `jq_server_grpc`:**
- `src/server/grpc/worker_service_impl.h/.cc` — all 5 WorkerService RPCs: `RegisterWorker` (UpsertWorker), `Heartbeat` (UpdateWorkerHeartbeat), `StreamJobs` (RegisterStream with lambda wrapper, polls handle->active until cancel), `ReportResult` (StoreJobResult + retry logic + Kafka event), `Deregister` (SetWorkerStatus OFFLINE + RemoveWorker)
- `src/server/grpc/server.h/.cc` — owns `WorkerRepository`, `WorkerRegistry`, `Scheduler`; `Start()` launches scheduler after gRPC; `Stop()` stops scheduler first

**Toolchain fix:**
- `cmake/toolchain-macos.cmake` — added `CMAKE_EXE_LINKER_FLAGS` pointing to `/opt/homebrew/opt/llvm/lib/c++` so `__hash_memory` (Homebrew LLVM libc++ ABI symbol used by `std::unordered_map<std::string, ...>`) resolves correctly

**Tests — 44/44 non-DB tests pass:**
- 12 new `scheduler_unit_tests`: 5 `BackoffTest` (formula verification) + 7 `WorkerRegistryTest` (no workers, wrong queue, matching worker, concurrency limit, decrement allows next, remove prevents assign, write failure rollback)
- All prior tests unchanged: 15 server, 5 redis, 4 kafka, 8 config = 32 pre-existing
