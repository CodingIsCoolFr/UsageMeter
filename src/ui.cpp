#include "ui.hpp"

#include "jsonutil.hpp"

#include <windows.h>
#include <shellapi.h>

#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <map>
#include <thread>

namespace {

ImVec4 bar_color(double pct) {
    if (pct < 0) return ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    if (pct >= 95.0) return ImVec4(0.72f, 0.36f, 0.34f, 1.0f);
    if (pct >= 75.0) return ImVec4(0.72f, 0.58f, 0.32f, 1.0f);
    return ImVec4(0.82f, 0.82f, 0.80f, 1.0f);
}

void apply_theme() {
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.FrameRounding = 6.0f;
    s.ChildRounding = 8.0f;
    s.PopupRounding = 8.0f;
    s.GrabRounding = 6.0f;
    s.WindowPadding = ImVec2(16, 14);
    s.FramePadding = ImVec2(10, 6);
    s.ItemSpacing = ImVec2(10, 8);
    s.WindowBorderSize = 0.0f;
    s.ChildBorderSize = 1.0f;
    s.FrameBorderSize = 0.0f;

    ImVec4* c = s.Colors;
    const ImVec4 base(0.071f, 0.071f, 0.071f, 1.0f);     // #121212
    const ImVec4 card(0.110f, 0.110f, 0.110f, 1.0f);     // #1C1C1C
    const ImVec4 raised(0.145f, 0.145f, 0.145f, 1.0f);   // #252525
    const ImVec4 line(0.180f, 0.180f, 0.180f, 1.0f);     // #2E2E2E
    const ImVec4 text(0.910f, 0.910f, 0.910f, 1.0f);     // #E8E8E8
    const ImVec4 muted(0.640f, 0.640f, 0.640f, 1.0f);    // #A3A3A3

    c[ImGuiCol_WindowBg] = base;
    c[ImGuiCol_ChildBg] = card;
    c[ImGuiCol_PopupBg] = ImVec4(0.165f, 0.165f, 0.165f, 1.0f);
    c[ImGuiCol_Border] = line;
    c[ImGuiCol_Separator] = line;
    c[ImGuiCol_FrameBg] = ImVec4(0.09f, 0.09f, 0.09f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = raised;
    c[ImGuiCol_FrameBgActive] = raised;
    c[ImGuiCol_Button] = raised;
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    c[ImGuiCol_Header] = raised;
    c[ImGuiCol_HeaderHovered] = ImVec4(0.20f, 0.20f, 0.20f, 1.0f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.25f, 0.25f, 0.25f, 1.0f);
    c[ImGuiCol_Text] = text;
    c[ImGuiCol_TextDisabled] = muted;
    c[ImGuiCol_CheckMark] = text;
    c[ImGuiCol_SliderGrab] = text;
    c[ImGuiCol_ScrollbarBg] = base;
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.28f, 0.28f, 0.28f, 1.0f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.36f, 0.36f, 0.36f, 1.0f);
    c[ImGuiCol_TitleBg] = base;
    c[ImGuiCol_TitleBgActive] = base;
    c[ImGuiCol_ResizeGrip] = line;
}

void meter_bar(const Meter& m) {
    ImGui::PushID(m.label.c_str());
    ImGui::TextUnformatted(m.label.c_str());

    char right[64] = "";
    if (m.unknown)
        snprintf(right, sizeof(right), "not reported");
    else if (m.used_pct >= 0)
        snprintf(right, sizeof(right), "%.0f%%", m.used_pct);

    ImVec2 pos = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = 18.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImU32 track = ImGui::GetColorU32(ImVec4(0.07f, 0.08f, 0.11f, 1.0f));
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + height), track, 4.0f);
    if (m.used_pct >= 0) {
        float fill = width * static_cast<float>(std::min(100.0, std::max(0.0, m.used_pct)) / 100.0);
        if (fill > 0.5f)
            dl->AddRectFilled(pos, ImVec2(pos.x + fill, pos.y + height), ImGui::GetColorU32(bar_color(m.used_pct)), 4.0f);
    }
    if (right[0]) {
        ImVec2 ts = ImGui::CalcTextSize(right);
        dl->AddText(ImVec2(pos.x + width - ts.x - 8, pos.y + (height - ts.y) / 2),
                    ImGui::GetColorU32(ImGuiCol_Text), right);
    }
    ImGui::Dummy(ImVec2(width, height));
    std::string caption = m.detail;
    if (!m.resets_at.empty()) {
        std::string left = resets_in(m.resets_at);
        std::string when = format_local(m.resets_at);
        caption = left;
        if (!when.empty()) caption += std::string("  ·  ") + when;
        if (!m.detail.empty() && m.detail.rfind("resets", 0) != 0) caption += "  ·  " + m.detail;
    }
    if (!caption.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted(caption.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
}

void apply_fresh(AppState& app, const nlohmann::json& snapshot, AccountView fresh);

void account_card(AppState& app, const AccountView& v) {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 14));
    ImGui::BeginChild(v.id.c_str(), ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
    ImGui::PopStyleVar();

    ImGui::TextUnformatted(v.provider.c_str());
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    std::string subtitle = v.plan;
    if (!v.identity.empty()) subtitle += (subtitle.empty() ? "" : "  ·  ") + v.identity;
    ImGui::TextUnformatted(subtitle.c_str());
    ImGui::PopStyleColor();

    ImGui::SameLine(ImGui::GetContentRegionAvail().x - 130);
    if (ImGui::SmallButton("Refresh")) {
        std::thread([&app, id = v.id] {
            nlohmann::json snapshot;
            std::string provider;
            {
                std::lock_guard<std::mutex> lock(app.mu);
                for (auto& a : app.store.data()["accounts"])
                    if (a.value("id", "") == id) {
                        snapshot = a;
                        provider = a.value("provider", "");
                    }
            }
            const Provider* p = find_provider(provider);
            AccountView fresh = p ? p->refresh(snapshot) : AccountView{};
            fresh.id = id;
            std::lock_guard<std::mutex> lock(app.mu);
            apply_fresh(app, snapshot, fresh);
            app.store.save();
        }).detach();
    }
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.28f, 0.16f, 0.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.40f, 0.20f, 0.18f, 1.0f));
    if (ImGui::SmallButton("Remove")) {
        std::lock_guard<std::mutex> lock(app.mu);
        auto& accounts = app.store.data()["accounts"];
        accounts.erase(std::remove_if(accounts.begin(), accounts.end(),
                                      [&](const nlohmann::json& a) { return a.value("id", "") == v.id; }),
                       accounts.end());
        app.store.save();
        app.views.erase(std::remove_if(app.views.begin(), app.views.end(),
                                       [&](const AccountView& a) { return a.id == v.id; }),
                        app.views.end());
    }
    ImGui::PopStyleColor(2);

    if (!v.meters.empty()) {
        for (const auto& m : v.meters) {
            ImGui::Spacing();
            meter_bar(m);
        }
    } else if (v.busy) {
        ImGui::TextDisabled("Loading…");
    } else if (!v.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.93f, 0.45f, 0.42f, 1.0f));
        ImGui::TextWrapped("%s", v.error.c_str());
        ImGui::PopStyleColor();
    } else {
        ImGui::TextDisabled("No usage reported yet.");
    }
    if (!v.note.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", v.note.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::Spacing();
}

