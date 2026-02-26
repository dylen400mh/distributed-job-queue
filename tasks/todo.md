# Next Step: Observability & Local Dev Completeness

## Status Assessment

All 8 build prompts are complete. The system builds, runs, and passes end-to-end tests.
The last work added Kubernetes manifests. Three gaps remain:

### Remaining gaps

| Area | Gap | Priority |
|---|---|---|
| **Prometheus** | No alert rules file — `prometheus.yml` has scrape config but no alerts | High |
| **Grafana** | No dashboard JSON — only the datasource provisioning exists | High |
| **K8s** | `k8s/worker/pdb.yaml` missing (worker has HPA but no PDB) | Medium |
| **Docker Compose** | `jq-server` and `jq-worker` containers not in `docker-compose.yml` | Low* |

*docker-compose local dev works fine running binaries on the host; adding containers is optional.

---

## Proposed Plan

### Step 1 — Prometheus Alert Rules
Create `prometheus/alerts.yaml` with rules covering:
- High queue depth (>1000 PENDING for >2m)
- Worker count zero (no workers online)
- High job failure rate (>10% over 5m)
- Scheduler cycle slow (p95 >200ms)
- Kafka publish errors spiking
- Redis operation errors

Update `prometheus/prometheus.yml` to load the rules file (`rule_files` section).

### Step 2 — Grafana Dashboard
Create `grafana/provisioning/dashboards/jq-dashboard.json` and the provisioning sider config at `grafana/provisioning/dashboards/dashboard.yml`.

Dashboard panels:
- Job queue depth by queue/status (Gauge)
- Job throughput (jobs/sec submitted, completed, failed — Counter rate)
- End-to-end processing latency histogram (p50/p95/p99)
- Active workers and per-worker concurrency
- Scheduler cycle duration histogram
- Kafka publish error rate
- Redis operation duration
- gRPC request duration by method

Update `docker-compose.yml` to mount the provisioning directories so Grafana auto-loads everything on startup.

### Step 3 — Worker PDB
Create `k8s/worker/pdb.yaml` — `minAvailable: 1` so node drains don't take all workers offline simultaneously.

---

## Todo

- [ ] 1. Create `prometheus/alerts.yaml` with 6 alert rules
- [ ] 2. Update `prometheus/prometheus.yml` to reference the rules file
- [ ] 3. Create `grafana/provisioning/dashboards/dashboard.yml` provisioning config
- [ ] 4. Create `grafana/provisioning/dashboards/jq-dashboard.json`
- [ ] 5. Update `docker-compose.yml` to mount grafana provisioning dirs
- [ ] 6. Create `k8s/worker/pdb.yaml`
- [ ] 7. Verify `docker compose config --quiet` still passes
- [ ] 8. Commit and push
