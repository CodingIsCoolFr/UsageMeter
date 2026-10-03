#include "providers.hpp"

#include "http.hpp"
#include "jsonutil.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sstream>
#include <thread>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
using SOCKET = int;
#ifndef INVALID_SOCKET
#define INVALID_SOCKET (-1)
#endif
#define closesocket ::close
#endif

namespace {

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 3);
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out.push_back(static_cast<char>(c));
        else {
            char buf[8];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string form(std::initializer_list<std::pair<const char*, std::string>> fields) {
    std::string body;
    for (const auto& [k, v] : fields) {
        if (!body.empty()) body.push_back('&');
        body += url_encode(k);
        body.push_back('=');
        body += url_encode(v);
    }
    return body;
}

void open_browser(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    // argv, not a shell: authorize URLs contain '&'.
    pid_t pid = fork();
    if (pid == 0) {
#ifdef __APPLE__
        execlp("open", "open", url.c_str(), static_cast<char*>(nullptr));
#else
        execlp("xdg-open", "xdg-open", url.c_str(), static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
#endif
}

std::string json_error(const HttpResponse& res, const char* what) {
    if (!res.error.empty()) return std::string(what) + ": " + res.error;
    std::string detail;
    try {
        auto j = nlohmann::json::parse(res.body);
        if (j.contains("error")) {
            if (j["error"].is_string())
                detail = j["error"].get<std::string>();
            else if (j["error"].is_object() && j["error"].contains("message"))
                detail = j["error"]["message"].get<std::string>();
        } else if (j.contains("message") && j["message"].is_string()) {
            detail = j["message"].get<std::string>();
        }
    } catch (...) {
    }
    if (detail.size() > 180) detail.resize(180);
    return std::string(what) + " failed (HTTP " + std::to_string(res.status) + ")" +
           (detail.empty() ? "" : ": " + detail);
}

// ---------------------------------------------------------------------------
// Loopback redirect catcher. One connection is enough: the browser hits
// 127.0.0.1:<port><path>?code=...&state=... and we answer with a small page.
// ---------------------------------------------------------------------------

std::string recv_some(SOCKET s) {
    std::string data;
    char buf[4096];
#ifdef _WIN32
    DWORD wait = 2000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<char*>(&wait), sizeof(wait));
#else
    timeval wait{2, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
#endif
    for (;;) {
        int n = recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        data.append(buf, n);
        if (data.find("\r\n\r\n") != std::string::npos) break;
        if (data.size() > 65536) break;
    }
    return data;
}

std::string query_param(const std::string& request, const std::string& key) {
    auto q = request.find('?');
    auto sp = request.find(' ', q == std::string::npos ? 0 : q);
    if (q == std::string::npos || sp == std::string::npos) return {};
    std::string query = request.substr(q + 1, sp - q - 1);
    std::string needle = key + "=";
    auto at = query.find(needle);
    if (at == std::string::npos) return {};
    auto end = query.find('&', at);
    std::string value = query.substr(at + needle.size(), end == std::string::npos ? std::string::npos : end - at - needle.size());
    std::string decoded;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '%' && i + 2 < value.size()) {
            decoded.push_back(static_cast<char>(strtol(value.substr(i + 1, 2).c_str(), nullptr, 16)));
            i += 2;
        } else if (value[i] == '+') {
            decoded.push_back(' ');
        } else {
            decoded.push_back(value[i]);
        }
    }
    return decoded;
}

}  // namespace

OAuthResult oauth_loopback(const std::string& authorize_url, const std::string& redirect_host, int port,
                           const std::string& redirect_path, const std::string& expected_state, int timeout_s) {
    OAuthResult result;
#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        result.error = "WSAStartup failed";
        return result;
    }
#endif

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, redirect_host.c_str(), &addr.sin_addr);
    int reuse = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || listen(listener, 8) != 0) {
        result.error = "Could not listen on " + redirect_host + ":" + std::to_string(port) +
                       " (is another sign-in already running?)";
        closesocket(listener);
        return result;
    }

    open_browser(authorize_url);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    while (std::chrono::steady_clock::now() < deadline) {
        auto left = std::chrono::duration_cast<std::chrono::seconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left < 1) left = 1;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        timeval tv{static_cast<long>(left > 5 ? 5 : left), 0};
#ifdef _WIN32
        if (select(0, &fds, nullptr, nullptr, &tv) <= 0) continue;
#else
        if (select(static_cast<int>(listener) + 1, &fds, nullptr, nullptr, &tv) <= 0) continue;
