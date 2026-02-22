# Prompt 5 — Redis, Kafka, and Metrics Clients

## Notes

- `redis-plus-plus` is not available via Homebrew; using raw `hiredis` C API directly.
- `librdkafka` C++ API (`rdkafkacpp.h`) is available at `/opt/homebrew/include/librdkafka/`.
- `prometheus-cpp` is available at `/opt/homebrew/include/prometheus/`.
- Metrics are defined in FR-040; all 11 metrics must be registered as global singletons.

## Todo

- [x] 1. Write `src/common/metrics/metrics.h` — declare all FR-040 metrics + `StartMetricsServer(port)`
- [x] 2. Write `src/common/metrics/metrics.cc` — register metrics, expose via prometheus-cpp Exposer
- [x] 3. Write `src/common/redis/redis_client.h` — RedisClient interface + DistributedLock class
- [x] 4. Write `src/common/redis/redis_client.cc` — hiredis-backed implementation; metrics instrumentation
- [x] 5. Write `src/common/kafka/kafka_producer.h` — KafkaProducer interface + KafkaTopics constants
- [x] 6. Write `src/common/kafka/kafka_producer.cc` — librdkafka producer with delivery report callback
- [x] 7. Write `src/common/kafka/kafka_consumer.h` — KafkaConsumer interface + Message struct
- [x] 8. Write `src/common/kafka/kafka_consumer.cc` — librdkafka consumer implementation
- [x] 9. Write `tests/unit/redis/distributed_lock_test.cc` — RAII behavior with mock Redis
- [x] 10. Write `tests/unit/kafka/kafka_delivery_test.cc` — delivery report failure path
- [x] 11. Update `CMakeLists.txt` — add jq_metrics, jq_redis, jq_kafka static libs; new test targets
- [x] 12. Verify full build compiles cleanly
- [x] 13. Append to docs/activity.md and push to git

## Review

### Summary of Changes

**New libraries:**
- `jq_metrics` — registers all 11 FR-040 metrics as static singletons via `prometheus-cpp`; `StartMetricsServer(port)` starts the HTTP `/metrics` exposer thread
- `jq_redis` — `RedisClient` wraps raw `hiredis` C API (redis-plus-plus unavailable on Homebrew); `IRedisLockable` interface enables mock injection; `DistributedLock` RAII acquires `lock:job:<key>` on construction, releases on destruction; every operation observed by `jq_redis_operation_duration_seconds`
- `jq_kafka` — `KafkaProducer` wraps librdkafka C++ API with async `Publish`, `Flush`, and a `DeliveryReportCb` that logs errors and increments `jq_kafka_publish_errors_total`; `KafkaConsumer` wraps the high-level consumer with manual offset commit; `KafkaTopics` namespace provides six topic-name constants

**New tests (9 tests, all pass without external services):**
- `redis_unit_tests` (5 tests): `DistributedLock` acquires/releases, throws on contention, correct key prefix, Del called exactly once, default TTL = 60 000 ms — using `gmock` mock of `IRedisLockable`
- `kafka_unit_tests` (4 tests): delivery-failure counter incremented per-topic, independent counter instances per topic, all six topics handled without exception, success path does not create phantom counters

**Build:** Full clean build, zero errors; 17/17 unit tests pass.
