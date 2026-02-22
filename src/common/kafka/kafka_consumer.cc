#include "common/kafka/kafka_consumer.h"

#include <stdexcept>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "common/config/config.h"
#include "common/logging/logger.h"

namespace jq {

// ---------------------------------------------------------------------------
// KafkaConsumer
// ---------------------------------------------------------------------------

KafkaConsumer::KafkaConsumer(const KafkaConfig&              cfg,
                              const std::string&              group_id,
                              const std::vector<std::string>& topics) {
    std::string errstr;

    auto conf = std::unique_ptr<RdKafka::Conf>(
        RdKafka::Conf::create(RdKafka::Conf::CONF_GLOBAL));

    // Broker list
    std::string brokers;
    for (size_t i = 0; i < cfg.brokers.size(); ++i) {
        if (i) brokers += ',';
        brokers += cfg.brokers[i];
    }
    if (conf->set("bootstrap.servers", brokers, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("Kafka consumer: bootstrap.servers: " + errstr);
    }

    if (conf->set("group.id", group_id, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("Kafka consumer: group.id: " + errstr);
    }

    // Auto-offset reset
    const std::string& aor = cfg.consumer.auto_offset_reset;
    if (!aor.empty()) {
        if (conf->set("auto.offset.reset", aor, errstr) != RdKafka::Conf::CONF_OK) {
            LOG_WARN("Kafka consumer: could not set auto.offset.reset",
                     {{"error", errstr}});
        }
    }

    // Disable auto-commit so callers control offset commits explicitly
    conf->set("enable.auto.commit", "false", errstr);

    // SASL
    if (!cfg.sasl.username.empty()) {
        conf->set("security.protocol", "SASL_PLAINTEXT", errstr);
        conf->set("sasl.mechanism",    "PLAIN",           errstr);
        conf->set("sasl.username",     cfg.sasl.username, errstr);
        conf->set("sasl.password",     cfg.sasl.password, errstr);
    }

    consumer_.reset(RdKafka::KafkaConsumer::create(conf.get(), errstr));
    if (!consumer_) {
        throw std::runtime_error("Failed to create Kafka consumer: " + errstr);
    }

    RdKafka::ErrorCode err = consumer_->subscribe(topics);
    if (err != RdKafka::ERR_NO_ERROR) {
        throw std::runtime_error("Kafka consumer subscribe failed: " +
                                  RdKafka::err2str(err));
    }

    LOG_INFO("Kafka consumer ready",
             {{"brokers", brokers}, {"group_id", group_id}});
}

KafkaConsumer::~KafkaConsumer() {
    Close();
}

std::optional<KafkaMessage> KafkaConsumer::Poll(int timeout_ms) {
    if (!consumer_ || closed_) return std::nullopt;

    RdKafka::Message* msg = consumer_->consume(timeout_ms);
    if (!msg) return std::nullopt;

    auto err = msg->err();

    if (err == RdKafka::ERR__TIMED_OUT || err == RdKafka::ERR__PARTITION_EOF) {
        delete msg;
        return std::nullopt;
    }

    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_WARN("Kafka poll error", {{"error", msg->errstr()}});
        delete msg;
        return std::nullopt;
    }

    KafkaMessage out;
    out.topic     = msg->topic_name();
    out.partition = msg->partition();
    out.offset    = msg->offset();
    if (msg->key()) out.key = *msg->key();

    const uint8_t* data = static_cast<const uint8_t*>(msg->payload());
    out.payload.assign(data, data + msg->len());

    // Store raw pointer for Commit — consumer owns this message until committed
    // or until the next Poll call, whichever comes first.
    out.raw = msg;

    return out;
}

void KafkaConsumer::Commit(const KafkaMessage& msg) {
    if (!consumer_ || closed_ || !msg.raw) return;

    RdKafka::ErrorCode err = consumer_->commitAsync(msg.raw);
    delete msg.raw;
    // msg.raw is now dangling; callers must not use it after Commit

    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_WARN("Kafka commit failed",
                 {{"topic",  msg.topic},
                  {"offset", msg.offset},
                  {"error",  RdKafka::err2str(err)}});
    }
}

void KafkaConsumer::Close() {
    if (!consumer_ || closed_) return;
    closed_ = true;
    consumer_->close();
    LOG_INFO("Kafka consumer closed");
}

}  // namespace jq
