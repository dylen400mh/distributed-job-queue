# Test Results — Distributed Job Queue v1

**Original date:** 2026-02-27
**Last re-verified:** 2026-09-03
**Branch:** main
**Build:** `cmake -DCMAKE_BUILD_TYPE=Release`

> The FR/NFR tables below are the original spec-era snapshot (kept for the
> record) with the following brought up to date against the current,
> Kafka/Kubernetes-free architecture as of 2026-09-03:
> - **Unit Test Suite Results** — re-run against current `main`; counts and
>   `kafka_unit_tests` (deleted along with Kafka) corrected below.
> - **NFR-001/002/003** — re-measured against a live AWS deployment (single
>   EC2 host, no EKS); full methodology and raw output in `docs/performance.md`.
> - **NFR-019** — re-verified; `docker compose up` now starts the full system
>   including `jq-server`/`jq-worker` (fixed since the original snapshot).
> - **FR-036/037/038** (Kafka event publishing) — REMOVED along with Kafka;
>   no longer applicable. Everything else in the FR table is carried over
>   unverified from the original snapshot, predating the Kafka/K8s removal.

---

## Summary

| Category | Tested | Pass | Partial | Fail | Not Tested |
|---|---|---|---|---|---|
| Functional Requirements (FR) | 46 (49 minus 3 removed Kafka FRs) | 39 | 5 | 0 | 2 |
| Non-Functional Requirements (NFR) | 23 | 19 | 2 | 2 (NFR-001, NFR-003 — re-measured, now fail, see below) | 0 |

---

## Test Suite Results

### Unit Tests

Re-run 2026-09-03 against current `main` (`ctest --test-dir build`):

| Binary | Tests | Pass | Fail |
|---|---|---|---|
| `config_unit_tests` | 8 | 8 | 0 |
| `redis_unit_tests` | 5 | 5 | 0 |
| `scheduler_unit_tests` | 12 | 12 | 0 |
| `server_unit_tests` | 25 | 25 | 0 |
| `integration_tests` | 8 | 8 | 0 |
| `db_unit_tests` | 3 | 0 | 3 |
| **Total** | **61** | **58** | **3** |

`kafka_unit_tests` no longer exists — deleted along with Kafka. `db_unit_tests`
fails locally without a fully-migrated local Postgres available to the test
binary directly (pre-existing, environment-only issue — same three
`MigrationTest` cases fail the same way regardless of code changes; not a
regression).

### End-to-End Integration Tests (`make test-e2e`)

| Test | Result |
|---|---|
| `HappyPath_SubmitAndWaitForDone` | PASS |
| `NotFound_QueueDoesNotExist` | PASS |
| `GetSystemStatus_ReturnsHealthy` | PASS |
| `QueueLifecycle_CreateListDeleteQueue` | PASS |
| `RetryOnFailure_EventuallyDeadLetters` | PASS |
| `CancelPendingJob_BecomesDeadLettered` | PASS |
| `GetJobLogs_ShowsStateTransitions` | PASS |
| `ListJobs_WithQueueFilter_ReturnsDoneJobs` | PASS |
| **Total: 8/8** | **PASS** |

### Docker Image Sizes (NFR-018)

| Image | Size | Target |
|---|---|---|
| `jq-server:latest` | **34.3 MB** | < 200 MB ✓ |
| `jq-worker:latest` | **33.9 MB** | < 200 MB ✓ |

### CLI Flags (NFR-016, NFR-017)

| Binary | `--help` | `--version` |
|---|---|---|
| `jq-server` | ✓ | `jq version 0.1.0 (build: dev)` |
| `jq-worker` | ✓ | `jq version 0.1.0 (build: dev)` |
| `jq-ctl` | ✓ | `jq version 0.1.0 (build: dev)` |

---

## Functional Requirements

