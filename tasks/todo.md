# Finish Prometheus/Grafana metrics instrumentation

## Goal
Of the 10 metrics declared in `src/common/metrics/metrics.h` (FR-040), only 3 are
actually recorded anywhere in the code (`SchedulerCycleDuration`,
`SchedulerJobsAssignedTotal`, `RedisOperationDuration`). The other 7 have live
Grafana panels and — for 4 of them — active alert rules, all of which are
currently dead: the panels show "No data" forever and the alerts (including
`NoWorkersOnline`, the most operationally important one) can never fire. Wire
up the remaining 7.

**Branch note:** PR #1 (Kafka + Kubernetes/EKS removal) is now merged into
`main`, so this branch — cut fresh from `main` after that merge — has neither.
No workaround needed for Kafka lines anymore.

## Plan

### 1. `jq_worker_active_count` + `jq_worker_job_concurrency` — event-driven, in `WorkerRegistry`
In-memory state, no DB cost, so update on every mutation rather than polling.
- `src/server/scheduler/worker_registry.cc`: add a private `PublishMetrics()` (called
  while holding `mu_`) that sets `WorkerActiveCount` = `workers_.size()` and
  `WorkerJobConcurrency{worker_id}` = each worker's `active_job_count`.
- Call it at the end of `RegisterStream`, `RemoveWorker`, `DecrementActiveCount`,
  and all three mutating exits of `AssignJob`.
- On `RemoveWorker`, explicitly `Remove()` that worker's `WorkerJobConcurrency`
  series (fetch-then-remove) so it doesn't linger as a stale "ghost" gauge —
  this is the one piece of real polish beyond "just call Set()".

### 2. `jq_job_processing_duration_seconds` — single call site
- `src/server/grpc/worker_service_impl.cc::ReportResult`: after `StoreJobResult`
  succeeds (both success and failure branches), if `job->started_at` is set,
  observe `now - started_at` into `JobProcessingDuration{queue}`.

### 3. `jq_job_total{queue,status}` — increment at every real transition to a terminal-for-the-attempt status
Matches exactly what the dashboard/alerts already filter for (`DONE|FAILED|DEAD_LETTERED`).
- `WorkerServiceImpl::ReportResult` — `DONE` on success; `FAILED` on every failure;
  `DEAD_LETTERED` on the exhausted-retries branch
- `Scheduler::ApplyRetry` (`scheduler.cc`) — `DEAD_LETTERED` branch. Needs `queue_name`
  threaded in as a new parameter (its two callers — `HeartbeatMonitor` and
  `WorkerServiceImpl::ReportResult`'s own inline dup logic — both already have a
  `JobRow` with `.queue_name` in scope)
- `Scheduler::RunLoop`'s TTL-expiry block — `DEAD_LETTERED`
- `JobServiceImpl::CancelJob` — `DEAD_LETTERED` (reason `CANCELLED`)

### 4. `jq_job_queue_depth{queue,status}` — periodic snapshot, folded into the existing 10s `HeartbeatMonitor` loop
No new thread, no new SQL — `QueueRepository::GetQueueStats()` already computes
exactly this (`pending_count`, `running_count`, `failed_count`, `done_count`,
`dead_letter_count`) for `jq-ctl queue stats`.
- `src/server/scheduler/scheduler.cc::HeartbeatMonitor`: add a local
  `db::QueueRepository queue_repo(pool_);`, each cycle iterate
  `queue_repo.ListQueues()` and `Set()` the gauge for all 5 statuses per queue.
- **Known limitation, accepted rather than engineered around:** if a queue is
  deleted, its gauge series goes stale rather than disappearing. Queue deletion
  is a rare, explicit operator action — not worth the extra plumbing into
  `AdminServiceImpl::DeleteQueue` to clean it up.

### 5. `jq_grpc_request_duration_seconds{method,status_code}` — new gRPC server interceptor
The one genuinely new piece — nothing in the codebase does this yet.
- New `src/server/grpc/metrics_interceptor.h/.cc`: a
  `grpc::experimental::Interceptor` + `ServerInterceptorFactoryInterface` pair.
  Captures the method name from `experimental::ServerRpcInfo::method()` at
  construction, times from `PRE_SEND_INITIAL_METADATA` to `PRE_SEND_STATUS`,
  reads the status code off `GetSendStatus()`.
