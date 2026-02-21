-- V3: Create the job_status enum and jobs table.
-- Jobs are the core unit of work in the system.

CREATE TYPE job_status AS ENUM (
    'PENDING',
    'ASSIGNED',
    'RUNNING',
    'DONE',
    'FAILED',
    'DEAD_LETTERED'
);

CREATE TABLE jobs (
    job_id        UUID        NOT NULL DEFAULT gen_random_uuid(),
    queue_name    TEXT        NOT NULL,
    payload       BYTEA       NOT NULL,
    status        job_status  NOT NULL DEFAULT 'PENDING',
    priority      SMALLINT    NOT NULL DEFAULT 0,
    max_retries   SMALLINT    NOT NULL DEFAULT 3,
    retry_count   SMALLINT    NOT NULL DEFAULT 0,
    worker_id     UUID,
    result        BYTEA,
    error_message TEXT,
    not_before    TIMESTAMPTZ,
    created_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    started_at    TIMESTAMPTZ,
    completed_at  TIMESTAMPTZ,
    updated_at    TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT jobs_pkey PRIMARY KEY (job_id),
    CONSTRAINT jobs_queue_fkey FOREIGN KEY (queue_name) REFERENCES queues (name),
    CONSTRAINT jobs_worker_fkey FOREIGN KEY (worker_id) REFERENCES workers (worker_id),
    CONSTRAINT jobs_priority_check CHECK (priority BETWEEN 0 AND 9),
    CONSTRAINT jobs_retry_count_check CHECK (retry_count >= 0),
    CONSTRAINT jobs_max_retries_check CHECK (max_retries >= 0)
);

-- Scheduler query: fetch PENDING jobs ordered by priority DESC, created_at ASC
CREATE INDEX jobs_scheduler_idx ON jobs (status, priority DESC, created_at ASC)
    WHERE status = 'PENDING';

-- Heartbeat timeout recovery: find ASSIGNED/RUNNING jobs for a stale worker
CREATE INDEX jobs_worker_idx ON jobs (worker_id)
    WHERE worker_id IS NOT NULL;

-- Queue stats: count jobs per queue per status
CREATE INDEX jobs_queue_status_idx ON jobs (queue_name, status);

-- Retry scheduling: find PENDING jobs whose not_before has passed
CREATE INDEX jobs_retry_idx ON jobs (status, not_before)
    WHERE status = 'PENDING' AND not_before IS NOT NULL;
