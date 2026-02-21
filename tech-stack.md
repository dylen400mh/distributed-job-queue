# Tech Stack: Distributed Job Queue

This document describes the full technology stack for a distributed job queue system. Use it as a reference when generating code, configuration files, infrastructure definitions, and architectural decisions.

---

## Overview

A high-performance, horizontally scalable distributed job queue with gRPC-based APIs, persistent storage, event streaming, real-time metrics, and cloud-native deployment.

---

## Core Language & Runtime

- **Language:** C++17 (or C++20 where supported)
- **Build System:** CMake (≥ 3.20)
- **Package Manager:** Homebrew for macOS development dependencies; vcpkg or Conan for cross-platform C++ library management
- **Compiler (macOS dev):** Clang 15+ via Homebrew LLVM (`brew install llvm`). Do NOT use Apple's system Clang — it lags on C++17/20 stdlib support and has known incompatibilities with gRPC and Abseil. Set `CC` and `CXX` to the Homebrew LLVM paths in CMake.
- **Compiler (production/Docker):** GCC 12+ or Clang 15+ inside the Linux Docker builder stage
- **Standard Libraries:** STL, Abseil (for additional utilities if needed)
- **Development OS:** macOS (native toolchain). All backing services (PostgreSQL, Redis, Kafka, Prometheus, Grafana) run locally via Docker Compose. Production runs in Linux Docker containers on AWS.

**macOS Homebrew setup (one-time):**
```bash
brew install llvm cmake ninja grpc protobuf libpqxx hiredis librdkafka \
             boost abseil spdlog nlohmann-json googletest
```

**CMake toolchain pointer for Homebrew LLVM:**
```cmake
set(CMAKE_C_COMPILER /opt/homebrew/opt/llvm/bin/clang)
set(CMAKE_CXX_COMPILER /opt/homebrew/opt/llvm/bin/clang++)
```

---

## API Layer — gRPC

- **Framework:** gRPC (C++ library via `grpc++`)
- **Serialization:** Protocol Buffers (protobuf v3)
- **Proto files:** Stored in `/proto` directory; compiled via `protoc` with the `grpc_cpp_plugin`
- **Patterns:** Unary RPC for job submission/status queries; server-streaming RPC for job progress updates
- **Auth:** gRPC interceptors for token-based authentication (JWT or API keys)
- **TLS:** Mutual TLS (mTLS) for service-to-service communication; plaintext allowed in local dev

**Key services to define in `.proto` files:**
- `JobService` — Submit, cancel, and query jobs
- `WorkerService` — Worker registration, heartbeat, and result reporting
- `AdminService` — Queue management, worker pool stats

---

## Primary Database — PostgreSQL

- **Version:** PostgreSQL 15+
- **Hosting:** AWS RDS (PostgreSQL engine), Multi-AZ for production
- **Local dev:** Docker Compose (PostgreSQL 15 container); do not use a Homebrew-managed Postgres instance to avoid port conflicts
- **C++ Client:** `libpqxx` (v7+)
- **Schema Management:** Flyway or Liquibase for migrations (stored in `/db/migrations`)
- **Connection Pooling:** PgBouncer (sidecar or standalone) in transaction pooling mode

**Core tables:**
- `jobs` — job ID, payload, status, priority, created_at, updated_at, worker_id, retry_count
- `workers` — worker ID, hostname, status, last_heartbeat, capacity
- `job_events` — append-only audit log of all job state transitions
- `queues` — named queue definitions with config (max retries, TTL, priority weights)

**Indexes:** On `status`, `priority`, `created_at`, and `worker_id` for efficient polling and ordering.

---

## Caching & Fast State — Redis

- **Version:** Redis 7+
- **Hosting:** AWS ElastiCache (Redis cluster mode enabled) or self-managed on EC2
- **Local dev:** Docker Compose (Redis 7 container)
- **C++ Client:** `hiredis` + `redis-plus-plus` wrapper
- **Use Cases:**
  - Job status cache (short TTL to reduce DB reads)
  - Distributed locks (Redlock algorithm) for preventing duplicate job execution
  - Worker presence / heartbeat tracking (key expiry as TTL-based health check)
  - Rate limiting per client/queue
  - In-flight job counters per worker

---

## Event Streaming — Kafka

