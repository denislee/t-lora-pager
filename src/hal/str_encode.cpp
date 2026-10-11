#include "str_encode.h"

#include <cstdio>
#include <cstdlib>

#ifdef ARDUINO
#include <mbedtls/base64.h>
#endif

namespace hal {

std::string json_escape(const std::string &in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char b[8];
                    snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                    out += b;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string url_encode(const std::string &in)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(in.size() * 3);
    for (unsigned char c : in) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back((char)c);
        } else {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0xF]);
        }
    }
    return out;
}

std::string trim(const std::string &in)
{
    static const char ws[] = " \t\r\n";
    size_t a = in.find_first_not_of(ws);
    if (a == std::string::npos) return std::string();
    size_t b = in.find_last_not_of(ws);
    return in.substr(a, b - a + 1);
}

bool parse_url(const std::string &url, UrlParts &out)
{
    std::string rest = url;
    out.scheme.clear();
    size_t scheme_end = url.find("://");
    if (scheme_end != std::string::npos) {
        out.scheme = url.substr(0, scheme_end);
        rest = url.substr(scheme_end + 3);
    }
    out.port = (out.scheme == "https") ? 443 : 80;

    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    out.path = (slash == std::string::npos) ? "/" : rest.substr(slash);

    size_t colon = hostport.find(':');
    out.host = hostport.substr(0, colon);
    if (colon != std::string::npos) {
        long p = strtol(hostport.c_str() + colon + 1, nullptr, 10);
        if (p > 0 && p < 65536) out.port = (uint16_t)p;
    }
    return !out.host.empty();
}

uint32_t utf8_byte_pos(const char *txt, uint32_t char_pos)
{
    uint32_t bp = 0;
    while (char_pos > 0 && txt[bp] != '\0') {
        uint8_t c = (uint8_t)txt[bp];
        if      ((c & 0x80) == 0x00) bp += 1;
        else if ((c & 0xE0) == 0xC0) bp += 2;
        else if ((c & 0xF0) == 0xE0) bp += 3;
        else if ((c & 0xF8) == 0xF0) bp += 4;
        else                         bp += 1;
        char_pos--;
    }
    return bp;
}

#ifdef ARDUINO
bool base64_encode(const uint8_t *data, size_t len, std::string &out)
{
    size_t olen = 0;
    // Probe first to size the buffer — mbedtls writes the required length
    // into olen when dst is NULL/undersized.
    mbedtls_base64_encode(nullptr, 0, &olen, data, len);
    out.resize(olen);
    size_t written = 0;
    int rc = mbedtls_base64_encode((unsigned char *)&out[0], out.size(),
                                   &written, data, len);
    if (rc != 0) { out.clear(); return false; }
    // The returned byte count excludes the null terminator — resize to the
    // actual encoded length.
    out.resize(written);
    return true;
}
#endif

} // namespace hal
