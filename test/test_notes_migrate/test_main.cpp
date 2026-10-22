/**
 * @file      test_main.cpp
 * @brief     Unity tests for hal/notes_migrate.{h,cpp} — the pure planning
 *            helpers behind the bulk passphrase migration in
 *            notes_crypto.cpp (P7.7).
 *
 * These helpers decide which copies of a protected path exist, which of them
 * must be transformed, what an unlock does after an interrupted
 * change-passphrase, and whether two reads are byte-identical. They take no
 * filesystem, NVS or crypto dependency, so the decisions are pinned here as
 * golden cases: a regression could either strand a note in the old key or
 * rewrite one copy under a different key than its twin.
 */
#include <unity.h>

#include "hal/notes_migrate.h"

void setUp(void) {}
void tearDown(void) {}

/* ---- group_by_name ---- */

static void test_group_both_lists_empty(void)
{
    const auto out = nc_migrate::group_by_name({}, {});
    TEST_ASSERT_EQUAL_INT(0, (int)out.size());
}

static void test_group_merges_ffat_then_sd_only(void)
{
    /* FFat order first, then SD-only names in SD order. The shared name keeps
     * both flags. */
    const auto out = nc_migrate::group_by_name(
        {"notes/a.txt", "/journal_idx.bin"},
        {"notes/b.txt", "notes/a.txt"});

    TEST_ASSERT_EQUAL_INT(3, (int)out.size());

    TEST_ASSERT_EQUAL_STRING("notes/a.txt", out[0].path.c_str());
    TEST_ASSERT_TRUE(out[0].ffat);
    TEST_ASSERT_TRUE(out[0].sd);

    TEST_ASSERT_EQUAL_STRING("/journal_idx.bin", out[1].path.c_str());
    TEST_ASSERT_TRUE(out[1].ffat);
    TEST_ASSERT_FALSE(out[1].sd);

    TEST_ASSERT_EQUAL_STRING("notes/b.txt", out[2].path.c_str());
    TEST_ASSERT_FALSE(out[2].ffat);
    TEST_ASSERT_TRUE(out[2].sd);
}

static void test_group_collapses_duplicates_within_list(void)
{
    const auto out = nc_migrate::group_by_name({"x", "x"}, {});

    TEST_ASSERT_EQUAL_INT(1, (int)out.size());
    TEST_ASSERT_EQUAL_STRING("x", out[0].path.c_str());
    TEST_ASSERT_TRUE(out[0].ffat);
    TEST_ASSERT_FALSE(out[0].sd);
}

static void test_group_sd_only_entries(void)
{
    const auto out = nc_migrate::group_by_name({}, {"notes/a.txt", "notes/b.txt"});

    TEST_ASSERT_EQUAL_INT(2, (int)out.size());
    TEST_ASSERT_EQUAL_STRING("notes/a.txt", out[0].path.c_str());
    TEST_ASSERT_FALSE(out[0].ffat);
    TEST_ASSERT_TRUE(out[0].sd);
    TEST_ASSERT_EQUAL_STRING("notes/b.txt", out[1].path.c_str());
    TEST_ASSERT_FALSE(out[1].ffat);
    TEST_ASSERT_TRUE(out[1].sd);
}

static void test_group_names_case_sensitive(void)
{
    /* Names are merged by exact bytes: differing only by case stays two
     * names, because they are two files on both filesystems. */
    const auto out = nc_migrate::group_by_name(
        {"notes/A.txt"},
        {"notes/a.txt"});

    TEST_ASSERT_EQUAL_INT(2, (int)out.size());
    TEST_ASSERT_EQUAL_STRING("notes/A.txt", out[0].path.c_str());
    TEST_ASSERT_TRUE(out[0].ffat);
    TEST_ASSERT_FALSE(out[0].sd);
    TEST_ASSERT_EQUAL_STRING("notes/a.txt", out[1].path.c_str());
    TEST_ASSERT_FALSE(out[1].ffat);
    TEST_ASSERT_TRUE(out[1].sd);
}

/* ---- plan_copies ---- */

static void test_plan_both_copies_ok(void)
{
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::Shared,
                          (int)nc_migrate::plan_copies(true, true, true));
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::Separate,
                          (int)nc_migrate::plan_copies(true, true, false));
}

static void test_plan_ffat_only(void)
{
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::FfatOnly,
                          (int)nc_migrate::plan_copies(true, false, true));
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::FfatOnly,
                          (int)nc_migrate::plan_copies(true, false, false));
}

static void test_plan_sd_only(void)
{
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::SdOnly,
                          (int)nc_migrate::plan_copies(false, true, true));
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::SdOnly,
                          (int)nc_migrate::plan_copies(false, true, false));
}

