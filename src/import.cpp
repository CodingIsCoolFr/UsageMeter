#include "providers.hpp"

#include <windows.h>

#include <ctime>
#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

std::string env_path(const char* var, const char* rest) {
    const char* base = std::getenv(var);
    if (!base || !base[0]) return {};
    return std::string(base) + rest;
}

nlohmann::json read_json(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    try {
        return nlohmann::json::parse(in);
    } catch (...) {
        return {};
    }
}

bool has_provider(const nlohmann::json& accounts, const char* id) {
    if (!accounts.is_array()) return false;
    for (const auto& a : accounts)
        if (a.value("provider", "") == id) return true;
    return false;
}

long long iso_to_unix(const std::string& iso) {
    if (iso.size() < 19) return 0;
    std::tm tm{};
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
    tm.tm_year = y - 1900;
    tm.tm_mon = mo - 1;
    tm.tm_mday = d;
    tm.tm_hour = h;
    tm.tm_min = mi;
    tm.tm_sec = s;
    return static_cast<long long>(_mkgmtime(&tm));
}

std::string capture(const std::wstring& cmdline) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0)) return {};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = write;
    si.hStdError = write;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = cmdline;
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(read);
        CloseHandle(write);
        return {};
    }
    CloseHandle(write);
    std::string out;
    char buf[512];
    DWORD n = 0;
    while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n > 0) out.append(buf, n);
    WaitForSingleObject(pi.hProcess, 15000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) out.pop_back();
    return out;
}

nlohmann::json grok_from_hermes() {
    auto path = env_path("LOCALAPPDATA", "\\hermes\\auth.json");
    auto root = read_json(path);
    if (!root.is_object()) return {};
const nlohmann::json* tokens = nullptr;
    std::string last_refresh;
    long long expires_in = 0;
    try {
        tokens = &root.at("providers").at("xai-oauth").at("tokens");
        last_refresh = root.at("providers").at("xai-oauth").value("last_refresh", "");
        expires_in = root.at("providers").at("xai-oauth").at("tokens").value("expires_in", 0);
    } catch (...) {
        return {};
    }
    if (!tokens->is_object() || !tokens->contains("access_token")) return {};
    nlohmann::json account = {{"provider", "grok"}, {"kind", "oauth"}, {"source", "hermes"}, {"source_path", path}};
    account["access_token"] = (*tokens)["access_token"];
    if (tokens->contains("refresh_token")) account["refresh_token"] = (*tokens)["refresh_token"];
    long long exp = iso_to_unix(last_refresh);
    if (exp > 0 && expires_in > 0) account["expires_at"] = exp + expires_in;
    return account;
}

nlohmann::json codex_from_file() {
    auto path = env_path("USERPROFILE", "\\.codex\\auth.json");
    auto root = read_json(path);
    if (!root.is_object() || !root.contains("tokens") || !root["tokens"].is_object()) return {};
    const auto& tokens = root["tokens"];
    if (!tokens.contains("access_token")) return {};
    nlohmann::json account = {{"provider", "codex"}, {"kind", "chatgpt"}, {"source", "codex"}, {"source_path", path}};
    account["access_token"] = tokens["access_token"];
    if (tokens.contains("refresh_token")) account["refresh_token"] = tokens["refresh_token"];
    if (tokens.contains("account_id")) account["account_id"] = tokens["account_id"];
    return account;
}

nlohmann::json github_from_cli() {
    wchar_t gh[MAX_PATH];
    if (!SearchPathW(nullptr, L"gh.exe", nullptr, MAX_PATH, gh, nullptr)) return {};
    std::wstring cmd = L"\"" + std::wstring(gh) + L"\" auth token";
    std::string token = capture(cmd);
    if (token.size() < 8 || token.find(' ') != std::string::npos) return {};
    return {{"provider", "github"}, {"kind", "gh"}, {"source", "gh"}, {"token", token}};
}

}  // namespace

int import_local_accounts(nlohmann::json& accounts) {
    if (!accounts.is_array()) accounts = nlohmann::json::array();
    int added = 0;
    auto add = [&](nlohmann::json account) {
        if (account.is_null() || account.empty()) return;
        if (has_provider(accounts, account.value("provider", "").c_str())) return;
        account["id"] = std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + account.value("provider", "x");
        accounts.push_back(std::move(account));
        ++added;
    };
    add(grok_from_hermes());
    add(github_from_cli());
    return added;
}

bool reload_linked_session(nlohmann::json& account) {
    const std::string source = account.value("source", "");
    if (source == "hermes") {
        auto fresh = grok_from_hermes();
        if (fresh.empty()) return false;
        account["access_token"] = fresh["access_token"];
        if (fresh.contains("refresh_token")) account["refresh_token"] = fresh["refresh_token"];
        if (fresh.contains("expires_at")) account["expires_at"] = fresh["expires_at"];
        return true;
    }
    if (source == "codex") {
        auto fresh = codex_from_file();
        if (fresh.empty()) return false;
        account["access_token"] = fresh["access_token"];
        if (fresh.contains("refresh_token")) account["refresh_token"] = fresh["refresh_token"];
        if (fresh.contains("account_id")) account["account_id"] = fresh["account_id"];
        return true;
    }
    return false;
}