std::string new_id() {
    return std::to_string(static_cast<long long>(std::time(nullptr))) + "-" +
           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count() % 100000);
}

std::string pretty_provider(const std::string& id) {
    if (id == "claude") return "Claude";
    if (id == "grok") return "Grok";
    if (id == "codex") return "Codex";
    if (id == "github") return "GitHub";
    if (id == "openai") return "OpenAI";
    return id;
}

AccountView view_from_account(const nlohmann::json& account) {
    AccountView v;
    v.id = account.value("id", "");
    v.provider = pretty_provider(account.value("provider", ""));
    v.plan = account.value("plan", "");
    if (account.contains("cached_meters") && account["cached_meters"].is_array()) {
        for (const auto& m : account["cached_meters"]) {
            Meter meter;
            meter.label = m.value("label", "");
            meter.used_pct = m.value("used_pct", -1.0);
            meter.detail = m.value("detail", "");
            meter.resets_at = m.value("resets_at", "");
            v.meters.push_back(meter);
        }
    }
    v.busy = v.meters.empty();
    return v;
}

std::atomic<bool> g_refreshing{false};

void remember(nlohmann::json& account, const AccountView& fresh) {
    if (fresh.error.empty() && !fresh.meters.empty()) {
        nlohmann::json cached = nlohmann::json::array();
        for (const auto& m : fresh.meters) {
            cached.push_back({{"label", m.label},
                              {"used_pct", m.used_pct},
                              {"detail", m.detail},
                              {"resets_at", m.resets_at}});
        }
        account["cached_meters"] = cached;
        if (!fresh.plan.empty()) account["plan"] = fresh.plan;
    }
}