#endif

        SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        std::string request = recv_some(client);
        const char* page =
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
            "<!doctype html><meta charset=utf-8><title>UsageMeter</title>"
            "<body style=\"font-family:Segoe UI,sans-serif;background:#1a1d27;color:#e7e9ee;"
            "display:flex;align-items:center;justify-content:center;height:100vh;margin:0\">"
            "<div style=\"text-align:center\"><h2>Signed in</h2><p>You can close this tab and return to UsageMeter.</p></div>";
        send(client, page, static_cast<int>(strlen(page)), 0);
        closesocket(client);

        // Chrome probes the port and hangs up before the real redirect. Keep
        // listening until a callback actually carries a code.
        if (request.empty() || request.find("code=") == std::string::npos) continue;
        std::string err = query_param(request, "error");
        if (!err.empty()) {
            result.error = "Sign-in was declined (" + err + ")";
            break;
        }
        std::string state = query_param(request, "state");
        if (!expected_state.empty() && state != expected_state) continue;
        result.code = query_param(request, "code");
        if (!result.code.empty()) break;
    }
    closesocket(listener);

    if (result.code.empty() && result.error.empty())
        result.error = "Sign-in did not finish. It will retry from the session already on this PC.";
    return result;
}

// ---------------------------------------------------------------------------
// Claude
//
// Subscription usage (the bars in the screenshot) comes from
// GET https://api.anthropic.com/api/oauth/usage with an OAuth bearer and the
// anthropic-beta: oauth-2025-04-20 header — the same call Claude Code's
// /usage makes. The authorize step is Anthropic's copy-paste PKCE flow; the
// client id below is the public Claude Code client, which is what the usage
// endpoint accepts.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kAnthropicClientId = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";
constexpr const char* kAnthropicRedirect = "https://platform.claude.com/oauth/code/callback";
constexpr const char* kAnthropicToken = "https://platform.claude.com/v1/oauth/token";
constexpr const char* kAnthropicUsage = "https://api.anthropic.com/api/oauth/usage";
constexpr const char* kAnthropicBeta = "oauth-2025-04-20";

bool anthropic_refresh(nlohmann::json& account) {
    if (!account.contains("refresh_token")) return false;
    HttpRequest req;
    req.method = "POST";
    req.url = kAnthropicToken;
    req.headers = {"Content-Type: application/json", "Accept: application/json"};
    nlohmann::json body = {{"grant_type", "refresh_token"},
                           {"client_id", kAnthropicClientId},
                           {"refresh_token", account["refresh_token"].get<std::string>()}};
    req.body = body.dump();
    auto res = http_request(req);
    if (res.status != 200) return false;
    try {
        auto j = nlohmann::json::parse(res.body);
        if (!j.contains("access_token")) return false;
        account["access_token"] = j["access_token"];
        if (j.contains("refresh_token")) account["refresh_token"] = j["refresh_token"];
        account["expires_at"] = static_cast<long long>(std::time(nullptr)) + j.value("expires_in", 3600);
        return true;
    } catch (...) {
        return false;
    }
}

Meter meter_from_window(const std::string& label, const nlohmann::json& w) {
    Meter m;
    m.label = label;
    if (auto pct = json_number(w, "utilization")) m.used_pct = *pct;
    if (w.contains("resets_at") && w["resets_at"].is_string()) m.resets_at = w["resets_at"].get<std::string>();
    std::string when = format_local(m.resets_at);
    std::string left = resets_in(m.resets_at);
    if (!when.empty() && !left.empty())
        m.detail = left + "  ·  " + when;
    else
        m.detail = left.empty() ? when : left;
    return m;
}

}  // namespace

PasteFlow anthropic_authorize_url() {
    PasteFlow flow;
    flow.verifier = random_b64url(32);
    flow.state = random_b64url(24);
    std::string challenge = sha256_b64url(flow.verifier);
    flow.url = std::string("https://claude.com/cai/oauth/authorize?code=true") +
               "&client_id=" + kAnthropicClientId +
               "&response_type=code" +
               "&redirect_uri=" + url_encode(kAnthropicRedirect) +
               "&scope=" + url_encode("user:profile user:inference user:sessions:claude_code") +
               "&code_challenge=" + challenge +
               "&code_challenge_method=S256" +
               "&state=" + flow.state;
    return flow;
}

