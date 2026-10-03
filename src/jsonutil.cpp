#include "jsonutil.hpp"

#include <cctype>
#include <ctime>

std::optional<double> json_number(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || j[key].is_null()) return std::nullopt;
    const auto& v = j[key];
    if (v.is_number()) return v.get<double>();
    if (v.is_string()) {
        try {
            return std::stod(v.get<std::string>());
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

const nlohmann::json* json_first(const nlohmann::json& j, std::initializer_list<const char*> keys) {
    if (!j.is_object()) return nullptr;
    for (const char* key : keys) {
        if (j.contains(key) && !j[key].is_null()) return &j[key];
    }
    return nullptr;
}

namespace {

// ISO-8601 with a trailing Z or numeric offset, at second precision or finer.
bool parse_iso(const std::string& iso, std::time_t& out) {
    if (iso.size() < 20) return false;
    std::tm tm{};
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6)
        return false;
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = minute;
    tm.tm_sec = second;

    long offset_min = 0;
    auto pos = iso.find_first_of("Z+-", 19);
    if (pos != std::string::npos && iso[pos] != 'Z') {
        int oh = 0, om = 0;
        if (sscanf(iso.c_str() + pos + 1, "%d:%d", &oh, &om) >= 1) {
            offset_min = (oh * 60 + om) * (iso[pos] == '-' ? -1 : 1);
        }
    }
    out = _mkgmtime(&tm) - offset_min * 60;
    return out > 0;
}

}  // namespace

std::string resets_in(const std::string& iso8601) {
    std::time_t when = 0;
    if (!parse_iso(iso8601, when)) return {};
    long long secs = static_cast<long long>(when) - static_cast<long long>(std::time(nullptr));
    if (secs <= 0) return "reset";
    long long days = secs / 86400;
    long long hours = (secs % 86400) / 3600;
    long long mins = (secs % 3600) / 60;
    char buf[64];
    if (days > 0)
        snprintf(buf, sizeof(buf), "resets in %lldd %lldh", days, hours);
    else if (hours > 0)
        snprintf(buf, sizeof(buf), "resets in %lldh %lldm", hours, mins);
    else
        snprintf(buf, sizeof(buf), "resets in %lldm", mins < 1 ? 1 : mins);
    return buf;
}

std::string format_local(const std::string& iso8601) {
    std::time_t when = 0;
    if (!parse_iso(iso8601, when)) return {};
    std::tm local{};
    if (localtime_s(&local, &when) != 0) return {};
    char buf[64];
    // %-I is a glibc flag. MSVC's strftime treats it as an invalid parameter and
    // abort()s the process, so the fallback below it never ran — that is what
    // killed the app the moment a reset time came back. %I is the portable form.
    if (strftime(buf, sizeof(buf), "%a %I:%M %p", &local) == 0) return {};
    std::string rendered = buf;
    auto space = rendered.find(' ');
    if (space != std::string::npos && space + 2 < rendered.size() && rendered[space + 1] == '0' &&
        isdigit(static_cast<unsigned char>(rendered[space + 2]))) {
        rendered.erase(space + 1, 1);
    }
    return rendered;
}