- **Version:** Apache Kafka 3.5+
- **Hosting:** Self-managed on EC2 (or MSK — Amazon Managed Streaming for Apache Kafka)
- **Local dev:** Docker Compose (Kafka + Zookeeper or KRaft containers)
- **C++ Client:** `librdkafka` (v2+)
- **Topics:**
  - `job.submitted` — new job events
  - `job.started` — job picked up by a worker
  - `job.completed` — successful completion with result
  - `job.failed` — failure with error details and retry info
  - `job.dead-lettered` — jobs that exceeded max retries
  - `worker.heartbeat` — periodic worker liveness signals
- **Consumer Groups:** One consumer group per service type (e.g., `result-processor`, `metrics-aggregator`)
- **Retention:** 7 days default; dead-letter topic retained for 30 days
- **Serialization:** Protobuf (reuse `.proto` definitions from gRPC layer)

---

## Metrics & Observability — Prometheus + Grafana

- **Metrics Exposition:** Expose a `/metrics` HTTP endpoint from each service using the `prometheus-cpp` client library
- **Prometheus:** Scrapes all services; runs in Docker Compose locally, deployed via Kubernetes in production
- **Grafana:** Dashboards for job throughput, queue depth, worker utilization, latency percentiles, error rates; runs in Docker Compose locally
- **Alerting:** Prometheus Alertmanager for threshold-based alerts (PagerDuty/Slack integration)

**Key metrics to instrument:**
- `jq_job_queue_depth` (gauge, per queue)
- `jq_job_processing_duration_seconds` (histogram, p50/p95/p99)
- `jq_job_total` (counter, labeled by queue and status)
- `jq_worker_active_count` (gauge)
- `jq_grpc_request_duration_seconds` (histogram, labeled by method)
- `jq_kafka_consumer_lag` (gauge, per topic/partition)
- `jq_db_query_duration_seconds` (histogram)
- `jq_redis_operation_duration_seconds` (histogram)

**Additional Observability:**
- **Structured Logging:** JSON logs via spdlog; ship to CloudWatch Logs in production
- **Distributed Tracing:** OpenTelemetry C++ SDK -> AWS X-Ray or Jaeger
- **AWS CloudWatch:** EC2 instance metrics, RDS performance insights, custom metrics via CloudWatch agent

---

## Containerization — Docker

- **Base Image:** `ubuntu:22.04` or `debian:bookworm-slim` for runtime; multi-stage builds to keep images lean
- **Multi-stage Build:**
  - Stage 1 (`builder`): Full Linux build toolchain (GCC/Clang), compiles the C++ binaries inside the container
  - Stage 2 (`runtime`): Minimal image with only shared libraries and the compiled binary
- **Note:** macOS developers build natively for local iteration. Docker builds are used for producing production Linux images and for running backing services locally via Docker Compose.
- **Images to produce:**
  - `jq-server` — gRPC server + scheduler daemon
  - `jq-worker` — job executor daemon
- **Registry:** Amazon ECR (Elastic Container Registry)
- **Docker Compose:** Runs all backing services locally: PostgreSQL, Redis, Kafka, Prometheus, Grafana

---

## Orchestration — Kubernetes

- **Cluster:** Amazon EKS (Elastic Kubernetes Service) or self-managed K8s on EC2
- **Manifests:** Stored in `/k8s` directory; organized by component
- **Key Resources:**
  - `Deployment` for stateless services (jq-server, jq-worker)
  - `HorizontalPodAutoscaler` (HPA) for workers based on `jq_job_queue_depth` custom metric
  - `ConfigMap` for environment-specific config
  - `Secret` for credentials (synced from AWS Secrets Manager via External Secrets Operator)
  - `Service` + `Ingress` for gRPC endpoint exposure (use `nginx-ingress` with `grpc` annotation or AWS ALB)
  - `PodDisruptionBudget` for rolling update safety
- **Namespaces:** `job-queue-prod`, `job-queue-staging`, `job-queue-dev`
- **Service Mesh (optional):** Istio or AWS App Mesh for mTLS, traffic management, and observability

---

## Cloud Infrastructure — AWS