void apply_fresh(AppState& app, const nlohmann::json& snapshot, AccountView fresh) {
    for (auto& a : app.store.data()["accounts"]) {
        if (a.value("id", "") != fresh.id) continue;
        nlohmann::json merged = snapshot;
        remember(merged, fresh);
        if (a.contains("cached_meters") && !merged.contains("cached_meters"))
            merged["cached_meters"] = a["cached_meters"];
        a = std::move(merged);
    }
    bool found = false;
    for (auto& existing : app.views) {
        if (existing.id != fresh.id) continue;
        found = true;
        if (!fresh.error.empty() && !existing.meters.empty()) {
            existing.busy = false;
            existing.error.clear();
            if (fresh.error.find("429") == std::string::npos)
                existing.note = "Retrying — " + fresh.error;
            else
                existing.note.clear();
        } else {
            existing = fresh;
        }
    }
    if (!found) app.views.push_back(fresh);
}

void refresh_all(AppState& app) {
    if (g_refreshing.exchange(true)) return;
    std::vector<nlohmann::json> snapshots;
    {
        std::lock_guard<std::mutex> lock(app.mu);
        if (!app.store.data().contains("accounts")) {
            g_refreshing = false;
            return;
        }
        for (auto& a : app.store.data()["accounts"]) snapshots.push_back(a);
        for (auto& v : app.views)
            if (v.meters.empty()) v.busy = true;
    }
    std::thread([&app, snapshots = std::move(snapshots)]() mutable {
        using clock = std::chrono::steady_clock;
        static std::map<std::string, clock::time_point> next_poll;
        const auto now = clock::now();
        for (auto& snapshot : snapshots) {
            const std::string id = snapshot.value("id", "");
            if (next_poll.count(id) && now < next_poll[id]) continue;
            const Provider* p = find_provider(snapshot.value("provider", ""));
            if (!p) continue;
            AccountView fresh = p->refresh(snapshot);
            fresh.id = id;
            const bool hit_limit = fresh.error.find("429") != std::string::npos ||
                                   fresh.error.find("rate limit") != std::string::npos;
            // Every provider, not just Claude. Countdowns still tick every second;
            // the usage endpoints do not, and polling them faster is what got us limited.
            const int wait = hit_limit ? 900 : 180;
            next_poll[id] = now + std::chrono::seconds(wait);
            {
                std::lock_guard<std::mutex> lock(app.mu);
                apply_fresh(app, snapshot, fresh);
            }
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
        std::lock_guard<std::mutex> lock(app.mu);
        app.store.save();
        app.status.clear();
        g_refreshing = false;
    }).detach();
}

void commit_account(AppState& app, nlohmann::json account) {
    std::lock_guard<std::mutex> lock(app.mu);
    app.store.data()["accounts"].push_back(account);
    app.store.save();
    AccountView v;
    v.id = account.value("id", "");
    v.provider = account.value("provider", "");
    v.busy = true;
    app.views.push_back(v);
    app.signing_in = false;
    app.status.clear();
}

void add_account(AppState& app, nlohmann::json account) {
    account["id"] = new_id();
    commit_account(app, account);
    std::thread([&app, account = std::move(account)]() mutable {
        const Provider* p = find_provider(account.value("provider", ""));
        AccountView fresh = p ? p->refresh(account) : AccountView{};
        fresh.id = account.value("id", "");
        std::lock_guard<std::mutex> lock(app.mu);
        for (auto& a : app.store.data()["accounts"])
            if (a.value("id", "") == fresh.id) a = account;
        app.store.save();
        for (auto& existing : app.views)
            if (existing.id == fresh.id) existing = fresh;
    }).detach();
}

// Claude's copy-paste sign-in. The browser opens on the button; the exchange
// happens once the user pastes the code#state Claude prints.
void claude_dialog(AppState& app, PasteFlow& flow, bool& flow_ready) {
    ImGui::OpenPopup("Sign in to Claude");
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460, 0));
    if (!ImGui::BeginPopupModal("Sign in to Claude", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("Claude opens in your browser. Approve access, then paste the code it shows you here.");
    ImGui::Spacing();
    if (!flow_ready) {
        flow = anthropic_authorize_url();
        flow_ready = true;
    }
    if (ImGui::Button("Open browser", ImVec2(140, 0))) {
        ShellExecuteA(nullptr, "open", flow.url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::Spacing();
    ImGui::TextDisabled("Paste the code");
    static char buf[2048] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##code", buf, sizeof(buf));
    ImGui::Spacing();

    if (ImGui::Button("Sign in", ImVec2(120, 0))) {
        std::string pasted = buf;
        PasteFlow captured = flow;
        std::thread([&app, pasted, captured] {
            nlohmann::json account = {{"provider", "claude"}};
            std::string err = anthropic_exchange(account, pasted, captured);
            if (!err.empty()) {
                std::lock_guard<std::mutex> lock(app.mu);
                app.status = err;
                app.signing_in = false;
                return;
            }
            add_account(app, std::move(account));
        }).detach();
        buf[0] = '\0';
        flow_ready = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        flow_ready = false;
        app.signing_in = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void key_dialog(AppState& app, const char* title, const char* provider, const char* field, const char* hint) {
    ImGui::OpenPopup(title);
    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(440, 0));
    if (!ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::TextWrapped("%s", hint);
    ImGui::Spacing();
    static char buf[512] = "";
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##key", buf, sizeof(buf), ImGuiInputTextFlags_Password);
    ImGui::Spacing();
    if (ImGui::Button("Add", ImVec2(120, 0)) && buf[0]) {
        nlohmann::json account = {{"provider", provider}, {field, std::string(buf)}};
        add_account(app, std::move(account));
        buf[0] = '\0';
        app.signing_in = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) {
        buf[0] = '\0';
        app.signing_in = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}  // namespace

void load_saved_views(AppState& app) {
    for (const auto& a : app.store.data()["accounts"]) app.views.push_back(view_from_account(a));
}

void start_ui(AppState& app) {
    glfwInit();
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    GLFWwindow* window = glfwCreateWindow(560, 760, "UsageMeter", nullptr, nullptr);
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    HWND native = glfwGetWin32Window(window);
    BOOL use_dark = TRUE;
    DwmSetWindowAttribute(native, 20, &use_dark, sizeof(use_dark));
    // Match the client background (#121212) so the caption is not a separate bar.
    COLORREF caption = 0x00121212;
    COLORREF text = 0x00E8E8E8;
    int backdrop_none = 1;
    DwmSetWindowAttribute(native, 34, &caption, sizeof(caption));
    DwmSetWindowAttribute(native, 35, &caption, sizeof(caption));
    DwmSetWindowAttribute(native, 36, &text, sizeof(text));
    DwmSetWindowAttribute(native, 38, &backdrop_none, sizeof(backdrop_none));

    bool pinned = false;
    {
        std::lock_guard<std::mutex> lock(app.mu);
        pinned = app.store.data().value("pinned", false);
    }
    if (pinned) glfwSetWindowAttrib(window, GLFW_FLOATING, GLFW_TRUE);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;

    float scale = 1.0f;
    glfwGetWindowContentScale(window, &scale, nullptr);
    io.FontGlobalScale = 1.0f;
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", 18.0f * scale);
    if (!font) io.Fonts->AddFontDefault();
    ImGui::GetStyle().ScaleAllSizes(scale);

    apply_theme();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 130");

    refresh_all(app);

    PasteFlow claude_flow;
    bool claude_flow_ready = false;
    auto last_refresh = std::chrono::steady_clock::now();

    while (!glfwWindowShouldClose(window)) {
        glfwPollEvents();
        if (std::chrono::steady_clock::now() - last_refresh > std::chrono::seconds(15)) {
            refresh_all(app);
            last_refresh = std::chrono::steady_clock::now();
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##root", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextUnformatted("Usage");
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextUnformatted("Claude, Grok and anything else you sign in");
        ImGui::PopStyleColor();
        ImGui::Spacing();

        if (ImGui::Button("Add account")) ImGui::OpenPopup("add-account");
        ImGui::SameLine();
        if (ImGui::Button("Refresh all")) {
            refresh_all(app);
            last_refresh = std::chrono::steady_clock::now();
        }
        ImGui::SameLine();
        if (ImGui::Button(pinned ? "Unpin" : "Pin")) {
            pinned = !pinned;
            glfwSetWindowAttrib(window, GLFW_FLOATING, pinned ? GLFW_TRUE : GLFW_FALSE);
            std::lock_guard<std::mutex> lock(app.mu);
            app.store.data()["pinned"] = pinned;
            app.store.save();
        }
        {
            std::lock_guard<std::mutex> lock(app.mu);
            if (!app.status.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", app.status.c_str());
            }
        }

        if (ImGui::BeginPopup("add-account")) {
            ImGui::TextDisabled("SIGN IN");
            for (const auto& p : providers()) {
                if (ImGui::Selectable(p.name.c_str())) {
                    if (p.id == "claude") {
                        app.signing_in = true;
                        app.sign_in_provider = "claude";
                    } else if (p.id == "grok" || p.id == "codex" || p.id == "github") {
                        int added = 0;
                        {
                            std::lock_guard<std::mutex> lock(app.mu);
                            added = import_local_accounts(app.store.data()["accounts"]);
                            if (added > 0) {
                                app.store.save();
                                app.views.clear();
                                for (const auto& a : app.store.data()["accounts"])
                                    app.views.push_back(view_from_account(a));
                            }
                        }
                        if (added > 0) refresh_all(app);
                        if (p.id == "grok") {
                            bool have = false;
                            {
                                std::lock_guard<std::mutex> lock(app.mu);
                                for (const auto& a : app.store.data()["accounts"])
                                    if (a.value("provider", "") == "grok") have = true;
                            }
                            if (!have) {
                                app.signing_in = true;
                                std::thread([&app] {
                                    nlohmann::json account = {{"provider", "grok"}, {"kind", "oauth"}};
                                    std::string err = xai_sign_in(account, [&app](const std::string& msg) {
                                        std::lock_guard<std::mutex> lock(app.mu);
                                        app.status = msg;
                                    });
                                    if (!err.empty()) {
                                        std::lock_guard<std::mutex> lock(app.mu);
                                        app.status = err;
                                        app.signing_in = false;
                                        return;
                                    }
                                    add_account(app, std::move(account));
                                }).detach();
                            }
                        }
                    } else if (p.id == "openai") {
                        app.signing_in = true;
                        app.sign_in_provider = "openai";
                    } else if (p.id == "github") {
                        app.signing_in = true;
                        app.sign_in_provider = "github";
                    }
                }
                ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                ImGui::TextWrapped("%s", p.blurb.c_str());
                ImGui::PopStyleColor();
                ImGui::Spacing();
            }
            ImGui::EndPopup();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        std::vector<AccountView> snapshot;
        {
            std::lock_guard<std::mutex> lock(app.mu);
            snapshot = app.views;
        }
        if (snapshot.empty()) {
            ImGui::TextDisabled("No accounts yet. Add one to see its limits.");
        }
        ImGui::BeginChild("##cards", ImVec2(0, 0), ImGuiChildFlags_None);
        for (const auto& v : snapshot) account_card(app, v);
        ImGui::EndChild();

        if (app.signing_in && app.sign_in_provider == "claude")
            claude_dialog(app, claude_flow, claude_flow_ready);
        if (app.signing_in && app.sign_in_provider == "openai")
            key_dialog(app, "OpenAI admin key", "openai", "admin_key",
                       "From platform.openai.com → Settings → Admin keys. Read-only is enough.");
        if (app.signing_in && app.sign_in_provider == "github")
            key_dialog(app, "GitHub token", "github", "token",
                       "A personal access token (classic or fine-grained). No scopes are required to read rate limits.");

        ImGui::End();
        ImGui::Render();
        int w = 0, h = 0;
        glfwGetFramebufferSize(window, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.071f, 0.071f, 0.071f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}