static void test_plan_no_copies(void)
{
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::None,
                          (int)nc_migrate::plan_copies(false, false, true));
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::CopyPlan::None,
                          (int)nc_migrate::plan_copies(false, false, false));
}

/* ---- pending_recovery ---- */

static void test_recovery_nothing_pending(void)
{
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::Recovery::None,
                          (int)nc_migrate::pending_recovery(false, false));
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::Recovery::None,
                          (int)nc_migrate::pending_recovery(false, true));
}

static void test_recovery_pending(void)
{
    /* The pending verifier decrypts under the accepted canary: the commit
     * happened, so finish installing the staged copies. */
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::Recovery::RollForward,
                          (int)nc_migrate::pending_recovery(true, true));
    /* It does not: the canary is still the old one, so discard the staged
     * copies. */
    TEST_ASSERT_EQUAL_INT((int)nc_migrate::Recovery::RollBack,
                          (int)nc_migrate::pending_recovery(true, false));
}

/* ---- staged_path ---- */

static void test_staged_path_appends_suffix(void)
{
    TEST_ASSERT_EQUAL_STRING("notes/a.txt.ncnew",
                             nc_migrate::staged_path("notes/a.txt").c_str());
    TEST_ASSERT_EQUAL_STRING("/journal_idx.bin.ncnew",
                             nc_migrate::staged_path("/journal_idx.bin").c_str());
}

static void test_staged_path_is_not_the_original(void)
{
    const std::string original = "notes/a.txt";
    const std::string staged = nc_migrate::staged_path(original);

    /* The staged name must be a different file, and it must not look like a
     * note (.txt) to the rest of the code. */
    TEST_ASSERT_TRUE(staged != original);
    TEST_ASSERT_TRUE(staged.size() < 4 ||
                     staged.compare(staged.size() - 4, 4, ".txt") != 0);
}

/* ---- bytes_equal ---- */

static void test_bytes_equal_identical(void)
{
    const uint8_t a[] = {0x00, 0x01, 0xfe, 0xff};
    const uint8_t b[] = {0x00, 0x01, 0xfe, 0xff};

    TEST_ASSERT_TRUE(nc_migrate::bytes_equal(a, sizeof(a), b, sizeof(b)));
}

static void test_bytes_equal_last_byte_differs(void)
{
    const uint8_t a[] = {0x00, 0x01, 0xfe, 0xff};
    const uint8_t b[] = {0x00, 0x01, 0xfe, 0x00};

    TEST_ASSERT_FALSE(nc_migrate::bytes_equal(a, sizeof(a), b, sizeof(b)));
}

static void test_bytes_equal_different_sizes(void)
{
    const uint8_t a[] = {0x00, 0x01, 0xfe};
    const uint8_t b[] = {0x00, 0x01, 0xfe, 0xff};

    TEST_ASSERT_FALSE(nc_migrate::bytes_equal(a, sizeof(a), b, sizeof(b)));
}

static void test_bytes_equal_empty(void)
{
    TEST_ASSERT_TRUE(nc_migrate::bytes_equal(nullptr, 0, nullptr, 0));
}

static void test_bytes_equal_null_with_size(void)
{
    const uint8_t a[] = {0x00, 0x01, 0xfe, 0xff};

    TEST_ASSERT_FALSE(nc_migrate::bytes_equal(nullptr, sizeof(a), a, sizeof(a)));
    TEST_ASSERT_FALSE(nc_migrate::bytes_equal(a, sizeof(a), nullptr, sizeof(a)));
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_group_both_lists_empty);
    RUN_TEST(test_group_merges_ffat_then_sd_only);
    RUN_TEST(test_group_collapses_duplicates_within_list);
    RUN_TEST(test_group_sd_only_entries);
    RUN_TEST(test_group_names_case_sensitive);
    RUN_TEST(test_plan_both_copies_ok);
    RUN_TEST(test_plan_ffat_only);
    RUN_TEST(test_plan_sd_only);
    RUN_TEST(test_plan_no_copies);
    RUN_TEST(test_recovery_nothing_pending);
    RUN_TEST(test_recovery_pending);
    RUN_TEST(test_staged_path_appends_suffix);
    RUN_TEST(test_staged_path_is_not_the_original);
    RUN_TEST(test_bytes_equal_identical);
    RUN_TEST(test_bytes_equal_last_byte_differs);
    RUN_TEST(test_bytes_equal_different_sizes);
    RUN_TEST(test_bytes_equal_empty);
    RUN_TEST(test_bytes_equal_null_with_size);
    return UNITY_END();
}
