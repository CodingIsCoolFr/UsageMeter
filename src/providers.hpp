#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

// One metered window: a percentage used plus when it resets.
struct Meter {
    std::string label;
    double used_pct = -1;  // 0..100; negative means "not reported"
    std::string resets_at;  // ISO-8601, may be empty
    std::string detail;     // "resets Thu 2:00 AM", "$8 of $100 left", ...
    bool unknown = false;   // provider confirmed the window but omitted the percent
};

struct AccountView {
    std::string id;
    std::string provider;  // "Claude", "Grok", "OpenAI", "GitHub"
    std::string plan;      // "Pro", "SuperGrok", ...
    std::string identity;  // email or login, when the provider reports one
    std::vector<Meter> meters;
    std::string note;   // extra line (credits, tier, rate-limit family)
    std::string error;  // set when the refresh failed
    bool busy = false;
};

// A provider knows how to sign a user in and turn the saved account JSON
// into meters. Sign-in runs off the UI thread; it reports progress through
// the callback and returns an error string (empty on success).
struct Provider {
    std::string id;
    std::string name;
    std::string blurb;

    std::function<std::string(nlohmann::json& account, const std::function<void(const std::string&)>& progress)>
        sign_in;
    std::function<AccountView(nlohmann::json& account)> refresh;
};

const std::vector<Provider>& providers();
const Provider* find_provider(const std::string& id);

// Shared OAuth pieces.
struct OAuthResult {
    std::string code;
    std::string error;
};

// Opens the system browser and waits for the loopback redirect.
OAuthResult oauth_loopback(const std::string& authorize_url, const std::string& redirect_host, int port,
                           const std::string& redirect_path, const std::string& expected_state, int timeout_s);

// Anthropic's copy-paste flow: browser shows a code, user pastes "code#state".
struct PasteFlow {
    std::string verifier;
    std::string state;
    std::string url;
};

PasteFlow anthropic_authorize_url();
std::string anthropic_exchange(nlohmann::json& account, const std::string& pasted, const PasteFlow& flow);

// xAI Grok CLI public client, loopback on 127.0.0.1:56121.
std::string xai_sign_in(nlohmann::json& account, const std::function<void(const std::string&)>& progress);

AccountView refresh_claude(nlohmann::json& account);
AccountView refresh_grok(nlohmann::json& account);
AccountView refresh_openai(nlohmann::json& account);
AccountView refresh_github(nlohmann::json& account);
AccountView refresh_codex(nlohmann::json& account);

// Pull Grok, Codex and GitHub from sessions already on this machine.
// Returns how many accounts were added. Never logs tokens.
int import_local_accounts(nlohmann::json& accounts);

// Re-read a linked session (Hermes, Codex) so a refresh elsewhere is picked up
// without rotating that app's token.
bool reload_linked_session(nlohmann::json& account);
