#pragma once
// Minimal header-only base64 codec. Used by ProjectSerializer to
// embed CLAP plugin state blobs (which contain arbitrary binary data,
// including 0x22 " and 0x5C \ bytes) safely inside JSON strings.
//
// RFC 4648 §4 standard alphabet. No URL-safe variant. No line breaks.

#include <string>
#include <cstdint>
#include <vector>

namespace base64 {

inline std::string encode(const std::string& in) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 3 <= in.size()) {
        uint32_t v = (uint8_t)in[i] << 16 | (uint8_t)in[i+1] << 8 | (uint8_t)in[i+2];
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += tbl[(v >> 6)  & 0x3F];
        out += tbl[ v        & 0x3F];
        i += 3;
    }
    if (i < in.size()) {
        uint32_t v = (uint8_t)in[i] << 16;
        if (i + 1 < in.size()) v |= (uint8_t)in[i+1] << 8;
        out += tbl[(v >> 18) & 0x3F];
        out += tbl[(v >> 12) & 0x3F];
        out += (i + 1 < in.size()) ? tbl[(v >> 6) & 0x3F] : '=';
        out += '=';
    }
    return out;
}

inline std::string decode(const std::string& in) {
    // Build reverse table on first use (static-init order is not an
    // issue because we use a function-local lambda).
    static int8_t rev[256];
    static bool init = false;
    if (!init) {
        for (int j = 0; j < 256; ++j) rev[j] = -1;
        const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int j = 0; j < 64; ++j) rev[(uint8_t)tbl[j]] = (int8_t)j;
        init = true;
    }
    std::string out;
    out.reserve((in.size() / 4) * 3);
    uint32_t buf = 0;
    int bits = 0;
    for (char c : in) {
        if (c == '=' || c == '\n' || c == '\r') break;
        int8_t v = rev[(uint8_t)c];
        if (v < 0) continue;  // skip unknown chars (defensive)
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((buf >> bits) & 0xFF);
        }
    }
    return out;
}

} // namespace base64
