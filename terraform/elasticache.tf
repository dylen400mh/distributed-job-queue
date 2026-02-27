resource "aws_elasticache_subnet_group" "redis" {
  name       = "${var.cluster_name}-redis"
  subnet_ids = aws_subnet.private[*].id
  tags       = { Name = "${var.cluster_name}-redis-subnet-group" }
}

resource "aws_elasticache_cluster" "redis" {
  cluster_id           = "${var.cluster_name}-redis"
  engine               = "redis"
  engine_version       = "7.1"
  node_type            = var.redis_node_type
  num_cache_nodes      = 1
  parameter_group_name = "default.redis7"
  port                 = 6379

  subnet_group_name  = aws_elasticache_subnet_group.redis.name
  security_group_ids = [aws_security_group.redis.id]

  # Automatic minor version upgrades during the maintenance window
  auto_minor_version_upgrade = true
  maintenance_window         = "sun:05:00-sun:06:00"

  tags = { Name = "${var.cluster_name}-redis" }
}