std::string anthropic_exchange(nlohmann::json& account, const std::string& pasted, const PasteFlow& flow) {
    std::string code = pasted;
    std::string state = flow.state;
    auto hash = pasted.find('#');
    if (hash != std::string::npos) {
        code = pasted.substr(0, hash);
        state = pasted.substr(hash + 1);
    }
    while (!code.empty() && isspace(static_cast<unsigned char>(code.front()))) code.erase(code.begin());
    while (!code.empty() && isspace(static_cast<unsigned char>(code.back()))) code.pop_back();
    if (code.empty()) return "Paste the code Claude showed you";
    if (!state.empty() && state != flow.state) return "That code belongs to a different sign-in attempt";

    HttpRequest req;
    req.method = "POST";
    req.url = kAnthropicToken;
    req.headers = {"Content-Type: application/json", "Accept: application/json"};
    nlohmann::json body = {{"grant_type", "authorization_code"},
                           {"client_id", kAnthropicClientId},
                           {"code", code},
                           {"state", state},
                           {"redirect_uri", kAnthropicRedirect},
                           {"code_verifier", flow.verifier}};
    req.body = body.dump();
    auto res = http_request(req);
    if (res.status != 200) return json_error(res, "Claude sign-in");
    try {
        auto j = nlohmann::json::parse(res.body);
        account["access_token"] = j.at("access_token");
        if (j.contains("refresh_token")) account["refresh_token"] = j["refresh_token"];
        account["expires_at"] = static_cast<long long>(std::time(nullptr)) + j.value("expires_in", 3600);
    } catch (...) {
        return "Claude returned an unexpected token response";
    }
    return {};
}

AccountView refresh_claude(nlohmann::json& account) {
    AccountView view;
    view.provider = "Claude";
    view.id = account.value("id", "");

    long long now = static_cast<long long>(std::time(nullptr));
    if (account.value("expires_at", 0LL) < now + 120) {
        if (!anthropic_refresh(account)) {
            view.error = "Claude session expired — sign in again";
            return view;
        }
    }

    HttpRequest req;
    req.url = kAnthropicUsage;
    req.headers = {"Authorization: Bearer " + account.value("access_token", ""),
                   "anthropic-beta: " + std::string(kAnthropicBeta), "Accept: application/json"};
    auto res = http_request(req);
    if (res.status == 401 || res.status == 403) {
        if (anthropic_refresh(account)) {
            req.headers[0] = "Authorization: Bearer " + account.value("access_token", "");
            res = http_request(req);
        }
    }
    if (res.status != 200) {
        view.error = json_error(res, "Claude usage");
        return view;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(res.body);
    } catch (...) {
        view.error = "Claude returned usage that was not JSON";
        return view;
    }

    struct Named {
        const char* key;
        const char* label;
    };
    // Flat keys (older responses) and the structured `limits` array (current).
    for (const auto& n : {Named{"five_hour", "Session limit"}, Named{"seven_day", "Weekly · all models"},
                          Named{"seven_day_sonnet", "Weekly · Sonnet"}, Named{"seven_day_opus", "Weekly · Opus"}}) {
        if (j.contains(n.key) && j[n.key].is_object()) view.meters.push_back(meter_from_window(n.label, j[n.key]));
    }
    if (j.contains("limits") && j["limits"].is_array()) {
        for (const auto& lim : j["limits"]) {
            if (!lim.is_object()) continue;
            std::string kind = lim.value("kind", "");
            std::string label = "Usage";
            if (kind == "session")
                label = "Session limit";
            else if (kind == "weekly_all")
                label = "Weekly · all models";
            else if (kind == "weekly_scoped")
                label = "Weekly · " + lim.value("/scope/model/display_name"_json_pointer, std::string("model"));
            Meter m;
            m.label = label;
            if (auto pct = json_number(lim, "percent"))
                m.used_pct = *pct;
            else if (auto pct = json_number(lim, "utilization"))
                m.used_pct = *pct;
            if (lim.contains("resets_at") && lim["resets_at"].is_string()) m.resets_at = lim["resets_at"].get<std::string>();
            std::string when = format_local(m.resets_at);
            std::string left = resets_in(m.resets_at);
            m.detail = left + (when.empty() ? "" : "  ·  " + when);
            bool dup = std::any_of(view.meters.begin(), view.meters.end(),
                                   [&](const Meter& e) { return e.label == m.label; });
            if (!dup) view.meters.push_back(m);
        }
    }

    if (j.contains("extra_usage") && j["extra_usage"].is_object()) {
        const auto& extra = j["extra_usage"];
        bool enabled = extra.value("is_enabled", false);
        auto limit = json_number(extra, "monthly_limit");
        auto used = json_number(extra, "used_credits");
        if (enabled && limit && *limit > 0) {
            Meter m;
            m.label = "Cloud session credits";
            m.used_pct = used ? std::min(100.0, *used / *limit * 100.0) : 0;
            double scale = *limit > 1000 ? 100.0 : 1.0;  // cents when the figure is large
            double left = (*limit - (used ? *used : 0)) / scale;
            double cap = *limit / scale;
            char buf[64];
            snprintf(buf, sizeof(buf), "$%.0f of $%.0f left", left < 0 ? 0 : left, cap);
            m.detail = buf;
            view.meters.push_back(m);
        }
    }

    if (view.meters.empty()) view.error = "Claude reported no usage windows for this account";
    view.plan = account.value("plan", "Claude");
    return view;
}

