#include "ctl/output/formatter.h"

#include <algorithm>
#include <string>
#include <vector>

namespace jq::ctl {

// ---------------------------------------------------------------------------
// ParseFormat
// ---------------------------------------------------------------------------

OutputFormat ParseFormat(const std::string& s) {
    if (s == "json") return OutputFormat::JSON;
    if (s == "yaml") return OutputFormat::YAML;
    return OutputFormat::TABLE;
}

// ---------------------------------------------------------------------------
// FormatTable
// ---------------------------------------------------------------------------

std::string FormatTable(const std::vector<std::string>&              headers,
                        const std::vector<std::vector<std::string>>& rows) {
    if (headers.empty()) return "";

    // Compute column widths.
    std::vector<std::size_t> widths(headers.size(), 0);
    for (std::size_t c = 0; c < headers.size(); ++c) {
        widths[c] = headers[c].size();
    }
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < std::min(row.size(), widths.size()); ++c) {
            widths[c] = std::max(widths[c], row[c].size());
        }
    }

    std::string out;
    // Header row.
    for (std::size_t c = 0; c < headers.size(); ++c) {
        if (c) out += "  ";
        out += headers[c];
        out += std::string(widths[c] - headers[c].size(), ' ');
    }
    out += '\n';

    // Separator.
    for (std::size_t c = 0; c < headers.size(); ++c) {
        if (c) out += "  ";
        out += std::string(widths[c], '-');
    }
    out += '\n';

    // Data rows.
    for (const auto& row : rows) {
        for (std::size_t c = 0; c < headers.size(); ++c) {
            if (c) out += "  ";
            const std::string& cell = (c < row.size()) ? row[c] : "";
            out += cell;
            out += std::string(widths[c] - cell.size(), ' ');
        }
        out += '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------
// FormatJson
// ---------------------------------------------------------------------------

std::string FormatJson(const nlohmann::json& j) {
    return j.dump(2) + '\n';
}

// ---------------------------------------------------------------------------
// FormatYaml — simple recursive YAML serialiser (no external dep).
// Handles: objects, arrays of primitives/objects, primitives.
// ---------------------------------------------------------------------------

std::string FormatYaml(const nlohmann::json& j, int indent) {
    const std::string pad(static_cast<std::size_t>(indent * 2), ' ');
    std::string out;

    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            out += pad + it.key() + ":";
            const auto& v = it.value();
            if (v.is_object() || (v.is_array() && !v.empty() && v[0].is_object())) {
                out += "\n" + FormatYaml(v, indent + 1);
            } else if (v.is_array()) {
                out += "\n";
                for (const auto& elem : v) {
                    out += pad + "  - ";
                    if (elem.is_string()) out += elem.get<std::string>();
                    else out += elem.dump();
                    out += "\n";
                }
            } else if (v.is_string()) {
                out += " " + v.get<std::string>() + "\n";
            } else if (v.is_null()) {
                out += " null\n";
            } else {
                out += " " + v.dump() + "\n";
            }
        }
    } else if (j.is_array()) {
        for (const auto& elem : j) {
            if (elem.is_object()) {
                out += pad + "-\n" + FormatYaml(elem, indent + 1);
            } else {
                out += pad + "- " + (elem.is_string() ? elem.get<std::string>() : elem.dump()) + "\n";
            }
        }
    } else if (j.is_string()) {
        out += pad + j.get<std::string>() + "\n";
    } else {
        out += pad + j.dump() + "\n";
    }
    return out;
}

}  // namespace jq::ctl
