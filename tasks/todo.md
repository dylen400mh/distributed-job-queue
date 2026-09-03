# Remove Kafka and Kubernetes from the job queue

## Goal
Kafka is a write-only, unconsumed event log (Postgres `job_events` already
durably records every transition in the same transaction) and Kubernetes is
one of two deployment targets layered on top of Docker Compose (which already
runs the full stack per NFR-019). Neither is load-bearing. Remove both,
simplify the AWS/Terraform footprint accordingly, and consolidate the spec
docs into README.md / CLAUDE.md.

Confirmed with user:
- AWS infra (EKS/MSK) is already destroyed — safe to edit Terraform directly, no `terraform destroy` needed.
- Scope: Kafka + Kubernetes, replacing EKS as the deploy target with a single EC2 instance running Docker Compose (so `deploy.yml` still has something to roll out to instead of becoming build-only). Keep Terraform for RDS/ElastiCache/VPC/ECR.
- No event-bus replacement for Kafka; `job_events` table is the sole durable event record.
- Consolidate requirements.md + design-notes.md + tech-stack.md into README.md/CLAUDE.md, then delete the three files.

## Plan

### A. Remove Kafka [DONE]
- [x] Delete `src/common/kafka/` (kafka_producer.h/.cc, kafka_consumer.h/.cc)
- [x] `src/common/config/config.h` / `.cc` — remove `KafkaConfig` struct, YAML parsing, env var overrides (`JQ_KAFKA_*`), validation (`kafka.brokers is required`)
- [x] Remove `IKafkaProducer& kafka` param/member/wiring from:
  - `JobServiceImpl` (job_service_impl.h/.cc) — drop `PublishEvent`, kafka_.Publish calls
  - `WorkerServiceImpl` (worker_service_impl.h/.cc)
  - `AdminServiceImpl` (admin_service_impl.h/.cc) — drop "kafka" component from `GetSystemStatus`
  - `Scheduler` (scheduler.h/.cc) — drop `kafka_.Publish` calls in `ApplyRetry` etc.
  - `HealthServer` (health_server.h/.cc) — drop Kafka from `/readyz` check
  - `GrpcServer` (server.h/.cc) — drop kafka arg threading
  - `src/server/main.cc` — drop `KafkaProducer` construction, `TestKafka`, `--dry-run` Kafka check, `kafka->Flush` shutdown calls
- [x] `src/common/metrics/metrics.h` / `.cc` — remove `KafkaPublishErrorsTotal`
- [x] `CMakeLists.txt` — remove `jq_kafka` target, RDKAFKA pkg-config, all `jq_kafka` links, librdkafka from the toolchain pkgconfig path
- [x] `vcpkg.json` — remove `librdkafka`, update description
- [x] `tests/CMakeLists.txt` — remove `kafka_unit_tests` target, `jq_kafka` links from `server_unit_tests`
- [x] Delete `tests/unit/kafka/`
- [x] `tests/unit/server/job_service_test.cc`, `admin_service_test.cc` — remove `MockKafkaProducer`, kafka_ member, constructor args, `EXPECT_CALL(kafka, ...)`
- [x] `tests/unit/config/config_test.cc` — drop stale kafka comment reference
- [x] `docker-compose.yml` — remove `redpanda` service and its `depends_on` edge from `jq-server`
- [x] `config.example.yaml`, `config.local.yaml`, `config.docker.yaml` — remove `kafka:` section
- [x] `prometheus/alerts.yaml` — remove `KafkaPublishErrors` alert
- [x] `grafana/provisioning/dashboards/jq-dashboard.json` — remove Kafka publish-error panel
- [x] `prometheus/prometheus.yml` — check/remove any redpanda scrape target

### B. Remove Kubernetes, replace with a single EC2 host
- [x] Delete `k8s/` directory entirely
- [x] `terraform/eks.tf` — delete (EKS cluster + node group)
- [x] `terraform/msk.tf` — delete (in scope since Kafka is going too)
- [x] `terraform/iam.tf` — remove `eks_cluster` / `eks_node` IAM roles + attachments; remove `EKSDescribe` statement from the GitHub Actions policy; add an `app` EC2 instance role (SSM managed-instance core, ECR read-only, Secrets Manager read on the DB password secret) + instance profile
- [x] `terraform/security_groups.tf` — remove `eks_control_plane` SG + its cross-reference rules and `msk` SG; rename `eks_nodes` → `app`, scoped to inbound gRPC/health/metrics (50051/8080/9090) from `0.0.0.0/0` (public, like the old NLB path) instead of pod-to-pod self-ingress; repoint the RDS/Redis ingress rules at it
- [x] `terraform/vpc.tf` — drop `kubernetes.io/...` subnet tags (meaningless without EKS)
- [x] New `terraform/ec2.tf` — one `aws_instance` (Amazon Linux 2023, public subnet, public IP, the `app` SG + instance profile), `templatefile()` user-data that installs Docker, writes a minimal `config.yaml` + `/opt/jq/docker-compose.yml` pulling `jq-server`/`jq-worker` from ECR (`IMAGE_TAG` env, default `latest`), fetches the DB password from Secrets Manager, and brings the stack up. No SSH — access is via SSM Session Manager only.
- [x] New `terraform/templates/app_user_data.sh.tftpl`
- [x] `terraform/variables.tf` — remove `kubernetes_version`, `node_instance_type/min/max/desired`, `kafka_broker_instance_type/count`, `kafka_version`; add `app_instance_type` (default `t3.small`)
- [x] `terraform/outputs.tf` — remove `eks_cluster_name`, `msk_bootstrap_brokers` outputs; add `app_instance_id` output (consumed by `deploy.yml`); update header comment
- [x] `.github/workflows/deploy.yml` — replace the "Deploy to EKS" job with a "Deploy to EC2" job: after build-and-push, `aws ssm send-command` (`AWS-RunShellScript`) on `vars.APP_INSTANCE_ID` to set `IMAGE_TAG=sha-$GITHUB_SHA` in `/opt/jq/.env`, `docker compose pull && docker compose up -d`, then poll `ssm get-command-invocation` for success; update header comment (`EKS_CLUSTER_NAME` → `APP_INSTANCE_ID`)
- [x] `terraform/iam.tf` — swap the GitHub Actions policy's `EKSDescribe` statement for an `SSMDeploy` statement scoped to the app instance ARN + the `AWS-RunShellScript` document ARN
- [x] `.github/workflows/terraform.yml` — update comment mentioning "VPCs, EKS, RDS, MSK, IAM"
- [x] `terraform validate` / `terraform fmt -check` locally (provider already cached under `terraform/.terraform`) — no `plan`/`apply` against real AWS