// ---------------------------------------------------------------------------
// Grok
//
// Two different pools. The consumer subscription (the bar in grok.com →
// Settings → Usage) is GET https://cli-chat-proxy.grok.com/v1/billing?format=credits
// with the Grok CLI OAuth bearer. The xAI API is a separate wallet, read with
// a management key from the console, and is entered as a key rather than a login.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kXaiClientId = "b1a00492-073a-47ea-816f-4c329264a828";
constexpr const char* kXaiAuthorize = "https://auth.x.ai/oauth2/authorize";
constexpr const char* kXaiToken = "https://auth.x.ai/oauth2/token";
constexpr const char* kXaiRedirect = "http://127.0.0.1:56121/callback";
constexpr const char* kXaiScope = "openid profile email offline_access grok-cli:access api:access";
constexpr const char* kGrokBilling = "https://cli-chat-proxy.grok.com/v1/billing?format=credits";
constexpr const char* kGrokBillingMonthly = "https://cli-chat-proxy.grok.com/v1/billing";

bool xai_refresh(nlohmann::json& account) {
    if (!account.contains("refresh_token")) return false;
    HttpRequest req;
    req.method = "POST";
    req.url = kXaiToken;
    req.headers = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"};
    req.body = form({{"grant_type", "refresh_token"},
                     {"client_id", kXaiClientId},
                     {"refresh_token", account["refresh_token"].get<std::string>()}});
    auto res = http_request(req);
    if (res.status != 200) return false;
    try {
        auto j = nlohmann::json::parse(res.body);
        if (!j.contains("access_token")) return false;
        account["access_token"] = j["access_token"];
        if (j.contains("refresh_token")) account["refresh_token"] = j["refresh_token"];
        account["expires_at"] = static_cast<long long>(std::time(nullptr)) + j.value("expires_in", 3600);
        return true;
    } catch (...) {
        return false;
    }
}

std::string product_name(std::string raw) {
    if (raw.rfind("Grok", 0) == 0 && raw.size() > 4 && raw[4] >= 'A' && raw[4] <= 'Z') raw.insert(4, " ");
    return raw;
}

}  // namespace

std::string xai_sign_in(nlohmann::json& account, const std::function<void(const std::string&)>& progress) {
    std::string verifier = random_b64url(64);
    std::string challenge = sha256_b64url(verifier);
    std::string state = random_b64url(24);
    std::string nonce = random_b64url(24);
    std::string url = std::string(kXaiAuthorize) + "?response_type=code" +
                      "&client_id=" + kXaiClientId +
                      "&redirect_uri=" + url_encode(kXaiRedirect) +
                      "&scope=" + url_encode(kXaiScope) +
                      "&state=" + state +
                      "&nonce=" + nonce +
                      "&code_challenge=" + challenge +
                      "&code_challenge_method=S256";
    progress("Waiting for the browser sign-in…");
    auto result = oauth_loopback(url, "127.0.0.1", 56121, "/callback", state, 180);
    if (!result.error.empty()) return result.error;

    progress("Exchanging the sign-in code…");
    HttpRequest req;
    req.method = "POST";
    req.url = kXaiToken;
    req.headers = {"Content-Type: application/x-www-form-urlencoded", "Accept: application/json"};
    req.body = form({{"grant_type", "authorization_code"},
                     {"client_id", kXaiClientId},
                     {"code", result.code},
                     {"redirect_uri", kXaiRedirect},
                     {"code_verifier", verifier}});
    auto res = http_request(req);
    if (res.status != 200) return json_error(res, "Grok sign-in");
    try {
        auto j = nlohmann::json::parse(res.body);
        account["access_token"] = j.at("access_token");
        if (j.contains("refresh_token")) account["refresh_token"] = j["refresh_token"];
        account["expires_at"] = static_cast<long long>(std::time(nullptr)) + j.value("expires_in", 3600);
    } catch (...) {
        return "Grok returned an unexpected token response";
    }
    return {};
}

