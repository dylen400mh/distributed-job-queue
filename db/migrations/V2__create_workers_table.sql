-- V2: Create the worker_status enum and workers table.
-- Workers are long-running daemon processes that execute jobs.

CREATE TYPE worker_status AS ENUM ('ONLINE', 'OFFLINE', 'DRAINING');

CREATE TABLE workers (
    worker_id       UUID          NOT NULL DEFAULT gen_random_uuid(),
    hostname        TEXT          NOT NULL,
    status          worker_status NOT NULL DEFAULT 'ONLINE',
    concurrency     SMALLINT      NOT NULL DEFAULT 4,
    active_job_count SMALLINT     NOT NULL DEFAULT 0,
    last_heartbeat  TIMESTAMPTZ   NOT NULL DEFAULT now(),
    registered_at   TIMESTAMPTZ   NOT NULL DEFAULT now(),
    updated_at      TIMESTAMPTZ   NOT NULL DEFAULT now(),
    CONSTRAINT workers_pkey PRIMARY KEY (worker_id),
    CONSTRAINT workers_concurrency_check CHECK (concurrency > 0),
    CONSTRAINT workers_active_job_count_check CHECK (active_job_count >= 0)
);

CREATE INDEX workers_status_idx ON workers (status);
CREATE INDEX workers_last_heartbeat_idx ON workers (last_heartbeat);