- `src/server/grpc/server.cc::Start()`: register it via
  `builder.experimental().SetInterceptorCreators(...)` before `BuildAndStart()`.

### 6. `jq_db_query_duration_seconds{query_name}` — RAII timer across all repository methods
Mirrors the `OpTimer` pattern already used in `redis_client.cc`, generalized so
it's not duplicated three times.
- `src/common/metrics/metrics.h`: add a small reusable `ScopedDuration` RAII
  (histogram ref + start time, observes on destruction).
- One-line instantiation at the top of all 19 repository methods across
  `job_repository.cc` (9), `worker_repository.cc` (6), `queue_repository.cc` (4),
  each labeled with its own method name as `query_name`.

## Verification
- [x] Full build (`cmake --build build --parallel`)
- [x] `ctest` — same baseline as before (only `db_unit_tests` fails, no local Postgres)
- [x] Manually sanity-checked: started Postgres/Redis via compose, ran `jq-server` +
      `jq-worker` natively, submitted a success job, a job that fails and retries,
      and a job that gets cancelled mid-flight. Curled `/metrics` on both binaries —
      all 10 declared families now appear on `jq-server:9090` with real values;
      `jq-worker:9091` correctly has none of them (all FR-040 metrics are
      server-side by design).
- [x] `docker compose config` — clean

## Ship
- [x] Branch `finish-metrics-instrumentation` off `main` (post PR #1 merge)
- [x] Commit(s), activity log entry
- [ ] Push, open PR to `main`

## Review

All 7 unwired metrics are now recorded, verified live against a real
Postgres+Redis+jq-server+jq-worker run (not just build-green):

- `jq_worker_active_count` / `jq_worker_job_concurrency` — event-driven from
  `WorkerRegistry`'s existing mutation points; `RemoveWorker` explicitly drops
  the departed worker's concurrency series instead of leaving a stale gauge.
- `jq_job_total{queue,status}` — increments at all 4 real call sites
  (`ReportResult` DONE/FAILED/DEAD_LETTERED, `ApplyRetry`'s dead-letter branch,
  TTL expiry, `CancelJob`). Confirmed all three statuses appear correctly via
  a success run, a failing job, and a cancel.
- `jq_job_queue_depth{queue,status}` — piggybacks on the existing 10s
  `HeartbeatMonitor` loop, reusing `QueueRepository::GetQueueStats()` (already
  built for `jq-ctl queue stats`) rather than new SQL.
- `jq_grpc_request_duration_seconds{method,status_code}` — new
  `MetricsInterceptor`/`MetricsInterceptorFactory` pair registered via
  `builder.experimental().SetInterceptorCreators()`. First real interceptor
  usage in this codebase.
- `jq_db_query_duration_seconds{query_name}` — new `metrics::ScopedDuration`
  RAII + `DbQueryTimer()` helper, put to use with 23 one-line insertions across
  all three repositories (finally using the `kDurationBuckets` constant that
  was declared but dead in the original scaffolding).
- `jq_job_processing_duration_seconds{queue}` — **caught and fixed a real bug
  during manual testing, not just wiring it up.** The plan's original approach
  (observe from `job->started_at` fetched before the RUNNING transition) never
  fires: there's no separate "job started" RPC in this system, so every job is
  *always* auto-advanced ASSIGNED→RUNNING inside `ReportResult` itself, meaning
  the pre-fetched `job` row's `started_at` is *always* 0 at observation time.
  Fixed by tracking the actual started-at moment locally (the DB write we just
  did) instead of relying on the stale in-memory row. Confirmed non-zero
  histogram counts after the fix.

**Known, accepted limitations** (stated in-code, not silent):
- `jq_job_queue_depth` goes stale for a deleted queue rather than disappearing
  — queue deletion is rare and explicit; not worth touching `DeleteQueue` for.
- `started_at`/`completed_at` have whole-second granularity (existing schema),
  so sub-second jobs report ~0 duration — best available precision without a
  schema change, out of scope here.
- Observed but **not fixed** (pre-existing, unrelated to metrics): cancelling
  a job already dispatched to a worker updates the DB but doesn't tell
  `WorkerRegistry` to decrement that worker's active count — `jq_worker_job_concurrency`
  faithfully mirrors this existing staleness rather than papering over it.

**Files touched:** 13 modified + 2 new (`metrics_interceptor.h/.cc`), 1 commit.
