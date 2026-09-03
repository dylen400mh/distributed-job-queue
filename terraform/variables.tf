variable "region" {
  description = "AWS region to deploy into"
  type        = string
  default     = "us-east-1"
}

variable "cluster_name" {
  description = "Name prefix for all provisioned resources"
  type        = string
  default     = "jq-cluster"
}

# ---------- App EC2 host ----------

variable "app_instance_type" {
  description = "EC2 instance type for the single app host running jq-server + jq-worker via Docker Compose"
  type        = string
  default     = "t3.small"
}

# ---------- RDS ----------

variable "db_instance_class" {
  description = "RDS instance class"
  type        = string
  default     = "db.t3.medium"
}

variable "db_allocated_storage" {
  description = "Initial storage for RDS instance (GB)"
  type        = number
  default     = 20
}

variable "db_name" {
  description = "PostgreSQL database name"
  type        = string
  default     = "jobqueue"
}

variable "db_username" {
  description = "PostgreSQL admin username"
  type        = string
  default     = "jq"
}

# ---------- ElastiCache ----------

variable "redis_node_type" {
  description = "ElastiCache Redis node type"
  type        = string
  default     = "cache.t3.micro"
}

# ---------- GitHub OIDC ----------

variable "github_repo" {
  description = "GitHub repository in owner/repo format (used to scope the OIDC role)"
  type        = string
  default     = "dylen400mh/distributed-job-queue"
}
