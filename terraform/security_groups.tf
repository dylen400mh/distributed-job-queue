# ---------------------------------------------------------------------------
# Security Groups — least-privilege ingress for each tier
# ---------------------------------------------------------------------------

# ---------- App (single EC2 host running jq-server + jq-worker) ----------

resource "aws_security_group" "app" {
  name        = "${var.cluster_name}-app"
  description = "jq-server/jq-worker host traffic"
  vpc_id      = aws_vpc.main.id

  # gRPC, health, metrics — public like the old NLB-fronted access path.
  # No SSH: shell access is via SSM Session Manager only.
  ingress {
    from_port   = 50051
    to_port     = 50051
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  ingress {
    from_port   = 8080
    to_port     = 8080
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  ingress {
    from_port   = 9090
    to_port     = 9090
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-app-sg" }
}

# ---------- RDS PostgreSQL ----------

resource "aws_security_group" "rds" {
  name        = "${var.cluster_name}-rds"
  description = "RDS PostgreSQL - only from the app host"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port       = 5432
    to_port         = 5432
    protocol        = "tcp"
    security_groups = [aws_security_group.app.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-rds-sg" }
}

# ---------- ElastiCache Redis ----------

resource "aws_security_group" "redis" {
  name        = "${var.cluster_name}-redis"
  description = "ElastiCache Redis - only from the app host"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port       = 6379
    to_port         = 6379
    protocol        = "tcp"
    security_groups = [aws_security_group.app.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-redis-sg" }
}
