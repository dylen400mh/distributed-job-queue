# NFR-019 Fix: docker-compose includes jq-server + jq-worker

## Goal
A reviewer can run `docker compose up` and get a fully operational system — no local C++ toolchain required.

## Plan

- [ ] Create `config.docker.yaml` — config using docker-internal service names (postgres, redis, redpanda:9092)
- [ ] Update `docker/Dockerfile.server` — also build and install `jq-ctl` so reviewers can interact via `docker exec jq-server jq-ctl ...`
- [ ] Update `docker-compose.yml` — add `jq-server` and `jq-worker` services built from their Dockerfiles; bind-mount config; expose ports
- [ ] Update `prometheus/prometheus.yml` — scrape `jq-server:9090` and `jq-worker:9090` by container name (no longer host.docker.internal)
- [ ] Update `README.md` — new "Quick Start (Docker)" section at top; native build moved to "Advanced"; remove NFR-019 from Known Gaps table
- [x] Log to docs/activity.md and push

## Review

Five files changed to close NFR-019:

| File | Change |
|---|---|
| `config.docker.yaml` (new) | Config with docker-internal hostnames for the compose demo |
| `docker/Dockerfile.server` | Also builds + installs `jq-ctl` so `docker exec jq-server jq-ctl` works |
| `docker-compose.yml` | Added `jq-server` and `jq-worker` services; prometheus now scrapes by container name |
| `prometheus/prometheus.yml` | Targets changed from `host.docker.internal` to container names |
| `README.md` | Docker Quick Start promoted to top; native build moved to Advanced; NFR-019 removed from Known Gaps |

A reviewer can now run `docker compose up -d` and get a fully working system with no local C++ toolchain.
