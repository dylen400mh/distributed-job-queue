// src/worker/main.cc
// jq-worker: job executor daemon.
// Placeholder — application logic will be added in subsequent prompts.

#include <iostream>

#include "common/config/config.h"
#include "common/config/flags.h"
#include "common/logging/logger.h"

int main(int argc, char** argv) {
    auto flags = jq::ParseWorkerFlags(argc, argv);

    if (flags.version) { jq::PrintVersion(); return 0; }
    if (flags.help)    { jq::PrintWorkerHelp(argv[0]); return 0; }

    if (flags.config.empty()) {
        std::cerr << "Error: --config is required\n";
        jq::PrintWorkerHelp(argv[0]);
        return 1;
    }

    if (flags.server_addr.empty()) {
        std::cerr << "Error: --server-addr is required\n";
        jq::PrintWorkerHelp(argv[0]);
        return 1;
    }

    jq::Config cfg;
    try {
        cfg = jq::LoadConfig(flags.config);
    } catch (const std::exception& e) {
        std::cerr << "Config error: " << e.what() << '\n';
        return 1;
    }

    const auto errors = jq::ValidateConfig(cfg);
    if (!errors.empty()) {
        for (const auto& err : errors) std::cerr << "Config error: " << err << '\n';
        return 1;
    }

    // Flag overrides
    if (flags.metrics_port > 0) cfg.metrics.port = flags.metrics_port;
    if (flags.health_port  > 0) cfg.health.port  = flags.health_port;

    jq::log::InitLogger("jq-worker", flags.log_level);

    for (const auto& w : cfg.sensitive_field_warnings)
        LOG_WARN(w);

    LOG_INFO("jq-worker starting", {{"server_addr", flags.server_addr}});

    return 0;
}
