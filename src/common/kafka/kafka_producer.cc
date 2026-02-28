#include "common/kafka/kafka_producer.h"

#include <cstring>
#include <string>
#include <vector>

#include <librdkafka/rdkafkacpp.h>

#include "common/config/config.h"
#include "common/logging/logger.h"
#include "common/metrics/metrics.h"

namespace jq {

namespace {

// ---------------------------------------------------------------------------
// Delivery report callback — called by librdkafka for each produced message.
// ---------------------------------------------------------------------------
class DeliveryReportCb : public RdKafka::DeliveryReportCb {
public:
    void dr_cb(RdKafka::Message& msg) override {
        if (msg.err() == RdKafka::ERR_NO_ERROR) {
            LOG_DEBUG("Kafka message delivered",
                      {{"topic",     msg.topic_name()},
                       {"partition", msg.partition()},
                       {"offset",    msg.offset()}});
        } else {
            LOG_ERROR("Kafka delivery failure",
                      {{"topic", msg.topic_name()},
                       {"key",   msg.key() ? *msg.key() : ""},
                       {"error", msg.errstr()}});
            // Increment per-topic error counter
            metrics::KafkaPublishErrorsTotal()
                .Add({{"topic", msg.topic_name()}})
                .Increment();
        }
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// KafkaProducer
// ---------------------------------------------------------------------------

KafkaProducer::KafkaProducer(const KafkaConfig& cfg) {
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
        throw std::runtime_error("Kafka producer: bootstrap.servers: " + errstr);
    }

    // Acks
    if (!cfg.producer.acks.empty()) {
        if (conf->set("acks", cfg.producer.acks, errstr) != RdKafka::Conf::CONF_OK) {
            LOG_WARN("Kafka producer: could not set acks", {{"error", errstr}});
        }
    }

    // Compression
    if (!cfg.producer.compression.empty()) {
        if (conf->set("compression.codec", cfg.producer.compression, errstr)
                != RdKafka::Conf::CONF_OK) {
            LOG_WARN("Kafka producer: could not set compression",
                     {{"error", errstr}});
        }
    }

    // SASL if credentials are present
    if (!cfg.sasl.username.empty()) {
        conf->set("security.protocol", "SASL_PLAINTEXT", errstr);
        conf->set("sasl.mechanism",    "PLAIN",           errstr);
        conf->set("sasl.username",     cfg.sasl.username, errstr);
        conf->set("sasl.password",     cfg.sasl.password, errstr);
    }

    // Register delivery report callback (kept alive by static storage in dr_cb)
    static DeliveryReportCb dr_cb;
    if (conf->set("dr_cb", &dr_cb, errstr) != RdKafka::Conf::CONF_OK) {
        throw std::runtime_error("Kafka producer: dr_cb: " + errstr);
    }

    producer_.reset(RdKafka::Producer::create(conf.get(), errstr));
    if (!producer_) {
        throw std::runtime_error("Failed to create Kafka producer: " + errstr);
    }

    LOG_INFO("Kafka producer ready", {{"brokers", brokers}});
}

KafkaProducer::~KafkaProducer() {
    Flush(5000);
}

void KafkaProducer::Publish(const std::string&         topic,
                             const std::string&         key,
                             const std::vector<uint8_t>& payload) {
    if (!producer_) return;

    // Poll to serve delivery reports while we produce
    producer_->poll(0);

    RdKafka::ErrorCode err = producer_->produce(
        topic,
        RdKafka::Topic::PARTITION_UA,          // auto partition
        RdKafka::Producer::RK_MSG_COPY,        // copy payload
        const_cast<uint8_t*>(payload.data()),
        payload.size(),
        key.empty() ? nullptr : key.c_str(),
        key.size(),
        0,     // timestamp (0 = now)
        nullptr);

    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_ERROR("Kafka produce error",
                  {{"topic", topic}, {"error", RdKafka::err2str(err)}});
        metrics::KafkaPublishErrorsTotal()
            .Add({{"topic", topic}})
            .Increment();
    }
}

void KafkaProducer::Flush(int timeout_ms) {
    if (!producer_) return;
    RdKafka::ErrorCode err = producer_->flush(timeout_ms);
    if (err != RdKafka::ERR_NO_ERROR) {
        LOG_WARN("Kafka flush timed out",
                 {{"timeout_ms", timeout_ms},
                  {"error",      RdKafka::err2str(err)}});
    }
}

bool KafkaProducer::IsHealthy() {
    if (!producer_) return false;
    RdKafka::Metadata* md = nullptr;
    RdKafka::ErrorCode err = producer_->metadata(false, nullptr, &md, 2000);
    delete md;
    return err == RdKafka::ERR_NO_ERROR;
}

}  // namespace jq
