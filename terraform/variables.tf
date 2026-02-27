variable "region" {
  description = "AWS region to deploy into"
  type        = string
  default     = "us-east-1"
}

variable "cluster_name" {
  description = "Name of the EKS cluster"
  type        = string
  default     = "jq-cluster"
}

variable "kubernetes_version" {
  description = "Kubernetes version for the EKS cluster"
  type        = string
  default     = "1.30"
}

# ---------- EKS node group ----------

variable "node_instance_type" {
  description = "EC2 instance type for EKS worker nodes"
  type        = string
  default     = "t3.medium"
}

variable "node_min" {
  description = "Minimum number of EKS nodes"
  type        = number
  default     = 2
}

variable "node_max" {
  description = "Maximum number of EKS nodes"
  type        = number
  default     = 5
}

variable "node_desired" {
  description = "Desired number of EKS nodes at launch"
  type        = number
  default     = 2
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

# ---------- MSK ----------

variable "kafka_broker_instance_type" {
  description = "MSK broker instance type"
  type        = string
  default     = "kafka.t3.small"
}

variable "kafka_broker_count" {
  description = "Number of MSK broker nodes (must equal number of AZs in private subnets)"
  type        = number
  default     = 2
}

variable "kafka_version" {
  description = "Apache Kafka version for MSK"
  type        = string
  default     = "3.6.0"
}

# ---------- GitHub OIDC ----------

variable "github_repo" {
  description = "GitHub repository in owner/repo format (used to scope the OIDC role)"
  type        = string
  default     = "dylen400mh/distributed-job-queue"
}
