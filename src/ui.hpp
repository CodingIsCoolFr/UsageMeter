#pragma once

#include "providers.hpp"
#include "store.hpp"

#include <mutex>
#include <string>

// Everything the UI thread reads. Background work copies what it needs and
// writes results back under the mutex.
struct AppState {
    Store store;

    std::mutex mu;
    std::vector<AccountView> views;
    std::string status;  // one line under the toolbar

    // Sign-in dialog.
    bool signing_in = false;
    std::string sign_in_provider;
    std::string sign_in_prompt;
    std::string sign_in_input;  // paste buffer, owned by the UI thread
    bool sign_in_needs_paste = false;

    explicit AppState(std::string store_path) : store(std::move(store_path)) {}
};

void start_ui(AppState& app);
void load_saved_views(AppState& app);