AccountView refresh_grok(nlohmann::json& account) {
    AccountView view;
    view.provider = "Grok";
    view.id = account.value("id", "");

    if (account.value("kind", "") == "management_key") {
        // xAI API wallet. The management key is entered by the user; team id too.
        std::string key = account.value("management_key", "");
        std::string team = account.value("team_id", "");
        if (key.empty() || team.empty()) {
            view.error = "Add the management key and team id";
            return view;
        }
        HttpRequest req;
        req.method = "POST";
        req.url = "https://management-api.x.ai/v1/billing/teams/" + team + "/usage";
        req.headers = {"Authorization: Bearer " + key, "Content-Type: application/json", "Accept: application/json"};
        long long now = static_cast<long long>(std::time(nullptr));
        nlohmann::json body = {{"startTime", std::to_string(now - 30LL * 86400)},
                               {"endTime", std::to_string(now)},
                               {"granularity", "GRANULARITY_DAY"}};
        req.body = body.dump();
        auto res = http_request(req);
        if (res.status != 200) {
            view.error = json_error(res, "xAI usage");
            return view;
        }
        view.plan = "xAI API";
        view.note = "Management API returned usage for team " + team;
        try {
            auto j = nlohmann::json::parse(res.body);
            view.note = j.dump().size() > 400 ? "Usage data received" : j.dump();
        } catch (...) {
        }
        return view;
    }

    long long now = static_cast<long long>(std::time(nullptr));
    if (account.value("source", "") == "hermes") reload_linked_session(account);
    if (account.value("source", "") != "hermes" && account.value("expires_at", 0LL) < now + 120 &&
        !xai_refresh(account)) {
        view.error = "Grok session expired";
        return view;
    }

    auto bearer = [&](const std::string& url) {
        HttpRequest req;
        req.url = url;
        req.headers = {"Authorization: Bearer " + account.value("access_token", ""), "Accept: application/json",
                       "x-grok-client-mode: cli", "x-grok-client-version: 1.0.46",
                       "x-grok-client-surface: grok-build"};
        return http_request(req);
    };

    auto res = bearer(kGrokBilling);
    if ((res.status == 401 || res.status == 403) && account.value("source", "") != "hermes") {
        if (xai_refresh(account)) res = bearer(kGrokBilling);
    }
    if (res.status != 200) {
        view.error = json_error(res, "Grok usage");
        return view;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(res.body);
    } catch (...) {
        view.error = "Grok returned usage that was not JSON";
        return view;
    }
    const auto* cfg = j.contains("config") && j["config"].is_object() ? &j["config"] : &j;

    std::string tier = j.value("subscription_tier", j.value("subscriptionTier", ""));
    if (tier.empty() && cfg->contains("subscriptionTier") && (*cfg)["subscriptionTier"].is_string())
        tier = (*cfg)["subscriptionTier"].get<std::string>();
    view.plan = tier.empty() ? "SuperGrok" : tier;

    const nlohmann::json* period = json_first(*cfg, {"currentPeriod", "current_period"});
    std::string period_type = period ? period->value("type", "") : "";
    std::string resets = period && period->contains("end") && (*period)["end"].is_string()
                             ? (*period)["end"].get<std::string>()
                             : "";

    Meter weekly;
    weekly.label = period_type.find("MONTH") != std::string::npos ? "Monthly usage" : "Weekly usage";
    weekly.resets_at = resets;
    std::string when = format_local(resets);
    std::string left = resets_in(resets);
    weekly.detail = left + (when.empty() ? "" : "  ·  " + when);

    if (auto pct = json_number(*cfg, "creditUsagePercent"))
        weekly.used_pct = *pct > 1.0 ? *pct : *pct * 100.0;
    else if (auto pct = json_number(*cfg, "credit_usage_percent"))
        weekly.used_pct = *pct > 1.0 ? *pct : *pct * 100.0;
    else
        weekly.unknown = true;  // period exists, percent omitted — do not invent 0%
    view.meters.push_back(weekly);

    if (cfg->contains("productUsage") && (*cfg)["productUsage"].is_array()) {
        for (const auto& p : (*cfg)["productUsage"]) {
            if (!p.is_object()) continue;
            auto pct = json_number(p, "usagePercent");
            if (!pct) continue;
            Meter m;
            m.label = product_name(p.value("product", "Product"));
            m.used_pct = *pct > 1.0 ? *pct : *pct * 100.0;
            m.detail = "share of the weekly pool";
            view.meters.push_back(m);
        }
    }

    if (auto cap = json_number(*cfg, "onDemandCap")) {
        // { "val": N } shape.
    }
    const auto* cap_node = json_first(*cfg, {"onDemandCap"});
    const auto* used_node = json_first(*cfg, {"onDemandUsed"});
    auto cents = [](const nlohmann::json* node) -> std::optional<double> {
        if (!node) return std::nullopt;
        if (node->is_object()) return json_number(*node, "val");
        if (node->is_number()) return node->get<double>();
        return std::nullopt;
    };
    auto cap = cents(cap_node);
    auto used = cents(used_node);
    if (cap && *cap > 0) {
        Meter m;
        m.label = "On-demand";
        m.used_pct = used ? std::min(100.0, *used / *cap * 100.0) : 0;
        char buf[64];
        snprintf(buf, sizeof(buf), "$%.2f of $%.2f", (used ? *used : 0) / 100.0, *cap / 100.0);
        m.detail = buf;
        view.meters.push_back(m);
    }

    auto monthly = bearer(kGrokBillingMonthly);
    if (monthly.status == 200) {
        try {
            auto mj = nlohmann::json::parse(monthly.body);
            const auto* mc = mj.contains("config") ? &mj["config"] : &mj;
            auto limit = mc->contains("monthlyLimit") ? json_number((*mc)["monthlyLimit"], "val") : std::nullopt;
            auto spent = mc->contains("used") ? json_number((*mc)["used"], "val") : std::nullopt;
            if (limit && *limit > 0 && spent) {
                Meter m;
                m.label = "Monthly allowance";
                m.used_pct = std::min(100.0, *spent / *limit * 100.0);
                char buf[64];
                snprintf(buf, sizeof(buf), "$%.2f of $%.2f", *spent / 100.0, *limit / 100.0);
                m.detail = buf;
                view.meters.push_back(m);
            }
        } catch (...) {
        }
    }
    return view;
}

