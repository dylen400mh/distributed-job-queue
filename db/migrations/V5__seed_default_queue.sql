-- V5: Seed the default queue.
-- Created automatically on first startup per FR-027.

INSERT INTO queues (name, max_retries, ttl_seconds)
VALUES ('default', 3, NULL)
ON CONFLICT (name) DO NOTHING;