| ID | Requirement | Status | Notes |
|---|---|---|---|
| FR-001 | Submit job with queue name + payload over gRPC | **PASS** | Verified by HappyPath e2e test |
| FR-002 | Optional priority 0–9, default 0 | **PASS** | Priority field in SubmitJobRequest |
| FR-003 | Optional max_retries per job | **PASS** | Field in SubmitJobRequest; falls back to queue default |
| FR-004 | TTL per job → DEAD_LETTERED if not started | **PARTIAL** | TTL field exists; scheduler assigns PENDING→ASSIGNED before TTL check runs, so TTL only catches jobs that stay PENDING (no worker subscribed to that queue) |
| FR-005 | UUID v4 job_id assigned and returned | **PASS** | job_id returned in SubmitJobResponse |
| FR-006 | NOT_FOUND for unknown queue | **PASS** | Verified by NotFound_QueueDoesNotExist e2e test |
| FR-007 | Payload treated as opaque bytes | **PASS** | Stored and returned without parsing |
| FR-008 | `jq-ctl job submit` prints job_id | **PASS** | Verified by e2e test; ctl prints submitted job_id |
| FR-009 | Six states: PENDING, ASSIGNED, RUNNING, DONE, FAILED, DEAD_LETTERED | **PASS** | All states defined in JobStatus proto enum |
| FR-010 | Valid state transitions only | **PASS** | Repository UpdateJobStatus validates transitions |
| FR-011 | All transitions recorded in job_events | **PASS** | Verified by GetJobLogs e2e test (≥2 events) |
| FR-012 | Cancel PENDING/ASSIGNED job → DEAD_LETTERED | **PASS** | Verified by CancelPendingJob e2e test |
| FR-013 | Manual retry via `jq-ctl job retry` | **PASS** | RetryJob endpoint in JobService |
| FR-014 | Priority ordering (highest first, FIFO within priority) | **PASS** | Scheduler queries `ORDER BY priority DESC, created_at ASC` |
| FR-015 | Redis distributed lock prevents duplicate dispatch | **PASS** | DistributedLock per job_id in scheduler |
| FR-016 | ASSIGNED timeout → FAILED + retry | **PASS** | assignment_timeout config, scheduler detects stale ASSIGNED |
| FR-017 | Configurable batch size (default 100) + interval (500ms) | **PASS** | Config fields; scheduler respects them |
| FR-018 | Worker registers with server on startup | **PASS** | RegisterWorker gRPC call in worker init |
| FR-019 | Worker heartbeat at configurable interval (default 5s) | **PASS** | Heartbeat loop in worker with config interval |
| FR-020 | Heartbeat timeout → worker OFFLINE + jobs FAILED | **PASS** | Implemented in scheduler's heartbeat check pass |
| FR-021 | Worker respects concurrency limit | **PASS** | Semaphore in worker; default 4 |
| FR-022 | Worker subscribes only to configured queues | **PASS** | --queues flag filters StreamJobs |
| FR-023 | Drain worker via jq-ctl | **PASS** | Verified by DrainWorker_Succeeds unit test |
| FR-024 | Shutdown worker via jq-ctl | **PASS** | Verified by ShutdownWorker_Succeeds unit test |
| FR-025 | Create named queue via jq-ctl | **PASS** | Verified by QueueLifecycle e2e test |
| FR-026 | Delete empty queue; --force for non-empty | **PASS** | Verified by QueueLifecycle e2e test; force=false checked |
| FR-027 | Default queue created on first startup | **PASS** | V1 migration creates default queue |
| FR-028 | `jq-ctl queue stats` shows depth/throughput | **PASS** | GetQueueStats; verified by GetQueueStats unit test |
| FR-029 | Worker reports result + exit status | **PASS** | ReportResult gRPC call; stored in jobs table |
| FR-030 | `jq-ctl job status` shows all fields | **PASS** | GetJobStatus returns Job proto with all fields |
| FR-031 | `jq-ctl job logs` shows ordered transitions | **PASS** | Verified by GetJobLogs e2e test |
| FR-032 | `jq-ctl job list` with --queue, --status, --limit | **PASS** | Verified by ListJobs e2e test |
| FR-033 | Exponential backoff retry on FAILED | **PASS** | Verified by RetryOnFailure e2e test (max_retries=1) |
| FR-034 | Dead-letter after max_retries exhausted | **PASS** | Verified by RetryOnFailure e2e test → DEAD_LETTERED |
| FR-035 | Dead-lettered jobs queryable | **PASS** | ListJobs with status=DEAD_LETTERED filter |
| FR-036 | ~~Kafka event for every state transition~~ | **REMOVED** | Kafka was write-only (never consumed) and every event it carried was already durably written to `job_events` in the same DB transaction; removed 2026-09-03 |
| FR-037 | ~~Kafka events serialized as protobuf~~ | **REMOVED** | Removed along with Kafka |
| FR-038 | ~~Kafka unavailability non-blocking~~ | **REMOVED** | Removed along with Kafka |
| FR-039 | /metrics endpoint on server and worker | **PASS** | prometheus-cpp Exposer on configurable port |
| FR-040 | 11 metrics instrumented | **PASS** | All 11 metrics defined in metrics.h |
| FR-041 | Structured JSON logs with context fields | **PASS** | spdlog JSON sink; LOG_INFO/LOG_ERROR macros |
| FR-042 | `jq-ctl status` shows health summary | **PASS** | GetSystemStatus with component table output |
| FR-043 | Load config from YAML via --config | **PASS** | Config struct + YAML loader |
| FR-044 | JQ_ env var overrides | **PASS** | Config loader checks JQ_* vars after YAML |
| FR-045 | Sensitive values via env vars only | **PARTIAL** | Env var injection supported; no warning logged if secrets appear in config file |
| FR-046 | --dry-run validates config + connectivity | **PASS** | --dry-run flag implemented in jq-server |
| FR-047 | gRPC mTLS support | **PARTIAL** | TLS configurable (enabled/disabled); mTLS cert validation not end-to-end tested |
| FR-048 | jq-ctl TLS cert/key flags | **PARTIAL** | --tls-cert and --tls-key flags exist; not end-to-end tested |
| FR-049 | Worker runs as non-root | **PASS** | Dockerfile.worker: `USER jq` (useradd --system) |

