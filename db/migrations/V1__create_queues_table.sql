-- V1: Create the queues table.
-- Queues are named configurations that jobs are submitted to.

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE queues (
    name        TEXT        NOT NULL,
    max_retries SMALLINT    NOT NULL DEFAULT 3,
    ttl_seconds INTEGER,                           -- NULL means no TTL
    created_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    updated_at  TIMESTAMPTZ NOT NULL DEFAULT now(),
    CONSTRAINT queues_pkey PRIMARY KEY (name),
    CONSTRAINT queues_max_retries_check CHECK (max_retries >= 0),
    CONSTRAINT queues_ttl_check CHECK (ttl_seconds IS NULL OR ttl_seconds > 0)
);
