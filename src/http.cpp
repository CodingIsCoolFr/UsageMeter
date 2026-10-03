#include "http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fstream>
#endif

namespace {

std::once_flag g_curl_once;

void curl_init_once() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

size_t write_body(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t write_header(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* headers = static_cast<std::map<std::string, std::string>*>(userdata);
    std::string line(ptr, size * nmemb);
    auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string name = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.erase(value.begin());
        while (!value.empty() && (value.back() == '\r' || value.back() == '\n' || value.back() == ' '))
            value.pop_back();
        for (char& c : name) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        (*headers)[name] = value;
    }
    return size * nmemb;
}

}  // namespace

HttpResponse http_request(const HttpRequest& req) {
    std::call_once(g_curl_once, curl_init_once);
    HttpResponse res;

    CURL* curl = curl_easy_init();
    if (!curl) {
        res.error = "curl_easy_init failed";
        return res;
    }

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, write_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &res.headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, req.timeout_s);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "UsageMeter/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

    if (!req.cookie_jar.empty()) {
        curl_easy_setopt(curl, CURLOPT_COOKIEFILE, req.cookie_jar.c_str());
        curl_easy_setopt(curl, CURLOPT_COOKIEJAR, req.cookie_jar.c_str());
    }
    if (!req.body.empty() || req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    }

    curl_slist* hdrs = nullptr;
    for (const auto& h : req.headers) hdrs = curl_slist_append(hdrs, h.c_str());
    if (hdrs) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        res.error = curl_easy_strerror(rc);
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &res.status);
    }

    if (hdrs) curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return res;
}

std::string b64url(const unsigned char* data, size_t len) {
    static const char* kTable =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= len) {
        unsigned n = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(kTable[(n >> 6) & 63]);
        out.push_back(kTable[n & 63]);
        i += 3;
    }
    if (i < len) {
        unsigned n = data[i] << 16;
        if (i + 1 < len) n |= data[i + 1] << 8;
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        if (i + 1 < len) out.push_back(kTable[(n >> 6) & 63]);
    }
    return out;
}

std::string random_b64url(size_t bytes) {
    std::vector<unsigned char> buf(bytes);
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, buf.data(), static_cast<ULONG>(buf.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        return {};
    }
#else
    std::ifstream rnd("/dev/urandom", std::ios::binary);
    rnd.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
    if (!rnd) return {};
#endif
    return b64url(buf.data(), buf.size());
}

std::string sha256_b64url(const std::string& input) {
#ifdef _WIN32
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::string out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return out;

    DWORD hash_len = 0, cb = 0;
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hash_len), sizeof(hash_len), &cb, 0);
    std::vector<unsigned char> digest(hash_len);

    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(input.data())),
                       static_cast<ULONG>(input.size()), 0) == 0 &&
        BCryptFinishHash(hash, digest.data(), hash_len, 0) == 0) {
        out = b64url(digest.data(), digest.size());
    }
    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(alg, 0);
    return out;
#else
    // Compact public-domain SHA-256 so sign-in works without a Windows crypto API.
    struct Sha {
        uint32_t s[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        uint64_t bits = 0;
        unsigned char buf[64]{};
        size_t n = 0;
        static uint32_t ro(uint32_t x, int c) { return (x >> c) | (x << (32 - c)); }
        void block(const unsigned char* p) {
            static const uint32_t k[64] = {
                0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
                0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
                0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
                0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
                0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
                0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
                0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
                0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
            uint32_t w[64];
            for (int i = 0; i < 16; ++i)
                w[i] = (p[i * 4] << 24) | (p[i * 4 + 1] << 16) | (p[i * 4 + 2] << 8) | p[i * 4 + 3];
            for (int i = 16; i < 64; ++i) {
                uint32_t s0 = ro(w[i - 15], 7) ^ ro(w[i - 15], 18) ^ (w[i - 15] >> 3);
                uint32_t s1 = ro(w[i - 2], 17) ^ ro(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }
            uint32_t a = s[0], b = s[1], c = s[2], d = s[3], e = s[4], f = s[5], g = s[6], h = s[7];
            for (int i = 0; i < 64; ++i) {
                uint32_t S1 = ro(e, 6) ^ ro(e, 11) ^ ro(e, 25);
                uint32_t ch = (e & f) ^ (~e & g);
                uint32_t t1 = h + S1 + ch + k[i] + w[i];
                uint32_t S0 = ro(a, 2) ^ ro(a, 13) ^ ro(a, 22);
                uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + S0 + maj;
            }
            s[0] += a; s[1] += b; s[2] += c; s[3] += d; s[4] += e; s[5] += f; s[6] += g; s[7] += h;
        }
        void add(const unsigned char* p, size_t len) {
            bits += len * 8;
            while (len) {
                size_t take = std::min(len, 64 - n);
                memcpy(buf + n, p, take);
                n += take; p += take; len -= take;
                if (n == 64) { block(buf); n = 0; }
            }
        }
        void finish(unsigned char out[32]) {
            buf[n++] = 0x80;
            if (n > 56) { while (n < 64) buf[n++] = 0; block(buf); n = 0; }
            while (n < 56) buf[n++] = 0;
            for (int i = 7; i >= 0; --i) buf[n++] = static_cast<unsigned char>(bits >> (i * 8));
            block(buf);
            for (int i = 0; i < 8; ++i) {
                out[i * 4] = static_cast<unsigned char>(s[i] >> 24);
                out[i * 4 + 1] = static_cast<unsigned char>(s[i] >> 16);
                out[i * 4 + 2] = static_cast<unsigned char>(s[i] >> 8);
                out[i * 4 + 3] = static_cast<unsigned char>(s[i]);
            }
        }
    } sha;
    sha.add(reinterpret_cast<const unsigned char*>(input.data()), input.size());
    unsigned char dig[32];
    sha.finish(dig);
    return b64url(dig, 32);
#endif
}