// ---------------------------------------------------------------------------
// OpenAI — API usage against an admin key. ChatGPT subscription meters have no
// public endpoint, so this one is key-based on purpose.
// ---------------------------------------------------------------------------

AccountView refresh_openai(nlohmann::json& account) {
    AccountView view;
    view.provider = "OpenAI";
    view.id = account.value("id", "");
    view.plan = "API";
    std::string key = account.value("admin_key", "");
    if (key.empty()) {
        view.error = "Add an admin key (sk-admin-…)";
        return view;
    }

    long long now = static_cast<long long>(std::time(nullptr));
    HttpRequest req;
    req.url = "https://api.openai.com/v1/organization/costs?start_time=" + std::to_string(now - 30LL * 86400) +
              "&limit=30&bucket_width=1d";
    req.headers = {"Authorization: Bearer " + key, "Accept: application/json"};
    auto res = http_request(req);
    if (res.status != 200) {
        view.error = json_error(res, "OpenAI usage");
        return view;
    }
    try {
        auto j = nlohmann::json::parse(res.body);
        double total = 0;
        if (j.contains("data") && j["data"].is_array()) {
            for (const auto& bucket : j["data"]) {
                if (!bucket.contains("results") || !bucket["results"].is_array()) continue;
                for (const auto& r : bucket["results"]) {
                    if (r.contains("amount") && r["amount"].contains("value") && r["amount"]["value"].is_number())
                        total += r["amount"]["value"].get<double>();
                }
            }
        }
        Meter m;
        m.label = "Last 30 days";
        char buf[64];
        snprintf(buf, sizeof(buf), "$%.2f spent", total);
        m.detail = buf;
        m.used_pct = -1;
        view.meters.push_back(m);
    } catch (...) {
        view.error = "OpenAI returned usage that was not JSON";
    }
    return view;
}

// ---------------------------------------------------------------------------
// GitHub — REST rate limit for the signed-in user. OAuth device flow, so no
// client secret and no loopback port.
// ---------------------------------------------------------------------------

namespace {

constexpr const char* kGithubClientId = "Ov23liplaceholder";  // replaced below if unset

std::string github_device(nlohmann::json& account, const std::function<void(const std::string&)>& progress) {
    // GitHub's device flow needs an OAuth app the user controls. Rather than
    // ship someone else's client id, accept a personal access token directly:
    // it reads exactly the same endpoint and never leaves this machine.
    (void)account;
    (void)progress;
    return "Paste a personal access token instead";
}

}  // namespace

