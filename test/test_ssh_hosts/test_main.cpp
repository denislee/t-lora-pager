/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/ssh_hosts.{h,cpp} — the ssh.hosts list: the
 *            parser and its error reasons, the canonical entry and the picker
 *            label.
 *
 * The parser is pure (no Arduino / LVGL), so the defaults, the skipped entries,
 * the limits and every rejection are covered here.
 */
#include <unity.h>

#include <cstdint>
#include <string>
#include <vector>

#include "hal/ssh_hosts.h"

void setUp(void) {}
void tearDown(void) {}

using hal::ssh::Host;

/* Parse and return the error ("" on success); `hosts` keeps the result. */
static std::string parse_err(const std::string &text, std::vector<Host> &hosts) {
    std::string err;
    if (hal::ssh::parse_hosts(text, &hosts, &err)) return "";
    return err;
}

/* ---- parse_hosts ---- */

static void test_parse_named_host_with_port(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("", parse_err("lab=pi@10.0.0.2:2222", hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(hosts.size()));
    TEST_ASSERT_EQUAL_STRING("lab", hosts[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("pi", hosts[0].user.c_str());
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", hosts[0].host.c_str());
    TEST_ASSERT_EQUAL_UINT16(2222, hosts[0].port);
}

static void test_parse_default_name_and_port(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("", parse_err("me@example.com", hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(hosts.size()));
    TEST_ASSERT_EQUAL_STRING("example.com", hosts[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("me", hosts[0].user.c_str());
    TEST_ASSERT_EQUAL_STRING("example.com", hosts[0].host.c_str());
    TEST_ASSERT_EQUAL_UINT16(22, hosts[0].port);
}

static void test_parse_skips_blank_entries_and_spaces(void) {
    std::vector<Host> hosts;
    const std::string text = "; \t lab=pi@10.0.0.2 ;  me@example.com\t; ;";
    TEST_ASSERT_EQUAL_STRING("", parse_err(text, hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(2, static_cast<uint32_t>(hosts.size()));
    TEST_ASSERT_EQUAL_STRING("lab", hosts[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING("pi", hosts[0].user.c_str());
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", hosts[0].host.c_str());
    TEST_ASSERT_EQUAL_STRING("example.com", hosts[1].name.c_str());
}

static void test_newline_inside_entry_is_bad_host(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: bad host", parse_err("u@h\nx", hosts).c_str());
}

static void test_missing_user_at(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 2: missing user@", parse_err("ok@a;bad", hosts).c_str());
    /* A bad entry leaves no hosts at all. */
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(hosts.size()));
}

static void test_double_at_fails(void) {
    std::vector<Host> hosts;
    std::string err;
    TEST_ASSERT_FALSE(hal::ssh::parse_hosts("u@@h", &hosts, &err));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(hosts.size()));
}

static void test_bad_port_zero(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: bad port", parse_err("u@h:0", hosts).c_str());
}

static void test_bad_port_too_large(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: bad port", parse_err("u@h:99999", hosts).c_str());
}

static void test_bad_port_not_a_number(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: bad port", parse_err("u@h:x", hosts).c_str());
}

static void test_bad_host(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: bad host", parse_err("u@a/b", hosts).c_str());
}

static void test_empty_name(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 1: empty name", parse_err("=u@h", hosts).c_str());
}

static void test_too_many_hosts(void) {
    std::string text;
    for (int i = 0; i < 17; ++i) {
        if (i) text += ";";
        text += "u@h" + std::to_string(i);
    }
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 17: more than 16 hosts", parse_err(text, hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(hosts.size()));
}

static void test_duplicate_name(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 3: duplicate name",
                             parse_err("a=p@x;b=p@y;a=p@z", hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(hosts.size()));
}

static void test_duplicate_default_name(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING("entry 2: duplicate name", parse_err("u@h;v@h", hosts).c_str());
}

static void test_empty_text(void) {
    std::vector<Host> hosts;
    std::string err = "stale";
    TEST_ASSERT_TRUE(hal::ssh::parse_hosts("", &hosts, &err));
    TEST_ASSERT_EQUAL_UINT32(0, static_cast<uint32_t>(hosts.size()));
    TEST_ASSERT_EQUAL_STRING("stale", err.c_str());
}

static void test_null_out_and_err(void) {
    TEST_ASSERT_TRUE(hal::ssh::parse_hosts("me@example.com", nullptr, nullptr));
    TEST_ASSERT_FALSE(hal::ssh::parse_hosts("bad", nullptr, nullptr));
}

/* ---- format_host / host_label ---- */

static void test_format_round_trip(void) {
    std::vector<Host> hosts;
    TEST_ASSERT_EQUAL_STRING(
        "",
        parse_err("lab=pi@10.0.0.2:2222;me@example.com;web=me@example.com", hosts).c_str());
    TEST_ASSERT_EQUAL_UINT32(3, static_cast<uint32_t>(hosts.size()));
    /* Port 22 is omitted; the name is omitted when it equals the host. */
    TEST_ASSERT_EQUAL_STRING("lab=pi@10.0.0.2:2222",
                             hal::ssh::format_host(hosts[0]).c_str());
    TEST_ASSERT_EQUAL_STRING("me@example.com", hal::ssh::format_host(hosts[1]).c_str());
    TEST_ASSERT_EQUAL_STRING("web=me@example.com", hal::ssh::format_host(hosts[2]).c_str());

    /* Formatting every parsed host parses back to the same list. */
    std::string text;
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (i) text += "; ";
        text += hal::ssh::format_host(hosts[i]);
    }
    std::vector<Host> again;
    TEST_ASSERT_EQUAL_STRING("", parse_err(text, again).c_str());
    TEST_ASSERT_EQUAL_UINT32(static_cast<uint32_t>(hosts.size()),
                             static_cast<uint32_t>(again.size()));
    for (size_t i = 0; i < hosts.size(); ++i) {
        TEST_ASSERT_EQUAL_STRING(hosts[i].name.c_str(), again[i].name.c_str());
        TEST_ASSERT_EQUAL_STRING(hosts[i].user.c_str(), again[i].user.c_str());
        TEST_ASSERT_EQUAL_STRING(hosts[i].host.c_str(), again[i].host.c_str());
        TEST_ASSERT_EQUAL_UINT16(hosts[i].port, again[i].port);
    }
}

static void test_host_label(void) {
    Host h;
    h.name = "lab";
    h.user = "pi";
    h.host = "10.0.0.2";
    h.port = 2222;
    TEST_ASSERT_EQUAL_STRING("lab  pi@10.0.0.2:2222", hal::ssh::host_label(h).c_str());

    h.name = "example.com";
    h.user = "me";
    h.host = "example.com";
    h.port = 22;
    TEST_ASSERT_EQUAL_STRING("example.com  me@example.com", hal::ssh::host_label(h).c_str());
}

int main(int argc, char **argv) {
    UNITY_BEGIN();

    RUN_TEST(test_parse_named_host_with_port);
    RUN_TEST(test_parse_default_name_and_port);
    RUN_TEST(test_parse_skips_blank_entries_and_spaces);
    RUN_TEST(test_newline_inside_entry_is_bad_host);
    RUN_TEST(test_missing_user_at);
    RUN_TEST(test_double_at_fails);
    RUN_TEST(test_bad_port_zero);
    RUN_TEST(test_bad_port_too_large);
    RUN_TEST(test_bad_port_not_a_number);
    RUN_TEST(test_bad_host);
    RUN_TEST(test_empty_name);
    RUN_TEST(test_too_many_hosts);
    RUN_TEST(test_duplicate_name);
    RUN_TEST(test_duplicate_default_name);
    RUN_TEST(test_empty_text);
    RUN_TEST(test_null_out_and_err);
    RUN_TEST(test_format_round_trip);
    RUN_TEST(test_host_label);

    return UNITY_END();
}