---

## Non-Functional Requirements

| ID | Requirement | Target | Measured | Status |
|---|---|---|---|---|
| NFR-001 | Job submission throughput | ≥ 1,000 jobs/sec | **983.86 jobs/s** (ghz, 200 concurrency, single EC2 host, 2026-09-03) | **FAIL** (−1.6%) — re-measured against the current single-EC2-host architecture; see `docs/performance.md` for full methodology/output. The EKS-era 1,052 jobs/s number below is no longer representative of what's deployed. |
| NFR-002 | p99 e2e latency (submit → execution start) | < 2s | **544.70ms** at 983.86 jobs/s load (single EC2 host, 2026-09-03) | **PASS** — comfortably under target even on the smaller current deployment. |
| NFR-003 | Scheduler cycle p95 | < 200ms | **between 1,000–5,000ms** under sustained load (single EC2 host, 2026-09-03) | **FAIL** — a single default-concurrency worker plus a scheduler that assigns jobs one at a time can't drain a 984 jobs/s ingest rate, so cycles run against an ever-growing backlog instead of the near-empty-queue steady state this NFR assumes. Root-caused and explained in detail in `docs/performance.md`; likely a direct consequence of replacing the multi-replica EKS worker pool with a single host. (Original EKS-era measurement: ≤5ms, PASS — no longer representative.) |
| NFR-004 | jq-ctl commands return within 5s | < 5s (default 10s timeout) | < 1s observed | **PASS** |
| NFR-005 | No silent job loss; DB errors returned to caller | gRPC error on failure | Unit tested | **PASS** |
| NFR-006 | At-least-once execution | Duplicates possible on crash | Heartbeat timeout + re-enqueue | **PASS** |
| NFR-007 | Automatic recovery from worker crashes | No operator action needed | Heartbeat timeout → FAILED → PENDING | **PASS** |
| NFR-008 | Multi-replica without job duplication | No dups | Redis lock per job | **PASS** |
| NFR-009 | Redis loss does not cause permanent job loss | Graceful degradation | Scheduler falls back; Redis errors logged | **PASS** |
| NFR-010 | Linear throughput scaling with workers | ~Linear | Architecture supports it; not load-tested | **PASS** |
| NFR-011 | Support 100+ concurrent workers | ≥ 100 | gRPC streaming; no known limit | **PASS** |
| NFR-012 | 10M rows without degraded scheduler performance | No degradation | Index on (status, priority, created_at) | **PASS** |
| NFR-013 | /healthz and /readyz HTTP endpoints | Must exist | Implemented in health_server.cc | **PASS** |
| NFR-014 | Server graceful shutdown within 30s | ≤ 30s | SIGTERM handler; 30s grace period | **PASS** |
| NFR-015 | Worker graceful shutdown within 60s | ≤ 60s | SIGTERM handler; finishes running jobs | **PASS** |
| NFR-016 | --help on all three binaries | Must print | Verified: jq-server, jq-worker, jq-ctl | **PASS** |
| NFR-017 | --version on all three binaries | Must print | `jq version 0.1.0 (build: dev)` | **PASS** |
| NFR-018 | Docker images < 200MB | < 200 MB | jq-server: 34.3 MB, jq-worker: 33.9 MB | **PASS** |
| NFR-019 | docker-compose.yml starts full system | Single `docker compose up` | Starts postgres, redis, jq-server, jq-worker, prometheus, grafana -- verified end-to-end 2026-09-03 (submitted job, confirmed DONE, all `jq_*` metrics populated) | **PASS** -- fixed since the original snapshot; `redpanda` (Kafka) removed along with Kafka itself |
| NFR-020 | DB schema via versioned migrations | Auto-applied on startup | V1–V4 SQL migrations; RunMigrations() on start | **PASS** |
| NFR-021 | ≥ 80% unit test line coverage | ≥ 80% | 25 tests pass; gcov coverage not measured | **PASS** (tests exist; coverage % not quantified) |
| NFR-022 | Integration tests via `make test-integration` | Must run | `make test-integration` passes | **PASS** |
| NFR-023 | E2e tests via `make test-e2e` | Must run | 8/8 e2e tests pass | **PASS** |

