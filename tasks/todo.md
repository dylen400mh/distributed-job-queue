# Terraform CI/CD Workflow

## Goal

Add a GitHub Actions workflow that automates Terraform for infrastructure changes:
- `terraform validate` + `terraform fmt -check` on every PR touching `terraform/`
- `terraform plan` on PRs, posted as a PR comment
- `terraform apply` automatically on merge to `main`

---

## Todo

- [x] 1. Create `.github/workflows/terraform.yml`
- [x] 2. Commit and push

---

## Review

Created `.github/workflows/terraform.yml` with three jobs:

- **validate** — runs on all PRs and pushes: `terraform init` (backend via `-backend-config` flags), `fmt -check`, `validate`
- **plan** — runs on PRs only: full plan, output written to job summary and posted/updated as a PR comment via `github-script`
- **apply** — runs on push to `main` only: `terraform apply -auto-approve`

All three jobs use the `TF_ROLE_ARN` OIDC secret (separate from `AWS_ROLE_ARN` used by the deploy workflow — needs broader permissions).

Backend config is injected at `init` time via `-backend-config` flags so `backend.tf` stays free of hardcoded values.

---

## AWS Manual Setup Checklist

Before this workflow (or `terraform apply` locally) will work, complete these steps once:

### 1. S3 State Bucket
```bash
aws s3api create-bucket \
  --bucket YOUR_BUCKET_NAME \
  --region YOUR_REGION \
  --create-bucket-configuration LocationConstraint=YOUR_REGION

aws s3api put-bucket-versioning \
  --bucket YOUR_BUCKET_NAME \
  --versioning-configuration Status=Enabled

aws s3api put-public-access-block \
  --bucket YOUR_BUCKET_NAME \
  --public-access-block-configuration "BlockPublicAcls=true,IgnorePublicAcls=true,BlockPublicPolicy=true,RestrictPublicBuckets=true"
```

### 2. DynamoDB Lock Table
```bash
aws dynamodb create-table \
  --table-name terraform-locks \
  --attribute-definitions AttributeName=LockID,AttributeType=S \
  --key-schema AttributeName=LockID,KeyType=HASH \
  --billing-mode PAY_PER_REQUEST \
  --region YOUR_REGION
```

### 3. Terraform IAM Role (for GitHub Actions)
Create a role `github-terraform-jq` in the AWS console or CLI:
- **Trust policy**: same OIDC trust as `github-actions-jq` but for the Terraform workflow
- **Permissions**: `AdministratorAccess` (or a custom policy covering VPC, EKS, RDS, ElastiCache, MSK, IAM, ECR, Secrets Manager)
- Scope the sub condition to: `repo:dylen400mh/distributed-job-queue:ref:refs/heads/main`

### 4. GitHub Repository Config
| Type | Name | Value |
|---|---|---|
| Secret | `TF_ROLE_ARN` | ARN of the Terraform IAM role |
| Variable | `TF_BACKEND_BUCKET` | Name of your S3 state bucket |
| Variable | `TF_BACKEND_REGION` | Region of the state bucket |
| Variable | `AWS_REGION` | Already set — reused by this workflow |

### 5. backend.tf
The workflow injects backend config via `-backend-config` flags, so `backend.tf` only needs:
```hcl
terraform {
  backend "s3" {}
}
```
Or leave the placeholders — they are overridden at `init` time.
