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
#     EKS_CLUSTER_NAME  →  eks_cluster_name
#
# Then update k8s/secret.yaml with the real endpoint values below.
# ---------------------------------------------------------------------------

output "region" {
  description = "AWS region"
  value       = var.region
}

output "aws_account_id" {
  description = "AWS account ID"
  value       = data.aws_caller_identity.current.account_id
}

output "eks_cluster_name" {
  description = "EKS cluster name — set as GitHub Variable EKS_CLUSTER_NAME"
  value       = aws_eks_cluster.main.name
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
  description = "RDS PostgreSQL endpoint — use in k8s/secret.yaml db_host"
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
  description = "ElastiCache Redis primary endpoint — use in k8s/secret.yaml redis_addr"
  value       = "${aws_elasticache_cluster.redis.cache_nodes[0].address}:${aws_elasticache_cluster.redis.cache_nodes[0].port}"
}

output "msk_bootstrap_brokers" {
  description = "MSK plaintext bootstrap broker string — use in k8s/secret.yaml kafka_brokers"
  value       = aws_msk_cluster.kafka.bootstrap_brokers
}
