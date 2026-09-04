# ---------------------------------------------------------------------------
# Single EC2 host running jq-server + jq-worker via Docker Compose.
# Public subnet, public IP, gRPC/health/metrics exposed directly (no load
# balancer). Access for operators is via SSM Session Manager — no SSH key,
# no open port 22.
# ---------------------------------------------------------------------------

data "aws_ami" "al2023" {
  most_recent = true
  owners      = ["amazon"]

  filter {
    name   = "name"
    values = ["al2023-ami-*-x86_64"]
  }
}

resource "aws_instance" "app" {
  ami                         = data.aws_ami.al2023.id
  instance_type               = var.app_instance_type
  subnet_id                   = aws_subnet.public[0].id
  vpc_security_group_ids      = [aws_security_group.app.id]
  iam_instance_profile        = aws_iam_instance_profile.app.name
  associate_public_ip_address = true

  # Perf-test workloads can burst well past t3's baseline CPU; "unlimited"
  # bursts past the credit balance for a small per-vCPU-hour surcharge
  # instead of throttling to baseline once credits run out.
  credit_specification {
    cpu_credits = "unlimited"
  }

  user_data = templatefile("${path.module}/templates/app_user_data.sh.tftpl", {
    region         = var.region
    ecr_server_url = aws_ecr_repository.jq_server.repository_url
    ecr_worker_url = aws_ecr_repository.jq_worker.repository_url
    db_host        = aws_db_instance.postgres.address
    db_port        = aws_db_instance.postgres.port
    db_name        = var.db_name
    db_user        = var.db_username
    db_secret_arn  = aws_secretsmanager_secret.db_password.arn
    redis_addr     = "${aws_elasticache_cluster.redis.cache_nodes[0].address}:${aws_elasticache_cluster.redis.cache_nodes[0].port}"
  })

  tags = { Name = "${var.cluster_name}-app" }
}
