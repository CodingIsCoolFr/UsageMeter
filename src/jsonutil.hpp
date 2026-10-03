#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

// Read a number that may arrive as a number or a numeric string.
std::optional<double> json_number(const nlohmann::json& j, const char* key);

// First present, non-null value among the keys, or nullptr.
const nlohmann::json* json_first(const nlohmann::json& j, std::initializer_list<const char*> keys);

// "resets in 3h 35m" / "resets in 2d 4h" from an ISO-8601 instant. Empty if past or unparseable.
std::string resets_in(const std::string& iso8601);

// Local-time rendering of an ISO-8601 instant, e.g. "Thu 2:00 AM".
std::string format_local(const std::string& iso8601);
