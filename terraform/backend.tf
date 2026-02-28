# Remote state stored in S3 with DynamoDB locking.
#
# One-time bootstrap (run once before `terraform init`):
#   aws s3api create-bucket --bucket <YOUR_BUCKET> --region <YOUR_REGION> \
#     --create-bucket-configuration LocationConstraint=<YOUR_REGION>
#   aws s3api put-bucket-versioning --bucket <YOUR_BUCKET> \
#     --versioning-configuration Status=Enabled
#   aws dynamodb create-table --table-name terraform-locks \
#     --attribute-definitions AttributeName=LockID,AttributeType=S \
#     --key-schema AttributeName=LockID,KeyType=HASH \
#     --billing-mode PAY_PER_REQUEST
#
# Then replace the placeholder values below and run `terraform init`.

terraform {
  backend "s3" {
    bucket         = "jq-terraform-state-865091755721"
    key            = "distributed-job-queue/terraform.tfstate"
    region         = "us-east-1"
    dynamodb_table = "terraform-locks"
    encrypt        = true
  }
}
