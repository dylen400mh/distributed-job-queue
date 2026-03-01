# Distributed Job Queue

A production-grade, distributed job queue written in C++17. Jobs are submitted via gRPC, dispatched to worker processes, and executed with at-least-once guarantees. PostgreSQL is the source of truth; Redis provides distributed locking; Kafka publishes every state transition as an event stream.

All operator interaction is through `jq-ctl` — there is no UI.

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
         │                               ┌──────────────┐
         │  gRPC server-streaming        │    Kafka     │
         │  (StreamJobs)                 └──────────────┘
         ▼                                     ▲
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

## Quick Start (Local)

**Prerequisites:** Docker, CMake ≥ 3.20, Homebrew LLVM (macOS).

```bash
# 1. Install dependencies (macOS)
brew install llvm cmake ninja grpc protobuf libpqxx hiredis librdkafka \
             boost abseil spdlog nlohmann-json googletest yaml-cpp

# 2. Configure build
cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build

# 3. Build all binaries
cmake --build build --parallel

# 4. Start backing services (PostgreSQL, Redis, Redpanda, Prometheus, Grafana)
docker compose up -d

# 5. Copy and edit config
cp config.example.yaml config.local.yaml
# Set db.password to match docker-compose.yml (default: "jq")

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
| `JQ_KAFKA_BROKERS` | `kafka.brokers` | Comma-separated broker list |

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

## Deployment (AWS / EKS)

**Prerequisites:** AWS CLI, Terraform ≥ 1.5, kubectl, Docker.

```bash
# 1. Bootstrap Terraform state bucket (once)
aws s3api create-bucket --bucket <YOUR_BUCKET> --region us-east-1
aws s3api put-bucket-versioning --bucket <YOUR_BUCKET> \
    --versioning-configuration Status=Enabled

# 2. Update terraform/backend.tf with your bucket name, then:
cd terraform
terraform init
terraform apply

# 3. Configure kubectl
aws eks update-kubeconfig --name jq-cluster --region us-east-1

# 4. Get endpoints from terraform output
terraform output

# 5. Create namespace and Kubernetes secret
kubectl apply -f k8s/namespace.yaml
kubectl create secret generic jq-secret --namespace jq \
    --from-literal=JQ_DB_HOST=<rds_endpoint> \
    --from-literal=JQ_DB_USER=jq \
    --from-literal=JQ_DB_PASSWORD=<password> \
    --from-literal=JQ_REDIS_ADDR=<elasticache_endpoint>:6379 \
    --from-literal=JQ_REDIS_PASSWORD="" \
    --from-literal=JQ_KAFKA_BROKERS=<msk_bootstrap_brokers>

# 6. Build and push images to ECR
ACCOUNT=<aws_account_id>
REGION=us-east-1

aws ecr get-login-password --region $REGION | \
    docker login --username AWS --password-stdin $ACCOUNT.dkr.ecr.$REGION.amazonaws.com

docker build --platform linux/amd64 -f docker/Dockerfile.server \
    -t $ACCOUNT.dkr.ecr.$REGION.amazonaws.com/jq-server:v0.1.0 .
docker build --platform linux/amd64 -f docker/Dockerfile.worker \
    -t $ACCOUNT.dkr.ecr.$REGION.amazonaws.com/jq-worker:v0.1.0 .

docker push $ACCOUNT.dkr.ecr.$REGION.amazonaws.com/jq-server:v0.1.0
docker push $ACCOUNT.dkr.ecr.$REGION.amazonaws.com/jq-worker:v0.1.0

# 7. Apply Kubernetes manifests
kubectl apply -f k8s/configmap.yaml
kubectl apply -f k8s/server/
kubectl apply -f k8s/worker/

# 8. Verify
kubectl get pods -n jq
kubectl rollout status deployment/jq-server -n jq
kubectl rollout status deployment/jq-worker -n jq
```

**Smoke test against live cluster:**

```bash
kubectl port-forward -n jq service/jq-server 50051:50051 &
./build/jq-ctl --server-addr localhost:50051 status
./build/jq-ctl --server-addr localhost:50051 job submit \
    --queue default --payload '{"command":["echo","hello"]}'
```

---

## Requirements Status

Full results in [docs/test-results.md](docs/test-results.md). Performance methodology and charts in [docs/performance.md](docs/performance.md).

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
| NFR-019: docker-compose.yml doesn't include jq-server/jq-worker services | Low |

---

## Tech Stack

| Layer | Technology |
|---|---|
| Language | C++17 |
| Build | CMake ≥ 3.20 + Ninja |
| gRPC | grpc++ + protobuf |
| Database | PostgreSQL via libpqxx |
| Cache / Locks | Redis via hiredis |
| Events | Apache Kafka via librdkafka |
| Metrics | prometheus-cpp |
| Logging | spdlog (JSON sink) |
| Config | yaml-cpp |
| Container | Docker multi-stage (Ubuntu 22.04) |
| Orchestration | Kubernetes / EKS |
| Infrastructure | Terraform (VPC, EKS, RDS, ElastiCache, MSK, ECR) |

---

## Directory Structure

```
proto/          # .proto definitions (JobService, WorkerService, AdminService)
src/
  server/       # jq-server: gRPC handlers, scheduler loop, health endpoints
  worker/       # jq-worker: job executor, heartbeat loop
  ctl/          # jq-ctl: operator CLI
  common/       # Shared: config, logging, db, redis, kafka, metrics
db/migrations/  # Versioned SQL migrations (applied automatically on startup)
docker/         # Multi-stage Dockerfiles for server and worker
k8s/            # Kubernetes manifests (Deployment, Service, HPA, PDB)
terraform/      # AWS infrastructure (VPC, EKS, RDS, ElastiCache, MSK, ECR)
tests/
  unit/         # GoogleTest unit tests (25 tests)
  integration/  # End-to-end tests via jq-ctl (8 tests)
  perf/         # Performance measurement script (NFR-001/002/003)
docs/           # Architecture docs, test results, activity log
prometheus/     # Prometheus config + alert rules
grafana/        # Grafana dashboard JSON
```