| Resource | Service | Notes |
|---|---|---|
| Compute | EC2 (via EKS node groups) | `c5.xlarge` or `m5.large` instance types |
| Database | RDS PostgreSQL 15 | Multi-AZ, automated backups, encryption at rest |
| Cache | ElastiCache Redis | Cluster mode, Multi-AZ, encryption in transit |
| Kafka | MSK or EC2 | MSK preferred for managed ops |
| Container Registry | ECR | Image scanning enabled |
| Secrets | AWS Secrets Manager | Rotated credentials for DB and Redis |
| Logging | CloudWatch Logs | Log groups per service |
| Monitoring | CloudWatch + Prometheus | Dual-stack observability |
| Load Balancing | ALB | gRPC-compatible, TLS termination |
| DNS | Route 53 | Internal and external service discovery |
| Networking | VPC, private subnets, Security Groups | RDS and Redis in private subnets only |

---

## Project Directory Structure

```
/
├── proto/                      # Protobuf definitions
├── src/
│   ├── server/                 # jq-server: gRPC API + scheduler daemon
│   ├── worker/                 # jq-worker: job executor daemon
│   ├── ctl/                    # jq-ctl: operator CLI tool
│   └── common/                 # Shared utilities, models, DB/Redis/Kafka clients
├── db/
│   └── migrations/             # SQL migration files (Flyway/Liquibase)
├── k8s/                        # Kubernetes manifests
├── docker/                     # Dockerfiles per service
├── docker-compose.yml          # Local backing services (Postgres, Redis, Kafka, Prometheus, Grafana)
├── CMakeLists.txt              # Root CMake build file
├── vcpkg.json                  # C++ dependencies manifest (fallback if not using Homebrew)
├── cmake/
│   └── toolchain-macos.cmake   # Homebrew LLVM toolchain file for local dev
├── prometheus/                 # Prometheus config & alert rules
├── grafana/                    # Grafana dashboard JSON exports
└── docs/                       # Architecture diagrams, ADRs
```

---

## Key C++ Dependencies

| Library | Homebrew (macOS dev) | vcpkg / Conan | Purpose |
|---|---|---|---|
| `grpc` + `protobuf` | `brew install grpc protobuf` | `grpc` | gRPC runtime + protobuf |
| `libpqxx` | `brew install libpqxx` | `libpqxx` | PostgreSQL C++ client |
| `hiredis` + `redis-plus-plus` | `brew install hiredis` | `hiredis`, `redis-plus-plus` | Redis client |
| `librdkafka` | `brew install librdkafka` | `librdkafka` | Kafka producer/consumer |
| `prometheus-cpp` | build from source | `prometheus-cpp` | Prometheus metrics exposition |
| `opentelemetry-cpp` | build from source | `opentelemetry-cpp` | Distributed tracing |
| `spdlog` | `brew install spdlog` | `spdlog` | Fast structured logging |
| `nlohmann-json` | `brew install nlohmann-json` | `nlohmann-json` | JSON serialization |
| `boost` | `brew install boost` | `boost` | Asio async I/O, utilities |
| `abseil` | `brew install abseil` | `abseil` | String utils, synchronization primitives |
| `gtest` + `gmock` | `brew install googletest` | `gtest` | Unit and integration testing |

---

## Development Workflow (macOS)

1. Install dependencies via Homebrew (see one-liner in Core Language & Runtime section above)
2. Run `docker compose up -d` to start all backing services (Postgres, Redis, Kafka, Prometheus, Grafana)
3. Configure CMake to use the Homebrew LLVM toolchain: `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build`
4. Build all targets: `cmake --build build --parallel`
5. Run `./build/jq-server --config config.local.yaml` and `./build/jq-worker --config config.local.yaml` pointing at `localhost` service ports
6. Use `./build/jq-ctl` from the same build directory to interact with the running server
7. To produce production Linux images: `docker build -f docker/Dockerfile.server -t jq-server .`

---

## Development Conventions

- All service configuration via **environment variables** (12-factor app); no hardcoded credentials
- Use **structured logging** (JSON) with correlation IDs propagated across services via gRPC metadata
- All database access through a **repository pattern**; no raw SQL outside repository classes
- **Retry logic** with exponential backoff and jitter for Kafka producers, Redis, and gRPC calls
- **Graceful shutdown**: drain in-flight jobs, flush Kafka producer, close DB connections on SIGTERM
- **Health checks**: implement `/healthz` (liveness) and `/readyz` (readiness) HTTP endpoints on a secondary port
- **Feature flags**: environment-variable-driven for safe rollouts
