# ---------------------------------------------------------------------------
# After `terraform apply`, run `terraform output` and copy these values into
# your GitHub repository settings:
#
#   Secrets  (Settings → Secrets and variables → Actions → Secrets):
#     AWS_ROLE_ARN  →  github_actions_role_arn
#
#   Variables  (Settings → Secrets and variables → Actions → Variables):
#     AWS_REGION        →  region
#     AWS_ACCOUNT_ID    →  aws_account_id
#     APP_INSTANCE_ID   →  app_instance_id
# ---------------------------------------------------------------------------

output "region" {
  description = "AWS region"
  value       = var.region
}

output "aws_account_id" {
  description = "AWS account ID"
  value       = data.aws_caller_identity.current.account_id
}

output "app_instance_id" {
  description = "EC2 instance ID running jq-server/jq-worker — set as GitHub Variable APP_INSTANCE_ID"
  value       = aws_instance.app.id
}

output "github_actions_role_arn" {
  description = "IAM role ARN for GitHub Actions OIDC — set as GitHub Secret AWS_ROLE_ARN"
  value       = aws_iam_role.github_actions.arn
}

output "ecr_server_url" {
  description = "ECR repository URL for jq-server"
  value       = aws_ecr_repository.jq_server.repository_url
}

output "ecr_worker_url" {
  description = "ECR repository URL for jq-worker"
  value       = aws_ecr_repository.jq_worker.repository_url
}

output "rds_endpoint" {
  description = "RDS PostgreSQL endpoint"
  value       = aws_db_instance.postgres.address
}

output "rds_port" {
  description = "RDS PostgreSQL port"
  value       = aws_db_instance.postgres.port
}

output "db_password_secret_arn" {
  description = "Secrets Manager ARN holding the DB password"
  value       = aws_secretsmanager_secret.db_password.arn
  sensitive   = true
}

output "redis_endpoint" {
  description = "ElastiCache Redis primary endpoint"
  value       = "${aws_elasticache_cluster.redis.cache_nodes[0].address}:${aws_elasticache_cluster.redis.cache_nodes[0].port}"
}
