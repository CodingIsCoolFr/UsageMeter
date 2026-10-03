#include "store.hpp"
#include "ui.hpp"

#include <algorithm>

namespace {

int run() {
    AppState app(default_store_path());
    app.store.load();
    if (!app.store.data().contains("accounts") || !app.store.data()["accounts"].is_array())
        app.store.data()["accounts"] = nlohmann::json::array();

    auto& accounts = app.store.data()["accounts"];
    auto before = accounts.size();
    accounts.erase(std::remove_if(accounts.begin(), accounts.end(),
                                  [](const nlohmann::json& a) {
                                      return a.value("provider", "") == "codex" && a.value("source", "") == "codex";
                                  }),
                   accounts.end());
    if (accounts.size() != before) app.store.save();

    if (import_local_accounts(app.store.data()["accounts"]) > 0) app.store.save();

    load_saved_views(app);
    start_ui(app);
    return 0;
}

}  // namespace

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return run(); }
#else
int main() { return run(); }
#endif
