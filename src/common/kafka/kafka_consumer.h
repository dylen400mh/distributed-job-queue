#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace RdKafka {
class KafkaConsumer;
class Message;
}

namespace jq {

struct KafkaConfig;  // from common/config/config.h

// ---------------------------------------------------------------------------
// Message — value type returned by KafkaConsumer::Poll.
// Holds a copy of the message data so the caller doesn't need to manage
// librdkafka message lifetimes.
// ---------------------------------------------------------------------------
struct KafkaMessage {
    std::string         topic;
    int32_t             partition = 0;
    int64_t             offset    = 0;
    std::string         key;
    std::vector<uint8_t> payload;

    // Opaque handle used by KafkaConsumer::Commit — do not inspect directly.
    RdKafka::Message* raw = nullptr;
};

// ---------------------------------------------------------------------------
// KafkaConsumer — wraps librdkafka's high-level consumer.
// ---------------------------------------------------------------------------
class KafkaConsumer {
public:
    KafkaConsumer(const KafkaConfig&              cfg,
                  const std::string&              group_id,
                  const std::vector<std::string>& topics);
    ~KafkaConsumer();

    KafkaConsumer(const KafkaConsumer&)            = delete;
    KafkaConsumer& operator=(const KafkaConsumer&) = delete;

    // Poll for one message; returns nullopt on timeout or transient error.
    // timeout_ms == 0 is non-blocking.
    std::optional<KafkaMessage> Poll(int timeout_ms = 100);

    // Commit offsets for the given message (async store + async commit).
    void Commit(const KafkaMessage& msg);

    // Unsubscribe and close the consumer.
    void Close();

private:
    std::unique_ptr<RdKafka::KafkaConsumer> consumer_;
    bool closed_ = false;
};

}  // namespace jq
