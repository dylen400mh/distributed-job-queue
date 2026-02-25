# Fix: Job Stuck in ASSIGNED State

## Root Cause

Two bugs prevent jobs from completing:

1. **Missing ASSIGNED→RUNNING transition** (`worker_service_impl.cc`): `ReportResult` checks
   `job->status != "RUNNING"` and rejects with FAILED_PRECONDITION — but the job is still
   `ASSIGNED` because nothing ever calls `TransitionJobStatus("ASSIGNED", "RUNNING", ...)`.
   The worker goes straight from receiving the assignment to reporting the result, skipping
   the RUNNING state entirely. Fix: auto-transition ASSIGNED→RUNNING at the start of
   `ReportResult` when the job is in ASSIGNED state.

2. **`worker_id` not written on ASSIGNED** (`job_repository.cc`): `TransitionJobStatus`
   only updates `status`, not `worker_id` on the `jobs` row. Fix: when `worker_id` is
   non-empty, add it to the UPDATE SET clause so the column is populated on the
   ASSIGNED→RUNNING transition.

## Todo

- [x] 1. `worker_service_impl.cc`: auto-transition ASSIGNED→RUNNING in `ReportResult`
- [x] 2. `job_repository.cc`: update `worker_id` column in `TransitionJobStatus` when non-empty
- [x] 3. Rebuild jq-server and verify job reaches DONE
- [x] 4. Commit, push, and update activity.md

## Review

- `worker_service_impl.cc`: `ReportResult` now handles ASSIGNED→RUNNING→DONE/FAILED in one call; no proto changes needed
- `job_repository.cc`: `worker_id` column on `jobs` table is populated on the ASSIGNED→RUNNING transition, enabling heartbeat recovery and correct display in `jq-ctl job status`
- Verified end-to-end: job submitted via `jq-ctl` reaches `DONE` with `worker_id`, `started_at`, and `completed_at` all correctly set
