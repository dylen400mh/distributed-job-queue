#include "ctl/commands/commands.h"

#include <iostream>
#include <string>

#include <nlohmann/json.hpp>

#include "admin_service.pb.h"
#include "common/config/flags.h"

namespace jq::ctl {

// ---------------------------------------------------------------------------
// system status
// ---------------------------------------------------------------------------

int RunSystemStatus(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    GetSystemStatusRequest req;
    auto resp = client.GetSystemStatus(req);

    if (fmt == OutputFormat::TABLE) {
        std::cout << "Overall:        " << (resp.healthy() ? "HEALTHY" : "UNHEALTHY") << '\n';
        std::cout << "Active workers: " << resp.active_workers() << '\n';
        std::cout << '\n';

        if (!resp.components().empty()) {
            std::vector<std::string> headers = {"COMPONENT", "HEALTHY", "MESSAGE"};
            std::vector<std::vector<std::string>> rows;
            for (const auto& c : resp.components()) {
                rows.push_back({c.name(), c.healthy() ? "yes" : "no", c.message()});
            }
            std::cout << FormatTable(headers, rows);
        }
    } else {
        nlohmann::json j;
        j["healthy"]        = resp.healthy();
        j["active_workers"] = resp.active_workers();

        nlohmann::json comps = nlohmann::json::array();
        for (const auto& c : resp.components()) {
            comps.push_back({
                {"name",    c.name()},
                {"healthy", c.healthy()},
                {"message", c.message()},
            });
        }
        j["components"] = comps;

        if (fmt == OutputFormat::JSON) std::cout << FormatJson(j);
        else                           std::cout << FormatYaml(j);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// version
// ---------------------------------------------------------------------------

int RunVersion(GrpcClient& client, OutputFormat fmt, int argc, char** argv) {
    jq::PrintVersion();
    return 0;
}

}  // namespace jq::ctl
