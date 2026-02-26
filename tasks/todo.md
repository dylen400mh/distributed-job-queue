# CI/CD Pipeline

## Plan

Two workflow files:

### `.github/workflows/ci.yml` — runs on every push and PR
- Trigger: `push` (all branches) + `pull_request`
- Job: `test` on `ubuntu-22.04`
  - Cache prometheus-cpp install + CMake build dir (keyed on CMakeLists hash)
  - Install apt build deps (matches Dockerfile builder stage)
  - Build prometheus-cpp from source (cached after first run)
  - `cmake -B build` + build all unit test targets
  - Run all 5 unit test binaries
  - Validate `docker compose config`

### `.github/workflows/deploy.yml` — runs on push to `main` only
- Trigger: `push` to `main`, after ci passes
- Auth: GitHub OIDC → AWS IAM role (no long-lived keys)
- Job 1: `build-and-push` (matrix: server, worker)
  - Login to ECR via `aws-actions/amazon-ecr-login`
  - Build Docker image with buildx + GitHub layer cache
  - Push two tags: `latest` and `sha-<7-char-SHA>`
- Job 2: `deploy` (depends on build-and-push)
  - Configure kubectl via `aws eks update-kubeconfig`
  - `kubectl apply -f k8s/` — applies namespace, configmap, services, HPA, PDB
  - `kubectl set image deployment/jq-server ...` with exact SHA tag
  - `kubectl set image deployment/jq-worker ...` with exact SHA tag
  - `kubectl rollout status` for both deployments

### Required GitHub configuration
- **Secret**: `AWS_ROLE_ARN` — IAM role ARN for OIDC federation
- **Variable**: `AWS_REGION`, `AWS_ACCOUNT_ID`, `EKS_CLUSTER_NAME`

## Todo

- [ ] 1. Create `.github/workflows/ci.yml`
- [ ] 2. Create `.github/workflows/deploy.yml`
- [ ] 3. Verify YAML is well-formed
- [ ] 4. Commit and push
