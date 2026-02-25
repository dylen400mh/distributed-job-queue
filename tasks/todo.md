# Docker & Local Dev Infrastructure

## Notes

- Docker Compose needs: PostgreSQL 15, Redis 7, Kafka, Prometheus, Grafana
- Use Redpanda for Kafka (single container, no Zookeeper, Kafka-compatible API)
- `config.local.yaml` points at localhost ports exposed by Docker Compose
- Dockerfiles use multi-stage builds: builder (Debian + full toolchain) → runtime (slim)
- Makefile `test-integration` target: build integration_tests, set JQ_TEST_SERVER_ADDR, run
- Prometheus scrapes jq-server on port 9090 and jq-worker on port 9091
- Grafana provisioned automatically from `grafana/provisioning/` at startup

## Todo

- [x] 1. Create `docker-compose.yml` — PostgreSQL 15, Redis 7, Redpanda (Kafka), Prometheus, Grafana
- [x] 2. Create `config.local.yaml` — full config pointing at localhost Docker Compose ports
- [x] 3. Create `Makefile` — `test-integration` and `test-e2e` targets
- [x] 4. Create `docker/Dockerfile.server` — multi-stage Linux build for jq-server
- [x] 5. Create `docker/Dockerfile.worker` — multi-stage Linux build for jq-worker
- [x] 6. Create `prometheus/prometheus.yml` — scrape configs for jq-server + jq-worker
- [x] 7. Create `grafana/provisioning/datasources/prometheus.yml` — auto-provision Prometheus datasource
- [x] 8. Verify: `docker compose config --quiet` passes; `config.local.yaml` parses correctly
- [x] 9. Append to docs/activity.md and push to git

## Review

- `docker-compose.yml` — five services with healthchecks; Redpanda used instead of Kafka+Zookeeper; Prometheus on host:9095 to avoid conflict with jq-server metrics on 9090
- `config.local.yaml` — Kafka broker set to localhost:19092 (Redpanda external listener); plaintext password is local-dev only; TLS disabled
- `Makefile` — `test-e2e` starts services and binaries as background processes, captures exit code, cleans up on completion
- Dockerfiles — Ubuntu 22.04 two-stage builds; prometheus-cpp built from source (not in apt); jq-worker runs as non-root `jq` user
- `prometheus/prometheus.yml` — uses `host.docker.internal` to reach host-side binaries from inside the Prometheus container
- `grafana/provisioning/` — datasource auto-provisioned; Grafana starts with Prometheus already connected