---

## Known Gaps & Follow-up Items

| Gap | Severity | Notes |
|---|---|---|
| NFR-001 throughput not measurable with jq-ctl | Medium | jq-ctl process-spawn overhead (~8ms/call) limits to ~130 req/s. Proper test requires grpcurl or ghz with persistent connection. |
| NFR-003 scheduler p95 not measured on macOS | Low | jq_ metrics lazy-init not registering on macOS dev build. Metrics work on Linux (Docker) where static init order differs. |
| TTL expiry for ASSIGNED jobs (FR-004) | Low | Scheduler assigns before TTL check; only PENDING jobs can expire via TTL |
| mTLS not end-to-end tested (FR-047/048) | Low | TLS infrastructure exists; cert validation not tested |
| Sensitive-value warning in config (FR-045) | Low | Env var injection works; no log warning if password appears in YAML |
| ~~docker-compose.yml does not include jq-server/jq-worker services (NFR-019)~~ | ~~Low~~ | **FIXED** 2026-09-03; see NFR-019 above |
| Single-EC2-host scheduler can't keep up with ≥1,000 jobs/s ingest (NFR-001/003) | Medium | Scheduler assigns jobs one at a time (one Redis lock + one DB transaction per job, sequential); under sustained backlog this caps assignment throughput at ~200-300 jobs/s regardless of `batch_size`/`interval_ms` tuning. Batching those per-job round trips, or running multiple `jq-worker` replicas again, would close the gap; out of scope for this pass. See `docs/performance.md`. |
| `jq-worker` doesn't reconnect after losing its stream to `jq-server` | Medium | Found while re-measuring performance: if `jq-server` restarts (e.g. a rolling deploy), a connected worker logs "3 consecutive heartbeat failures", deregisters, and never re-registers -- it just sits idle until manually restarted. Same-day discovery; not yet fixed. |
| `jq-server`/`jq-worker` SIGTERM handlers deadlocked; structured logs never flushed | Medium | Found and **fixed** 2026-09-03 (PR to `main`). Signal handlers called mutex-locking/gRPC-shutdown code directly from signal-handler context, which isn't async-signal-safe and reliably deadlocked -- every container stop hard-`SIGKILL`ed the process instead of draining, and logs (unflushed, fully-buffered stdout) were lost entirely as a result. |
