resource "aws_ecr_repository" "jq_server" {
  name                 = "jq-server"
  image_tag_mutability = "MUTABLE"

  image_scanning_configuration {
    scan_on_push = true
  }

  tags = { Name = "jq-server" }
}

resource "aws_ecr_repository" "jq_worker" {
  name                 = "jq-worker"
  image_tag_mutability = "MUTABLE"

  image_scanning_configuration {
    scan_on_push = true
  }

  tags = { Name = "jq-worker" }
}

# Keep the 10 most recent images; expire everything older.
resource "aws_ecr_lifecycle_policy" "jq_server" {
  repository = aws_ecr_repository.jq_server.name

  policy = jsonencode({
    rules = [{
      rulePriority = 1
      description  = "Keep last 10 images"
      selection = {
        tagStatus   = "any"
        countType   = "imageCountMoreThan"
        countNumber = 10
      }
      action = { type = "expire" }
    }]
  })
}

resource "aws_ecr_lifecycle_policy" "jq_worker" {
  repository = aws_ecr_repository.jq_worker.name

  policy = jsonencode({
    rules = [{
      rulePriority = 1
      description  = "Keep last 10 images"
      selection = {
        tagStatus   = "any"
        countType   = "imageCountMoreThan"
        countNumber = 10
      }
      action = { type = "expire" }
    }]
  })
}
