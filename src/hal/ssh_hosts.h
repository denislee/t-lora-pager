/**
 * @file      ssh_hosts.h
 * @brief     The SSH host list (settings id ssh.hosts).
 *
 * Pure C++ (C++11 on the device, gnu++17 in native_test): parses, formats and
 * labels the saved hosts of the SSH app. The list lives in one NVS string
 * (hal/nvs, namespace "ssh", key "hosts"); the phone and the pager's Settings
 * page share this syntax, and every entry uses the same ssh.user / ssh.key /
 * ssh.password credentials. Pinned by test/test_ssh_hosts.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hal {
namespace ssh {

/** Hosts the list keeps. */
constexpr size_t kMaxHosts = 16;

/** One saved host. `name` is what the picker shows (defaults to `host`). */
struct Host {
    std::string name;
    std::string user;
    std::string host;
    uint16_t port = 22;
};

/**
 * Parse the ssh.hosts text: entries separated by ';', each
 * 'name=user@host[:port]' (the 'name=' part optional; name defaults to host),
 * e.g. 'lab=pi@10.0.0.2:2222; me@example.com'. Entries are trimmed of
 * spaces/tabs; empty entries are skipped but still counted for numbering.
 * Returns false with *err set to 'entry N: <reason>' (N 1-based, counting every
 * ';'-separated piece) on the first bad entry; *out is cleared first and only
 * filled on success (out/err may be null).
 */
bool parse_hosts(const std::string &text, std::vector<Host> *out, std::string *err);

/** Canonical entry: 'user@host' plus ':port' when port != 22, prefixed by
 * 'name=' when name != host. */
std::string format_host(const Host &h);

/** Picker label: 'name  user@host' plus ':port' when port != 22 (two spaces
 * between). */
std::string host_label(const Host &h);

}  // namespace ssh
}  // namespace hal
