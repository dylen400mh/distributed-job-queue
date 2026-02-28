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
// IKafkaProducer — minimal interface for mock injection in tests.
// ---------------------------------------------------------------------------
class IKafkaProducer {
public:
    virtual ~IKafkaProducer() = default;
    virtual void Publish(const std::string&         topic,
                         const std::string&         key,
                         const std::vector<uint8_t>& payload) = 0;
    virtual void Flush(int timeout_ms = 5000) = 0;
    // Returns true if the Kafka broker is reachable. Default returns true for mocks.
    virtual bool IsHealthy() { return true; }
};

// ---------------------------------------------------------------------------
// KafkaProducer — async producer wrapping librdkafka C++ API.
//
// Publish() is fire-and-forget (librdkafka buffers internally).
// The delivery report callback logs errors and increments
// jq_kafka_publish_errors_total.
// Call Flush() during graceful shutdown to drain the producer queue.
// ---------------------------------------------------------------------------
class KafkaProducer : public IKafkaProducer {
public:
    explicit KafkaProducer(const KafkaConfig& cfg);
    ~KafkaProducer();

    KafkaProducer(const KafkaProducer&)            = delete;
    KafkaProducer& operator=(const KafkaProducer&) = delete;

    // Asynchronously publish a message.
    // key and payload are copied into librdkafka's internal buffer.
    void Publish(const std::string&         topic,
                 const std::string&         key,
                 const std::vector<uint8_t>& payload) override;

    // Block until all queued messages are delivered or timeout_ms elapses.
    void Flush(int timeout_ms = 5000) override;

    // Probe broker connectivity via metadata fetch (2s timeout).
    bool IsHealthy() override;

private:
    std::unique_ptr<RdKafka::Producer> producer_;
};

}  // namespace jq
