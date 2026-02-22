// src/ctl/main.cc
// jq-ctl: operator CLI tool.
// Placeholder — application logic will be added in subsequent prompts.

#include <iostream>

#include "common/config/config.h"
#include "common/config/flags.h"
#include "common/logging/logger.h"

int main(int argc, char** argv) {
    auto flags = jq::ParseCtlFlags(argc, argv);

    if (flags.version) { jq::PrintVersion(); return 0; }
    if (flags.help)    { jq::PrintCtlHelp(argv[0]); return 0; }

    // Config is optional for jq-ctl; load only if --config is provided.
    jq::Config cfg;
    if (!flags.config.empty()) {
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
    }

    jq::log::InitLogger("jq-ctl", flags.log_level);

    for (const auto& w : cfg.sensitive_field_warnings)
        LOG_WARN(w);

    LOG_INFO("jq-ctl ready", {{"server_addr", flags.server_addr}});

    return 0;
}
