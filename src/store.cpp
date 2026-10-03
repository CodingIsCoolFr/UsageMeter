#include "store.hpp"

#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>

#include <fstream>
#include <vector>

namespace {

std::string protect(const std::string& plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"UsageMeter", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string blob(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return blob;
}

std::string unprotect(const std::string& blob) {
    DATA_BLOB in{static_cast<DWORD>(blob.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return {};
    }
    std::string plain(reinterpret_cast<char*>(out.pbData), out.cbData);
    LocalFree(out.pbData);
    return plain;
}

}  // namespace

std::string default_store_path() {
    wchar_t buf[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return "accounts.json";
    std::wstring dir = std::wstring(buf) + L"\\UsageMeter";
    CreateDirectoryW(dir.c_str(), nullptr);
    int n = WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\accounts.json").c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\accounts.json").c_str(), -1, path.data(), n, nullptr, nullptr);
    return path;
}

Store::Store(std::string path) : path_(std::move(path)) {}

bool Store::load() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) return false;
    std::string blob((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (blob.empty()) return false;
    std::string plain = unprotect(blob);
    if (plain.empty()) return false;
    try {
        data_ = nlohmann::json::parse(plain);
    } catch (...) {
        return false;
    }
    return data_.is_object();
}

bool Store::save() const {
    std::string blob = protect(data_.dump());
    if (blob.empty()) return false;
    std::ofstream out(path_, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(blob.data(), static_cast<std::streamsize>(blob.size()));
    return static_cast<bool>(out);
}