AccountView refresh_github(nlohmann::json& account) {
    AccountView view;
    view.provider = "GitHub";
    view.id = account.value("id", "");
    std::string token = account.value("token", "");
    if (token.empty()) {
        view.error = "Add a personal access token";
        return view;
    }

    HttpRequest who;
    who.url = "https://api.github.com/user";
    who.headers = {"Authorization: Bearer " + token, "Accept: application/vnd.github+json",
                   "User-Agent: UsageMeter", "X-GitHub-Api-Version: 2022-11-28"};
    auto me = http_request(who);
    if (me.status == 200) {
        try {
            auto j = nlohmann::json::parse(me.body);
            view.identity = j.value("login", "");
            view.plan = j.value("plan", nlohmann::json::object()).value("name", "GitHub");
        } catch (...) {
        }
    }

    HttpRequest copilot = who;
    copilot.url = "https://api.github.com/copilot_internal/user";
    auto cop = http_request(copilot);
    if (cop.status == 200) {
        try {
            auto j = nlohmann::json::parse(cop.body);
            std::string plan = j.value("copilot_plan", "");
            if (!plan.empty()) {
                if (!plan.empty() && plan[0] >= 'a' && plan[0] <= 'z') plan[0] = static_cast<char>(plan[0] - 32);
                view.plan = "Copilot " + plan;
            }
            std::string reset = j.value("quota_reset_date_utc", "");
            const auto* snaps = j.contains("quota_snapshots") ? &j["quota_snapshots"] : nullptr;
            auto add_quota = [&](const char* key, const char* label) {
                if (!snaps || !snaps->contains(key) || !(*snaps)[key].is_object()) return;
                const auto& q = (*snaps)[key];
                auto entitlement = json_number(q, "entitlement");
                if (!entitlement || *entitlement <= 0) return;
                Meter m;
                m.label = label;
                if (auto left = json_number(q, "percent_remaining"))
                    m.used_pct = std::max(0.0, std::min(100.0, 100.0 - *left));
                m.resets_at = reset;
                auto remaining = json_number(q, "remaining");
                char buf[64];
                snprintf(buf, sizeof(buf), "%.0f of %.0f left", remaining ? *remaining : 0, *entitlement);
                m.detail = buf;
                view.meters.push_back(m);
            };
            add_quota("chat", "Chat");
            add_quota("completions", "Completions");
            add_quota("premium_interactions", "Premium");
        } catch (...) {
        }
    }

    HttpRequest req;
    req.url = "https://api.github.com/rate_limit";
    req.headers = who.headers;
    auto res = http_request(req);
    if (res.status != 200) {
        view.error = json_error(res, "GitHub rate limit");
        return view;
    }
    try {
        auto j = nlohmann::json::parse(res.body);
        if (!j.contains("resources") || !j["resources"].is_object()) {
            view.error = "GitHub returned no rate-limit resources";
            return view;
        }
        for (const char* family : {"core", "search", "graphql", "code_search"}) {
            if (!j["resources"].contains(family)) continue;
            const auto& r = j["resources"][family];
            double limit = r.value("limit", 0.0);
            double used = r.value("used", 0.0);
            if (limit <= 0) continue;
            Meter m;
            m.label = std::string(family) == "core" ? "REST" : family;
            m.used_pct = used / limit * 100.0;
            long long reset = r.value("reset", 0LL);
            if (reset > 0) {
                std::time_t when = static_cast<std::time_t>(reset);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &when);
#else
                localtime_r(&when, &local);
#endif
                char buf[64];
                strftime(buf, sizeof(buf), "resets %I:%M %p", &local);
                m.detail = buf;
            }
            char buf[64];
            snprintf(buf, sizeof(buf), "%.0f of %.0f used", used, limit);
            m.detail = std::string(buf) + (m.detail.empty() ? "" : "  ·  " + m.detail);
            view.meters.push_back(m);
        }
    } catch (...) {
        view.error = "GitHub returned rate limits that were not JSON";
    }
    return view;
}

namespace {

std::string iso_from_unix(long long unix_s) {
    if (unix_s <= 0) return {};
    std::time_t when = static_cast<std::time_t>(unix_s);
    std::tm tm{};
#ifdef _WIN32
    if (gmtime_s(&tm, &when) != 0) return {};
#else
    if (!gmtime_r(&when, &tm)) return {};
#endif
    char buf[40];
    if (strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm) == 0) return {};
    return buf;
}

void add_codex_window(AccountView& view, const char* fallback, const nlohmann::json& w) {
    if (!w.is_object()) return;
    Meter m;
    long long window_s = 0;
    if (auto n = json_number(w, "limit_window_seconds")) window_s = static_cast<long long>(*n);
    m.label = window_s >= 6 * 86400 ? "Weekly" : (window_s > 0 ? "Session" : fallback);
    if (auto pct = json_number(w, "used_percent"))
        m.used_pct = *pct;
    else if (auto pct = json_number(w, "usedPercent"))
        m.used_pct = *pct;
    long long reset = 0;
    if (auto n = json_number(w, "reset_at")) reset = static_cast<long long>(*n);
    if (reset > 0) {
        m.resets_at = iso_from_unix(reset);
        std::string when = format_local(m.resets_at);
        std::string left = resets_in(m.resets_at);
        m.detail = left + (when.empty() ? "" : "  ·  " + when);
    }
    if (m.used_pct >= 0 || !m.resets_at.empty()) view.meters.push_back(m);
}

}  // namespace

