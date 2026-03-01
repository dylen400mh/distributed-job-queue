# Requirements Verification & Deployment

## Goal

Verify all FRs and NFRs from requirements.md, run tests locally, deploy to AWS, then write README.

---

## Todo

- [x] Phase 1: Fix Kafka health check (KafkaProducer::IsHealthy, GetSystemStatus)
- [x] Phase 2: Add AdminService unit tests (10 cases, all pass)
- [x] Phase 3: Add 5 new e2e integration test scenarios
- [x] Phase 4: Add performance measurement script (tests/perf/throughput_test.sh)
- [x] Phase 5: Run full local test suite (25 unit tests pass, 8/8 e2e pass)
- [x] Phase 6: Docker image builds (jq-server: 34 MB, jq-worker: 34 MB — NFR-018 ✓)
- [x] Phase 6: NFR spot-checks (--help ✓, --version ✓, docker images ✓)
- [x] Phase 6: Performance measurement (NFR-002: 647ms ✓, NFR-001: methodology limited)
- [x] Phase 7: docs/test-results.md — FR/NFR tracking table
- [x] Phase 8: Deploy to AWS
  - [x] Run `terraform apply` to provision infrastructure (56 resources: VPC, EKS, RDS, ElastiCache, MSK, ECR)
  - [x] Configure kubectl for EKS cluster
  - [x] Create jq-secret in Kubernetes
  - [x] Push images to ECR (v0.1.0, v0.1.1)
  - [x] Verify pods are healthy: 2x jq-server, 2x jq-worker all Running
  - [x] Run smoke test: PENDING→ASSIGNED→RUNNING→DONE in ~3s, all components HEALTHY
  - [x] Fix metrics weak_ptr bug (RegisterCollectable stores weak_ptr; shared_ptr must outlive call)
  - [x] Measure NFR-001: 1,052 jobs/s via ghz (200 concurrency, 10k requests) — PASS
  - [x] Measure NFR-003: p95 ≤ 5ms via Prometheus histogram on live cluster — PASS
- [ ] Phase 9: Write README.md

---

## Performance Results

| NFR | Target | Measured | Result |
|---|---|---|---|
| NFR-001 (throughput) | ≥ 1000 jobs/s | **1,052 jobs/s** (ghz, 200 concurrency, AWS EKS+NLB) | **PASS** |
| NFR-002 (p99 latency) | < 2s | **647ms** (local empty queue); 639ms at peak load on EKS | **PASS** |
| NFR-003 (scheduler p95) | < 200ms | **≤ 5ms** (187 cycles, p95 in 5ms bucket, EKS Prometheus) | **PASS** |

## Known Gaps

- FR-004: TTL only fires for PENDING jobs; ASSIGNED jobs with no worker never expire
- FR-045: No log warning when sensitive values appear in config file
- FR-047/048: mTLS not end-to-end tested
- NFR-019: docker-compose.yml does not start jq-server/jq-worker (local dev pattern)
