#include "store.hpp"

#include <fstream>
#include <cstdlib>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <Security/Security.h>
#endif
#endif

namespace {

#ifdef _WIN32
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

#else
#ifdef __APPLE__
std::string protect(const std::string& plain) {
    CFDataRef secret = CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(plain.data()),
                                    static_cast<CFIndex>(plain.size()));
    if (!secret) return {};
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
    const void* vals[] = {kSecClassGenericPassword, CFSTR("UsageMeter"), CFSTR("accounts")};
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 3,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    SecItemDelete(query);
    const void* add_keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData, kSecAttrAccessible};
    const void* add_vals[] = {kSecClassGenericPassword, CFSTR("UsageMeter"), CFSTR("accounts"), secret,
                              kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly};
    CFDictionaryRef add = CFDictionaryCreate(kCFAllocatorDefault, add_keys, add_vals, 5,
                                             &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    OSStatus st = SecItemAdd(add, nullptr);
    CFRelease(add);
    CFRelease(query);
    CFRelease(secret);
    return st == errSecSuccess ? std::string("keychain") : std::string();
}

std::string unprotect(const std::string&) {
    const void* keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData};
    const void* vals[] = {kSecClassGenericPassword, CFSTR("UsageMeter"), CFSTR("accounts"), kCFBooleanTrue};
    CFDictionaryRef query = CFDictionaryCreate(kCFAllocatorDefault, keys, vals, 4,
                                               &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    CFRelease(query);
    if (st != errSecSuccess || !result) return {};
    auto* data = static_cast<CFDataRef>(result);
    std::string plain(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                      static_cast<size_t>(CFDataGetLength(data)));
    CFRelease(data);
    return plain;
}
#else
std::string protect(const std::string& plain) { return plain; }
std::string unprotect(const std::string& blob) { return blob; }
#endif
}  // namespace
#endif

std::string default_store_path() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_APPDATA, nullptr, 0, buf))) return "accounts.json";
    std::wstring dir = std::wstring(buf) + L"\\UsageMeter";
    CreateDirectoryW(dir.c_str(), nullptr);
    int n = WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\accounts.json").c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string path(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, (dir + L"\\accounts.json").c_str(), -1, path.data(), n, nullptr, nullptr);
    return path;
#else
    const char* home = std::getenv("HOME");
    std::string dir = home ? std::string(home) + "/Library/Application Support/UsageMeter"
                           : "UsageMeter";
    mkdir(dir.c_str(), 0700);
    return dir + "/accounts.json";
#endif
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
#ifndef _WIN32
    chmod(path_.c_str(), 0600);
#endif
    return static_cast<bool>(out);
}
