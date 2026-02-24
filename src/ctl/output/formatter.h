#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace jq::ctl {

enum class OutputFormat { TABLE, JSON, YAML };

// Parse "table" | "json" | "yaml" → OutputFormat. Defaults to TABLE.
OutputFormat ParseFormat(const std::string& s);

// Render aligned columns. headers.size() == rows[i].size() for all i.
std::string FormatTable(const std::vector<std::string>&              headers,
                        const std::vector<std::vector<std::string>>& rows);

// Pretty-printed JSON.
std::string FormatJson(const nlohmann::json& j);

// Simple YAML (no external dep; handles flat objects and string arrays).
std::string FormatYaml(const nlohmann::json& j, int indent = 0);

}  // namespace jq::ctl
