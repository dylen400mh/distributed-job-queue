.PHONY: build test-unit test-integration test-e2e services services-down

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------

build:
	cmake --build build --parallel

# ---------------------------------------------------------------------------
# Unit tests (no external services required)
# ---------------------------------------------------------------------------

test-unit: build
	./build/tests/config_unit_tests
	./build/tests/kafka_unit_tests
	./build/tests/redis_unit_tests
	./build/tests/scheduler_unit_tests
	./build/tests/server_unit_tests

# ---------------------------------------------------------------------------
# Integration tests
# Requires jq-server running at localhost:50051.
# Start backing services first: make services
# Then start jq-server:  ./build/jq-server --config config.local.yaml
# Then start jq-worker:  ./build/jq-worker --config config.local.yaml --server-addr localhost:50051
# ---------------------------------------------------------------------------

test-integration: build
	JQ_TEST_SERVER_ADDR=localhost:50051 ./build/tests/integration_tests

# ---------------------------------------------------------------------------
# End-to-end tests
# Starts backing services, jq-server, and jq-worker automatically,
# runs integration tests, then tears everything down.
# ---------------------------------------------------------------------------

test-e2e: build services
	@echo "Waiting for services to be healthy..."
	@sleep 5
	@./build/jq-server --config config.local.yaml & echo $$! > /tmp/jq-server.pid
	@sleep 3
	@./build/jq-worker --config config.local.yaml --server-addr localhost:50051 & echo $$! > /tmp/jq-worker.pid
	@sleep 2
	@JQ_TEST_SERVER_ADDR=localhost:50051 ./build/tests/integration_tests; \
	  EXIT=$$?; \
	  kill $$(cat /tmp/jq-server.pid) $$(cat /tmp/jq-worker.pid) 2>/dev/null; \
	  rm -f /tmp/jq-server.pid /tmp/jq-worker.pid; \
	  exit $$EXIT

# ---------------------------------------------------------------------------
# Docker Compose helpers
# ---------------------------------------------------------------------------

services:
	docker compose up -d --wait postgres redis redpanda

services-down:
	docker compose down
