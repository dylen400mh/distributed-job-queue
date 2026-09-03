# Distributed Job Queue

A distributed job queue written in C++17. Jobs are submitted via gRPC, dispatched to worker processes, and executed with at-least-once guarantees. PostgreSQL is the source of truth; Redis provides distributed locking.

All operator interaction is through `jq-ctl` — there is no UI.

---

## Highlights

| | |
|---|---|
| **Throughput** | 1,052 jobs/sec sustained (ghz, 200 concurrent connections)¹ |
| **p99 latency** | 661ms submit-to-execution-start at peak load (target: 2,000ms)¹ |
| **Scheduler p95** | ≤ 5ms per cycle (target: 200ms) — measured across 118,000+ cycles¹ |
| **Docker images** | 34MB server / 34MB worker (target: < 200MB) |
| **Test coverage** | 25 unit tests + 8 end-to-end tests, all passing |
| **Infrastructure** | AWS: RDS PostgreSQL, ElastiCache Redis, single EC2 host, Terraform |

¹ Measured under a prior Kafka + Kubernetes/EKS deployment before both were removed as unnecessary (see [Requirements Status](#requirements-status)); full methodology in [docs/performance.md](docs/performance.md). Not re-benchmarked against the current architecture, which is not expected to change these numbers materially — the scheduler, gRPC, and DB paths are unchanged.

---

## Architecture

```
  Client / Operator
       │
       │  gRPC (unary)
       ▼
  ┌─────────────┐      SQL (libpqxx)    ┌──────────────┐
  │  jq-server  │ ◄──────────────────► │  PostgreSQL  │
  │             │                       └──────────────┘
  │  scheduler  │      Redis lock        ┌──────────────┐
  │  loop 500ms │ ◄──────────────────► │    Redis     │
  └──────┬──────┘                       └──────────────┘
         │
         │  gRPC server-streaming
         │  (StreamJobs)
         ▼
  ┌─────────────┐   fork/exec     ┌──────────────────┐
  │  jq-worker  │ ──────────────► │  Job subprocess  │
  │             │                 └──────────────────┘
  │  heartbeat  │
  └─────────────┘

  jq-ctl ──► jq-server (unary gRPC for all operator commands)
```

**Job lifecycle:**

```
  PENDING ──► ASSIGNED ──► RUNNING ──► DONE
                  │                      │
                  └─► FAILED ◄───────────┘
                         │
                    (retry < max)
                         │
                    ──► PENDING
                         │
                    (retry == max)
                         │
                    ──► DEAD_LETTERED
```

The scheduler runs every 500 ms, queries `PENDING` jobs ordered by `(priority DESC, created_at ASC)`, acquires a per-job Redis lock, transitions the job to `ASSIGNED`, and streams it to an available worker.

---

## Quick Start (Docker)

**Prerequisites:** Docker only. No C++ toolchain required.

```bash
# 1. Start everything (builds server + worker images on first run, ~5 min)
docker compose up -d

# 2. Wait for jq-server to finish DB migrations (watch logs until quiet)
docker compose logs -f jq-server

# 3. Create a queue
docker exec jq-server jq-ctl --server-addr localhost:50051 \
    queue create default

# 4. Submit a job
docker exec jq-server jq-ctl --server-addr localhost:50051 \
    job submit --queue default --payload '{"command":["echo","hello"]}'

# 5. Check system health
docker exec jq-server jq-ctl --server-addr localhost:50051 status
```

Services after `docker compose up`:

| Service | URL |
|---|---|
| jq-server gRPC | `localhost:50051` |
| jq-server health | `http://localhost:8080/healthz` |
| Grafana | `http://localhost:3000` (admin / admin) |
| Prometheus | `http://localhost:9095` |

---

## Quick Start (Native Build — Advanced)

**Prerequisites:** Docker, CMake ≥ 3.20, Homebrew LLVM (macOS).

```bash
# 1. Install dependencies (macOS)
brew install llvm cmake ninja grpc protobuf libpqxx hiredis \
             boost abseil spdlog nlohmann-json googletest yaml-cpp

# 2. Start only the backing services
docker compose up -d postgres redis

# 3. Configure build
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build

# 4. Build all binaries
cmake --build build --parallel

# 5. Copy and edit config
cp config.example.yaml config.local.yaml
# Passwords match docker-compose defaults (db: jqpassword, redis: none)

# 6. Start the server (runs DB migrations on startup)
./build/jq-server --config config.local.yaml

# 7. Start a worker (in another terminal)
./build/jq-worker --config config.local.yaml \
    --server-addr localhost:50051 --queues default

# 8. Submit a job
./build/jq-ctl --server-addr localhost:50051 \
    job submit --queue default --payload '{"command":["echo","hello"]}'

# 9. Check system health
./build/jq-ctl --server-addr localhost:50051 status
```

---

## jq-ctl Commands

```
# System
jq-ctl status

# Jobs
jq-ctl job submit  --queue <name> --payload '<json>'
jq-ctl job status  <job_id>
jq-ctl job logs    <job_id>
jq-ctl job list    [--queue <name>] [--status <status>] [--limit <n>]
jq-ctl job cancel  <job_id>
jq-ctl job retry   <job_id>

# Queues
jq-ctl queue create <name> [--max-retries <n>] [--ttl <seconds>]
jq-ctl queue delete <name> [--force]
jq-ctl queue stats  <name>
jq-ctl queue list

# Workers
jq-ctl worker list
jq-ctl worker drain    <worker_id>
jq-ctl worker shutdown <worker_id>
```

---

## Configuration

All values in `config.yaml` can be overridden by `JQ_<UPPER_KEY>` environment variables. Secrets must come from env vars, not the config file.

| Env var | Config key | Description |
|---|---|---|
| `JQ_DB_PASSWORD` | `db.password` | PostgreSQL password |
| `JQ_DB_HOST` | `db.host` | PostgreSQL hostname |
| `JQ_REDIS_ADDR` | `redis.addr` | Redis `host:port` |
| `JQ_REDIS_PASSWORD` | `redis.password` | Redis auth token |

See [config.example.yaml](config.example.yaml) for all options.

---

## Running Tests

```bash
# Unit tests (no external services required)
make test-unit

# Integration tests (requires Docker Compose services)
docker compose up -d
make test-integration

# End-to-end tests (starts jq-server + jq-worker internally)
make test-e2e

# Performance / NFR validation (requires running server + worker)
./build/jq-server --config config.local.yaml &
./build/jq-worker --config config.local.yaml --server-addr localhost:50051 &
bash tests/perf/throughput_test.sh

# NFR-001 throughput benchmark — requires ghz (brew install ghz)
# Use a persistent gRPC connection for accurate server-side throughput measurement.
# (jq-ctl is limited to ~130 req/s due to process-spawn overhead)
PAYLOAD=$(echo -n '{"command":["echo","bench"]}' | base64)
ghz --insecure \
    --proto proto/job_service.proto --import-paths proto \
    --call jq.JobService/SubmitJob \
    --data "{\"queue_name\":\"default\",\"payload\":\"$PAYLOAD\"}" \
    --concurrency 200 --total 10000 \
    localhost:50051
```

---

## Deployment (AWS)

Terraform provisions RDS PostgreSQL, ElastiCache Redis, ECR repositories, and a single EC2 host that runs `jq-server` + `jq-worker` via Docker Compose (bootstrapped by user-data on first boot). There's no orchestrator — the app host is a plain instance managed via SSM Session Manager (no SSH key, no open port 22).

**Prerequisites:** AWS CLI, Terraform ≥ 1.5, Docker.

```bash
# 1. Bootstrap Terraform state bucket (once)
aws s3api create-bucket --bucket <YOUR_BUCKET> --region us-east-1
aws s3api put-bucket-versioning --bucket <YOUR_BUCKET> \
    --versioning-configuration Status=Enabled

# 2. Update terraform/backend.tf with your bucket name, then:
cd terraform
terraform init
terraform apply

# 3. Get endpoints and the app host's public IP
terraform output

# 4. Build and push images to ECR
ACCOUNT=$(terraform output -raw aws_account_id)
REGION=$(terraform output -raw region)

aws ecr get-login-password --region $REGION | \
    docker login --username AWS --password-stdin $ACCOUNT.dkr.ecr.$REGION.amazonaws.com

docker build --platform linux/amd64 -f docker/Dockerfile.server \
    -t $(terraform output -raw ecr_server_url):latest .
docker build --platform linux/amd64 -f docker/Dockerfile.worker \
    -t $(terraform output -raw ecr_worker_url):latest .

docker push $(terraform output -raw ecr_server_url):latest
docker push $(terraform output -raw ecr_worker_url):latest

# 5. The instance's user-data already ran `docker compose up -d` on first boot.
#    If it booted before images existed in ECR, re-trigger it once they're pushed:
aws ssm send-command \
    --instance-ids $(terraform output -raw app_instance_id) \
    --document-name "AWS-RunShellScript" \
    --parameters commands='["cd /opt/jq","docker compose pull","docker compose up -d"]'
```

Subsequent deploys go through `.github/workflows/deploy.yml` (`workflow_dispatch`): it builds and pushes new SHA-tagged images, then rolls them out to the app host over SSM — no manual steps needed once `AWS_ROLE_ARN`, `AWS_REGION`, `AWS_ACCOUNT_ID`, and `APP_INSTANCE_ID` are set as GitHub secrets/variables (see the workflow file header).

**Smoke test against the live host:**

```bash
IP=$(cd terraform && terraform output -raw app_public_ip)
./build/jq-ctl --server-addr $IP:50051 status
./build/jq-ctl --server-addr $IP:50051 job submit \
    --queue default --payload '{"command":["echo","hello"]}'
```

---

## Requirements

Condensed from the original v1 spec (IDs are referenced in code comments throughout `src/`). Kafka- and Kubernetes-specific requirements from the original spec have been removed along with those components.

**Job submission & lifecycle** (FR-001–013) — Submit a job over gRPC with a queue name and JSON payload; optional priority (0–9, default 0), `max_retries` (defaults to the queue's setting), and TTL. The server assigns a UUID v4 `job_id` and rejects submissions to unknown queues with `NOT_FOUND`; payloads are opaque bytes. Jobs move through `PENDING → ASSIGNED → RUNNING → DONE`, with `FAILED → PENDING` on retry or `→ DEAD_LETTERED` once retries are exhausted, the TTL expires, or an operator cancels. Every transition is recorded as an immutable row in `job_events`. Operators can cancel a `PENDING`/`ASSIGNED` job or manually retry a `FAILED`/`DEAD_LETTERED` one via `jq-ctl`.

**Scheduling & dispatch** (FR-014–017) — Jobs dispatch in `(priority DESC, created_at ASC)` order, in configurable batches (default 100) per scheduling interval (default 500ms). A Redis lock per job (`SET NX PX`) ensures a job is claimed by exactly one `jq-server` instance even when several are running. A job stuck in `ASSIGNED` past `assignment_timeout` (default 60s) is marked `FAILED` and retried.

**Worker management** (FR-018–024) — Workers register with hostname and concurrency limit, then heartbeat at a configurable interval (default 5s); missing a heartbeat for `worker_heartbeat_timeout` (default 30s) marks the worker `OFFLINE` and fails its in-flight jobs for retry. A worker only receives jobs from queues in its `--queues` flag and respects its `--concurrency` limit. Operators can drain (`DRAINING`: finish current jobs, accept no new ones) or gracefully shut down a worker.

**Queue management** (FR-025–028) — Create/delete named queues (`--max-retries`, `--ttl`); deleting a non-empty queue requires `--force`. A `default` queue is seeded automatically on first startup. `jq-ctl queue stats` reports depth by status, throughput, and error rate.

**Results & retries** (FR-029–035) — Workers report a result payload and success/failure to `jq-server`, both persisted in PostgreSQL. `jq-ctl job status`/`job logs`/`job list` expose job detail, the `job_events` audit trail, and filtered/paginated listings (default limit 20, max 100). Failed jobs retry with exponential backoff + jitter (`min(base_delay * 2^attempt, max_delay) + jitter`; defaults `base_delay=5s`, `max_delay=300s`) until `max_retries`, then move to `DEAD_LETTERED` and stay queryable indefinitely.

**Observability** (FR-039–042) — `jq-server` and `jq-worker` each expose Prometheus `/metrics`; all logs are structured JSON (`timestamp`, `level`, `service`, `message`, plus context like `job_id`/`worker_id`). See `src/common/metrics/metrics.h` for the full metric list. `jq-ctl status` summarizes server reachability, active worker count, per-queue depth, and Redis/DB connectivity.

**Configuration & security** (FR-043–049) — Config loads from a YAML file (`--config`); any value is overridable via a `JQ_<PATH>` env var. Secrets (DB/Redis passwords) must come from env vars only — the server warns if it finds one in the config file. `jq-server --dry-run` validates config and tests PostgreSQL/Redis connectivity, then exits. gRPC supports optional mTLS; `jq-ctl` accepts `--tls-cert`/`--tls-key`; workers run as non-root.

**Performance** (NFR-001–004) — ≥ 1,000 jobs/sec sustained submission throughput; p99 submit-to-execution-start under 2s at queue depth < 10,000; scheduler cycle (query + lock + dispatch, batch of 100) under 200ms at p95; `jq-ctl` commands respond within 5s (configurable timeout, default 10s).

**Reliability & scale** (NFR-005–012) — No job is silently lost — a failed enqueue returns a gRPC error to the caller. At-least-once execution (handlers must be idempotent); worker crashes recover automatically via heartbeat timeout. Multiple `jq-server` replicas run without duplicate dispatch (Redis locks). PostgreSQL is the system of record — Redis loss doesn't lose jobs. Throughput scales roughly linearly with worker count; supports 100+ concurrent workers and a 10M+ row `jobs` table with proper indexing.

**Availability & operability** (NFR-013–020) — `/healthz` (liveness) and `/readyz` (readiness, checks DB + Redis) HTTP endpoints. `jq-server` shuts down gracefully within 30s of `SIGTERM`; `jq-worker` within 60s. All binaries support `--help` and `--version`. Docker images are multi-stage builds under 200MB. `docker compose up` starts the full local stack (PostgreSQL, Redis, `jq-server`, `jq-worker`, Prometheus, Grafana). DB schema changes are versioned Flyway-style migrations in `db/migrations/`, applied automatically on startup.

**Testability** (NFR-021–023) — Unit tests cover scheduling, retry/backoff math, state transitions, and config parsing. Integration tests run against real PostgreSQL and Redis via Docker Compose (`make test-integration`). End-to-end tests drive the system through `jq-ctl` (`make test-e2e`).

**Constraints** — C++17/20; Linux (Ubuntu 22.04/Debian Bookworm) via Docker in production, native macOS toolchain for development, Windows unsupported; deployable on AWS via EC2, RDS, ElastiCache, and ECR; no UI (CLI/logs/metrics only); v1 has no multi-tenancy, job dependencies/DAGs, or workflow orchestration.

---

## Requirements Status

> Historical record from before Kafka and Kubernetes were removed (see above) — FR/NFR
> counts and IDs below reflect the spec as it existed at the time. Full results in
> [docs/test-results.md](docs/test-results.md); performance methodology and charts in
> [docs/performance.md](docs/performance.md).

| Category | Tested | Pass | Partial | Not Tested |
|---|---|---|---|---|
| Functional Requirements (49 total) | 49 | 42 | 5 | 2 |
| Non-Functional Requirements (23 total) | 23 | 21 | 2 | 0 |

### Test Suite Results

| Suite | Tests | Pass |
|---|---|---|
| Unit tests | 25 | 25 |
| E2e integration tests | 8 | 8 |

### Performance (measured on AWS EKS, us-east-1)

| NFR | Target | Measured | Result |
|---|---|---|---|
| NFR-001 throughput | ≥ 1,000 jobs/s | **1,052 jobs/s** (ghz 200c, NLB, 10k requests, 0 errors) | **PASS** |
| NFR-002 p99 latency | < 2s | **647ms** local / **639ms** at EKS peak load | **PASS** |
| NFR-003 scheduler p95 | < 200ms | **≤ 5ms** (Prometheus histogram, 187 cycles, EKS) | **PASS** |

Note: NFR-001 requires `ghz` (persistent gRPC connection). `jq-ctl` spawns a new process per call (~8ms overhead) and is limited to ~130 req/s regardless of server capacity.

### Known Gaps

| Gap | Severity |
|---|---|
| FR-004: TTL only catches PENDING jobs (ASSIGNED jobs with no worker don't expire) | Low |
| FR-047/048: mTLS infrastructure exists but not end-to-end tested | Low |
| FR-045: No log warning if secrets appear in YAML config file | Low |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| Build | CMake ≥ 3.20 + Ninja |
| gRPC | grpc++ + protobuf |
| Database | PostgreSQL via libpqxx |
| Cache / Locks | Redis via hiredis |
| Metrics | prometheus-cpp |
| Logging | spdlog (JSON sink) |
| Config | yaml-cpp |
| Container | Docker multi-stage (Ubuntu 22.04) |
| Deployment | Docker Compose on a single EC2 host (SSM-managed) |
| Infrastructure | Terraform (VPC, EC2, RDS, ElastiCache, ECR) |

---

## Directory Structure

```
proto/          # .proto definitions (JobService, WorkerService, AdminService)
src/
  server/       # jq-server: gRPC handlers, scheduler loop, health endpoints
  worker/       # jq-worker: job executor, heartbeat loop
  ctl/          # jq-ctl: operator CLI
  common/       # Shared: config, logging, db, redis, metrics
db/migrations/  # Versioned SQL migrations (applied automatically on startup)
docker/         # Multi-stage Dockerfiles for server and worker
terraform/      # AWS infrastructure (VPC, EC2, RDS, ElastiCache, ECR) + app host user-data
tests/
  unit/         # GoogleTest unit tests (25 tests)
  integration/  # End-to-end tests via jq-ctl (8 tests)
  perf/         # Performance measurement script (NFR-001/002/003)
docs/           # Architecture docs, test results, activity log
prometheus/     # Prometheus config + alert rules
grafana/        # Grafana dashboard JSON
```