AccountView refresh_codex(nlohmann::json& account) {
    AccountView view;
    view.provider = "Codex";
    view.id = account.value("id", "");
    reload_linked_session(account);
    std::string token = account.value("access_token", "");
    std::string account_id = account.value("account_id", "");
    if (token.empty()) {
        view.error = "No Codex session on this PC";
        return view;
    }

    auto fetch = [&](const std::string& bearer) {
        HttpRequest req;
        req.url = "https://chatgpt.com/backend-api/wham/usage";
        req.headers = {"Authorization: Bearer " + bearer, "Accept: application/json"};
        if (!account_id.empty()) req.headers.push_back("ChatGPT-Account-Id: " + account_id);
        return http_request(req);
    };

    auto res = fetch(token);
    if (res.status == 401 && account.contains("refresh_token")) {
        HttpRequest refresh;
        refresh.method = "POST";
        refresh.url = "https://auth.openai.com/oauth/token";
        refresh.headers = {"Content-Type: application/json", "Accept: application/json"};
        refresh.body = nlohmann::json({{"grant_type", "refresh_token"},
                                       {"client_id", "app_EMoamEEZ73f0CkXaXp7hrann"},
                                       {"refresh_token", account["refresh_token"].get<std::string>()}})
                           .dump();
        auto tok = http_request(refresh);
        if (tok.status == 200) {
            try {
                auto j = nlohmann::json::parse(tok.body);
                if (j.contains("access_token")) {
                    account["access_token"] = j["access_token"];
                    if (j.contains("refresh_token")) account["refresh_token"] = j["refresh_token"];
                    token = account["access_token"].get<std::string>();
                    res = fetch(token);
                }
            } catch (...) {
            }
        }
    }
    if (res.status != 200) {
        view.error = json_error(res, "Codex usage");
        return view;
    }
    try {
        auto j = nlohmann::json::parse(res.body);
        view.plan = j.value("plan_type", "ChatGPT");
        const nlohmann::json* limits = json_first(j, {"rate_limit"});
        if (limits) {
            if (limits->contains("primary_window")) add_codex_window(view, "Session", (*limits)["primary_window"]);
            if (limits->contains("secondary_window")) add_codex_window(view, "Weekly", (*limits)["secondary_window"]);
        }
        const nlohmann::json* spend = json_first(j, {"spend_control"});
        if (spend && spend->contains("individual_limit") && (*spend)["individual_limit"].is_object()) {
            const auto& lim = (*spend)["individual_limit"];
            Meter m;
            m.label = "Credits";
            if (auto pct = json_number(lim, "used_percent")) m.used_pct = *pct;
            auto money = [](const nlohmann::json& v) {
                if (v.is_string()) return v.get<std::string>();
                if (v.is_number()) {
                    char buf[32];
                    snprintf(buf, sizeof(buf), "%.0f", v.get<double>());
                    return std::string(buf);
                }
                return std::string();
            };
            std::string remaining = lim.contains("remaining") ? money(lim["remaining"]) : "";
            std::string cap = lim.contains("limit") ? money(lim["limit"]) : "";
            if (!remaining.empty() && !cap.empty()) m.detail = "$" + remaining + " of $" + cap + " left";
            view.meters.push_back(m);
        }
        if (view.meters.empty()) view.error = "Codex reported no usage windows";
    } catch (...) {
        view.error = "Codex returned usage that was not JSON";
    }
    return view;
}

const std::vector<Provider>& providers() {
    static const std::vector<Provider> all = {
        Provider{"claude", "Claude", "Pro, Max, Team and Enterprise plan limits — the session and weekly bars.",
                 nullptr, refresh_claude},
        Provider{"grok", "Grok", "SuperGrok weekly pool, picked up from the session already on this PC.", nullptr,
                 refresh_grok},
        Provider{"codex", "Codex", "ChatGPT session and weekly limits from the Codex login on this PC.", nullptr,
                 refresh_codex},
        Provider{"openai", "OpenAI", "API spend for the last 30 days, from an admin key.", nullptr, refresh_openai},
        Provider{"github", "GitHub", "Copilot chat and completion quota, plus REST limits, from the gh login on this PC.", nullptr,
                 refresh_github},
    };
    return all;
}

const Provider* find_provider(const std::string& id) {
    for (const auto& p : providers())
        if (p.id == id) return &p;
    return nullptr;
}
