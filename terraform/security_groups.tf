# ---------------------------------------------------------------------------
# Security Groups — least-privilege ingress for each tier
# ---------------------------------------------------------------------------

# ---------- EKS Nodes ----------

resource "aws_security_group" "eks_nodes" {
  name        = "${var.cluster_name}-nodes"
  description = "EKS worker node traffic"
  vpc_id      = aws_vpc.main.id

  # Nodes talk to each other (pod-to-pod, kubelet)
  ingress {
    from_port = 0
    to_port   = 0
    protocol  = "-1"
    self      = true
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-nodes-sg" }
}

# ---------- EKS Control Plane ----------

resource "aws_security_group" "eks_control_plane" {
  name        = "${var.cluster_name}-control-plane"
  description = "EKS control plane to node communication"
  vpc_id      = aws_vpc.main.id

  tags = { Name = "${var.cluster_name}-control-plane-sg" }
}

# Cross-references as separate rules to break the circular dependency.

# Control plane → nodes (kubelet, webhook admission)
resource "aws_security_group_rule" "cp_to_nodes_ephemeral" {
  type                     = "egress"
  from_port                = 1025
  to_port                  = 65535
  protocol                 = "tcp"
  security_group_id        = aws_security_group.eks_control_plane.id
  source_security_group_id = aws_security_group.eks_nodes.id
}

resource "aws_security_group_rule" "cp_to_nodes_443" {
  type                     = "egress"
  from_port                = 443
  to_port                  = 443
  protocol                 = "tcp"
  security_group_id        = aws_security_group.eks_control_plane.id
  source_security_group_id = aws_security_group.eks_nodes.id
}

# Nodes ← control plane (inbound)
resource "aws_security_group_rule" "nodes_from_cp_443" {
  type                     = "ingress"
  from_port                = 443
  to_port                  = 443
  protocol                 = "tcp"
  security_group_id        = aws_security_group.eks_nodes.id
  source_security_group_id = aws_security_group.eks_control_plane.id
}

resource "aws_security_group_rule" "nodes_from_cp_ephemeral" {
  type                     = "ingress"
  from_port                = 1025
  to_port                  = 65535
  protocol                 = "tcp"
  security_group_id        = aws_security_group.eks_nodes.id
  source_security_group_id = aws_security_group.eks_control_plane.id
}

# ---------- RDS PostgreSQL ----------

resource "aws_security_group" "rds" {
  name        = "${var.cluster_name}-rds"
  description = "RDS PostgreSQL — only from EKS nodes"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port       = 5432
    to_port         = 5432
    protocol        = "tcp"
    security_groups = [aws_security_group.eks_nodes.id]
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
  description = "ElastiCache Redis — only from EKS nodes"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port       = 6379
    to_port         = 6379
    protocol        = "tcp"
    security_groups = [aws_security_group.eks_nodes.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-redis-sg" }
}

# ---------- MSK Kafka ----------

resource "aws_security_group" "msk" {
  name        = "${var.cluster_name}-msk"
  description = "MSK Kafka — only from EKS nodes"
  vpc_id      = aws_vpc.main.id

  ingress {
    from_port       = 9092
    to_port         = 9092
    protocol        = "tcp"
    security_groups = [aws_security_group.eks_nodes.id]
  }

  # TLS port
  ingress {
    from_port       = 9094
    to_port         = 9094
    protocol        = "tcp"
    security_groups = [aws_security_group.eks_nodes.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = { Name = "${var.cluster_name}-msk-sg" }
}
