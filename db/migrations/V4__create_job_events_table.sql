-- V4: Create the job_events table.
-- Append-only audit log of every job state transition.

CREATE TABLE job_events (
    id          BIGSERIAL   NOT NULL,
    job_id      UUID        NOT NULL,
    from_status job_status,           -- NULL for the initial PENDING insertion
    to_status   job_status  NOT NULL,
    worker_id   UUID,
    reason      TEXT,
    occurred_at TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT job_events_pkey PRIMARY KEY (id),
    CONSTRAINT job_events_job_fkey FOREIGN KEY (job_id) REFERENCES jobs (job_id)
);

-- Log retrieval: fetch all events for a given job in order
CREATE INDEX job_events_job_id_idx ON job_events (job_id, occurred_at ASC);
