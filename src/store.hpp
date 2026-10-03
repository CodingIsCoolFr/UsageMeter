#pragma once

#include <nlohmann/json.hpp>

#include <string>

// DPAPI-encrypted account store at %APPDATA%\UsageMeter\accounts.json.
// Tokens never touch disk in the clear and are never logged.
class Store {
public:
    explicit Store(std::string path);

    nlohmann::json& data() { return data_; }
    const nlohmann::json& data() const { return data_; }

    bool load();
    bool save() const;

    std::string path() const { return path_; }

private:
    std::string path_;
    nlohmann::json data_ = nlohmann::json::object();
};

std::string default_store_path();