### C. Consolidate docs
- [x] Fold the FR/NFR requirements list (minus Kafka/K8s-specific items: FR-034/036-038/042/045/046-partial, NFR-013/019/022 wording, C-002/C-003) into a condensed "Requirements" section in README.md — keep IDs since code comments cite them
- [x] Fold design-notes.md rationale (scheduler loop detail, graceful shutdown sequence, config schema, testing strategy) into CLAUDE.md's existing "Architecture" section
- [x] Fold tech-stack.md's dependency/directory detail into CLAUDE.md's existing structure section + README's stack table
- [x] Delete `requirements.md`, `design-notes.md`, `tech-stack.md`
- [x] Update README.md: intro paragraph, Highlights table, Architecture diagram, Tech Stack table, Directory Structure, "Deployment (AWS/EKS)" section → rewrite for RDS/ElastiCache/ECR/single EC2 host (SSM-based deploy, no kubectl)
- [x] Update CLAUDE.md: Architecture section (drop Kafka bullet), Directory Structure (drop `kafka/`, `k8s/`), Reference Documents section (docs no longer exist)
- [x] Delete `prompts.md` (build-prompt log for the original from-scratch build; no longer needed)
- [x] Leave `docs/performance.md` and `docs/test-results.md` untouched — historical measurement records of the prior EKS deployment, not current-state specs

### D. Verify
- [x] `cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-macos.cmake -B build && cmake --build build --parallel`
- [x] `./build/tests/unit_tests` (or per-target tests per tests/CMakeLists.txt)
- [x] `docker compose config` sanity check (no dangling redpanda refs)
- [x] `grep -rn -i kafka` / `grep -rn -i "kubernetes\|k8s\|eks"` across the repo — confirm only historical docs (performance.md/test-results.md) remain

### E. Ship
- [x] New branch `remove-kafka-k8s`
- [x] Logical commits per phase (A/B/C)
- [x] Append activity log entry to `docs/activity.md`
- [ ] Push branch, open PR to `main`

## Review

**Kafka** was write-only and unconsumed — `jq-server` published lifecycle events that nothing ever read (`KafkaConsumer` existed in the source tree but was never instantiated), and every event it carried was already durably written to `job_events` in the same DB transaction. Removed entirely: `src/common/kafka/`, the `IKafkaProducer` wiring through every server-side service and the scheduler, the CMake target + librdkafka dependency, the Prometheus alert + Grafana panel, the Redpanda dev service, and all `kafka_unit_tests`/mock plumbing in tests.

**Kubernetes/EKS** was one of two deployment targets layered on top of Docker Compose, which already runs the full stack standalone (NFR-019). Removed `k8s/` and the EKS/MSK Terraform. Per the user's choice, replaced the EKS deploy target with a single EC2 host (`terraform/ec2.tf` + `templates/app_user_data.sh.tftpl`) running the same Docker Compose stack, managed via SSM (no SSH). `deploy.yml` now rolls out new images via `aws ssm send-command` instead of `kubectl set image`. RDS, ElastiCache, ECR, and the VPC are unchanged.

**Docs** — `requirements.md`, `design-notes.md`, and `tech-stack.md` were folded into `README.md` (condensed Requirements section, updated architecture/tech-stack/deployment) and `CLAUDE.md` (design rationale, graceful shutdown, testing strategy), then deleted along with `prompts.md` (superseded build-prompt log). Swept the rest of the repo for leftover references the main phases missed: `ci.yml`, `Makefile`, `admin_service.proto`, `.dockerignore`, and two code comments pointing at the now-deleted `design-notes.md`.

**Files changed:** 3 commits, ~60 files. Full build (`cmake --build build --parallel`) succeeds; `ctest` passes everything except `db_unit_tests`, which fails only because no local Postgres is running (pre-existing, unrelated to this change — confirmed via `docker ps`). `terraform validate`/`fmt` pass against the cached AWS provider (no real AWS calls made — infra was already destroyed per the user). `docker compose config` validates.

**Not done / explicitly out of scope:** no `terraform plan`/`apply` against real AWS (nothing to compare against — infra already destroyed); no re-benchmark of the historical throughput/latency numbers in `docs/performance.md`/`docs/test-results.md` (left as dated records, per the user's decision to leave them untouched); the EC2 replacement is a new, never-deployed code path — worth a real `terraform apply` + smoke test before relying on it for a live deploy.

**Suggested next steps:** run `terraform plan` against a real AWS account to sanity-check the EC2/IAM/SG changes before merging to a branch anyone deploys from; consider adding a CloudWatch alarm or simple uptime check for the single EC2 host now that there's no HPA/PDB safety net; if the team ever needs true multi-instance `jq-server`/`jq-worker` scaling again, an ASG behind the existing `app` security group would be the natural next step rather than reintroducing Kubernetes.
