resource "aws_msk_cluster" "kafka" {
  cluster_name           = "${var.cluster_name}-kafka"
  kafka_version          = var.kafka_version
  number_of_broker_nodes = var.kafka_broker_count

  broker_node_group_info {
    instance_type  = var.kafka_broker_instance_type
    client_subnets = slice(aws_subnet.private[*].id, 0, var.kafka_broker_count)

    storage_info {
      ebs_storage_info {
        volume_size = 100
      }
    }

    security_groups = [aws_security_group.msk.id]
  }

  # Plaintext within the VPC; TLS available for future hardening
  client_authentication {
    unauthenticated = true
  }

  encryption_info {
    encryption_in_transit {
      client_broker = "PLAINTEXT"
      in_cluster    = true
    }
  }

  # Broker-level metrics for CloudWatch
  open_monitoring {
    prometheus {
      jmx_exporter {
        enabled_in_broker = true
      }
      node_exporter {
        enabled_in_broker = true
      }
    }
  }

  tags = { Name = "${var.cluster_name}-kafka" }
}
