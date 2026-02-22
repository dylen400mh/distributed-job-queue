#pragma once

#include <memory>
#include <string>
#include <vector>

namespace RdKafka {
class Producer;
class Conf;
}

namespace jq {

struct KafkaConfig;  // from common/config/config.h

// ---------------------------------------------------------------------------
// Topic name constants — used by callers so topic strings are not scattered.
// ---------------------------------------------------------------------------
namespace KafkaTopics {
inline constexpr const char* kJobSubmitted   = "job.submitted";
inline constexpr const char* kJobStarted     = "job.started";
inline constexpr const char* kJobCompleted   = "job.completed";
inline constexpr const char* kJobFailed      = "job.failed";
inline constexpr const char* kJobDeadLettered = "job.dead-lettered";
inline constexpr const char* kWorkerHeartbeat = "worker.heartbeat";
}  // namespace KafkaTopics

// ---------------------------------------------------------------------------
// KafkaProducer — async producer wrapping librdkafka C++ API.
//
// Publish() is fire-and-forget (librdkafka buffers internally).
// The delivery report callback logs errors and increments
// jq_kafka_publish_errors_total.
// Call Flush() during graceful shutdown to drain the producer queue.
// ---------------------------------------------------------------------------
class KafkaProducer {
public:
    explicit KafkaProducer(const KafkaConfig& cfg);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&)            = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // Asynchronously publish a message.
    // key and payload are copied into librdkafka's internal buffer.
    void Publish(const std::string&         topic,
                 const std::string&         key,
                 const std::vector<uint8_t>& payload);

    // Block until all queued messages are delivered or timeout_ms elapses.
    void Flush(int timeout_ms = 5000);

private:
    std::unique_ptr<RdKafka::Producer> producer_;
};

}  // namespace jq
