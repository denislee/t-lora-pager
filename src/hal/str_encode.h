#pragma once

// Shared string-encoding helpers for building HTTP/JSON payloads. These used
// to be copy-pasted (byte-for-byte) across ui_chat.cpp, ui_notes_sync.cpp,
// ui_weather.cpp and hub.cpp — consolidated here per OPTIMIZATION_REPORT §2.18.
// They live in HAL because hub.cpp (HAL) is one consumer and apps may depend
// on HAL, but never the reverse.

#include <cstddef>
#include <cstdint>
#include <string>

namespace hal {

// Minimal JSON string escaper: escapes the characters JSON forbids verbatim
// (" \ and the C0 control bytes, with \n \r \t spelled out). Enough for the
// paths, commit messages and base64 blobs we put in request bodies.
std::string json_escape(const std::string &in);

// Percent-encode a value for use in a URL query. Unreserved characters
// (RFC 3986: A–Z a–z 0–9 - _ . ~) pass through; everything else, including
// UTF-8 bytes, is %XX-encoded.
std::string url_encode(const std::string &in);

// Strip leading and trailing whitespace (space, \t, \r, \n). An
// all-whitespace input yields "". Keyboard / paste flows routinely append a
// trailing newline or space to URLs, tokens and prompts.
std::string trim(const std::string &in);

// The pieces of an `[scheme://]host[:port][/path]` URL.
struct UrlParts {
    std::string scheme;  // "" when the URL has no "scheme://"
    std::string host;
    uint16_t    port;    // explicit port, else 443 for https, else 80
    std::string path;    // "/" when absent
};

// Split `url` into UrlParts. An explicit port outside 1..65535 (or not a
// number) is ignored and the scheme default kept. Returns false when the
// host is empty. Used by hub_is_reachable and the chat app's raw-socket POST.
bool parse_url(const std::string &url, UrlParts &out);

// Byte offset of the `char_pos`-th UTF-8 character in the NUL-terminated
// `txt` (lead-byte length; a stray continuation/invalid byte counts as one
// character). Stops at a terminator met on a character boundary, so an
// out-of-range position on well-formed text yields strlen(txt). Used by the keyboard / rotary word-jump and word-delete paths.
uint32_t utf8_byte_pos(const char *txt, uint32_t char_pos);

#ifdef ARDUINO
// Base64-encode via mbedtls (ESP32 only — mbedtls is not linked into the
// desktop emulator, and every caller is already under #ifdef ARDUINO).
// Returns false and clears `out` on error.
bool base64_encode(const uint8_t *data, size_t len, std::string &out);
#endif

} // namespace hal
