/**
 * @file      ssh_hosts.cpp
 * @brief     The SSH host list (see ssh_hosts.h). Pure C++.
 */
#include "ssh_hosts.h"

#include <cstddef>
#include <string>
#include <vector>

namespace hal {
namespace ssh {

namespace {

/** Longest accepted name / user / host, in bytes. */
constexpr size_t kNameMax = 32;
constexpr size_t kUserMax = 64;
constexpr size_t kHostMax = 128;

std::string trim(const std::string &s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

bool fail(std::vector<Host> *out, std::string *err, size_t entry, const char *why) {
    if (out) out->clear();
    if (err) *err = "entry " + std::to_string(entry) + ": " + why;
    return false;
}

bool control_char(char c) {
    const unsigned char b = static_cast<unsigned char>(c);
    return b < 0x20 || b == 0x7F;
}

bool bad_host_char(char c) {
    return c == ' ' || c == '\t' || c == '/' || c == '@' || c == ':' || c == ';' ||
           control_char(c);
}

}  // namespace

bool parse_hosts(const std::string &text, std::vector<Host> *out, std::string *err) {
    if (out) out->clear();
    std::vector<Host> hosts;

    size_t pos = 0;
    size_t entry_no = 0;
    while (pos < text.size()) {
        const size_t semi = text.find(';', pos);
        const size_t end = semi == std::string::npos ? text.size() : semi;
        const std::string entry = trim(text.substr(pos, end - pos));
        pos = semi == std::string::npos ? text.size() : semi + 1;
        ++entry_no;

        if (entry.empty()) continue;

        Host h;
        std::string rest = entry;
        bool have_name = false;
        const size_t eq = entry.find('=');
        if (eq != std::string::npos) {
            h.name = trim(entry.substr(0, eq));
            rest = entry.substr(eq + 1);
            if (h.name.empty()) return fail(out, err, entry_no, "empty name");
            if (h.name.size() > kNameMax) return fail(out, err, entry_no, "name too long");
            if (h.name.find('@') != std::string::npos) {
                return fail(out, err, entry_no, "bad name");
            }
            for (char c : h.name) {
                if (control_char(c)) return fail(out, err, entry_no, "bad name");
            }
            have_name = true;
        }

        /* Exactly one '@' splits the user from the host. */
        const size_t at = rest.find('@');
        if (at == std::string::npos || rest.find('@', at + 1) != std::string::npos) {
            return fail(out, err, entry_no, "missing user@");
        }
        h.user = rest.substr(0, at);
        if (h.user.empty() || h.user.size() > kUserMax) {
            return fail(out, err, entry_no, "bad user");
        }
        for (char c : h.user) {
            if (c == ' ' || c == '\t' || c == ':' || c == ';' || control_char(c)) {
                return fail(out, err, entry_no, "bad user");
            }
        }

        const std::string hostport = rest.substr(at + 1);
        const size_t colon = hostport.rfind(':');
        if (colon != std::string::npos) {
            const std::string port = hostport.substr(colon + 1);
            if (port.empty()) return fail(out, err, entry_no, "bad port");
            unsigned long v = 0;
            for (char c : port) {
                if (c < '0' || c > '9') return fail(out, err, entry_no, "bad port");
                v = v * 10 + static_cast<unsigned long>(c - '0');
                if (v > 65535) return fail(out, err, entry_no, "bad port");
            }
            if (v < 1) return fail(out, err, entry_no, "bad port");
            h.port = static_cast<uint16_t>(v);
            h.host = hostport.substr(0, colon);
        } else {
            h.host = hostport;
        }
        if (h.host.empty() || h.host.size() > kHostMax) {
            return fail(out, err, entry_no, "bad host");
        }
        for (char c : h.host) {
            if (bad_host_char(c)) return fail(out, err, entry_no, "bad host");
        }

        if (!have_name) h.name = h.host;

        if (hosts.size() >= kMaxHosts) return fail(out, err, entry_no, "more than 16 hosts");
        for (const Host &o : hosts) {
            if (o.name == h.name) return fail(out, err, entry_no, "duplicate name");
        }
        hosts.push_back(h);
    }

    if (out) *out = hosts;
    return true;
}

std::string format_host(const Host &h) {
    std::string s;
    if (h.name != h.host) s = h.name + "=";
    s += h.user;
    s += "@";
    s += h.host;
    if (h.port != 22) s += ":" + std::to_string(h.port);
    return s;
}

std::string host_label(const Host &h) {
    std::string s = h.name + "  " + h.user + "@" + h.host;
    if (h.port != 22) s += ":" + std::to_string(h.port);
    return s;
}

}  // namespace ssh
}  // namespace hal
