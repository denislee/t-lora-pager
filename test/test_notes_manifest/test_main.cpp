/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/notes_manifest.{h,cpp}.
 *
 * Covers the pure notes.sync manifest layer: the params serialization built by
 * notes_sync_params() and the greedy split performed by notes_manifest_batches().
 * A REQ payload is capped at 4096 B (PROTOCOL.md §3), so the batcher must keep
 * every serialized params object within NOTES_SYNC_MAX_PARAMS while covering
 * each file exactly once, in order. The long-name case pushes a 200-file
 * manifest past a single batch so the split is exercised for real, and a
 * smaller max_bytes proves the limit is honoured rather than ignored.
 */
#include <unity.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hal/notes_manifest.h"
#include "hal/link/link_json.h"

void setUp(void) {}
void tearDown(void) {}

/* --- helpers ------------------------------------------------------------- */

/* "note-" + 3-digit zero-padded index + "-" + 80 'x' + ".txt". */
static std::string long_name(size_t i) {
    std::string idx = std::to_string(i);
    while (idx.size() < 3) idx.insert(idx.begin(), '0');
    return "note-" + idx + "-" + std::string(80, 'x') + ".txt";
}

/* 200 deterministic entries whose serialized form is far too large for one
 * batch of NOTES_SYNC_MAX_PARAMS. */
static std::vector<NoteEntry> many_files(void) {
    std::vector<NoteEntry> files;
    files.reserve(200);
    for (size_t i = 0; i < 200; ++i) {
        NoteEntry e;
        e.name = long_name(i);
        e.size = static_cast<uint32_t>(1000 + i);
        e.mtime = static_cast<uint32_t>(1700000000 + i);
        e.crc32 = static_cast<uint32_t>(0xDEADBEEFu ^ static_cast<uint32_t>(i));
        e.vol = (i % 2) ? "sd" : "int";
        files.push_back(e);
    }
    return files;
}

/* The params for a batch must fit the running limit, and batches must tile
 * [0, files.size()) in order without gaps or overlap. */
static void assert_contiguous_cover(const std::vector<std::string> &targets,
                                    const std::vector<NoteEntry> &files,
                                    const std::vector<std::pair<size_t, size_t>> &batches,
                                    size_t max_bytes) {
    size_t covered = 0;
    for (size_t i = 0; i < batches.size(); ++i) {
        TEST_ASSERT_TRUE(batches[i].first < batches[i].second);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(
            max_bytes,
            notes_sync_params(targets, files, batches[i].first, batches[i].second).size());
        if (i == 0) {
            TEST_ASSERT_EQUAL_size_t(0, batches[i].first);
        } else {
            TEST_ASSERT_EQUAL_size_t(batches[i - 1].second, batches[i].first);
        }
        covered += batches[i].second - batches[i].first;
    }
    TEST_ASSERT_EQUAL_size_t(files.size(), batches.back().second);
    TEST_ASSERT_EQUAL_size_t(files.size(), covered);
}

/* --- tests --------------------------------------------------------------- */

static void test_empty_list_no_batches(void) {
    const std::vector<std::string> targets = {"github"};
    const std::vector<NoteEntry> files;

    const std::vector<std::pair<size_t, size_t>> batches =
        notes_manifest_batches(targets, files);
    TEST_ASSERT_EQUAL_size_t(0, batches.size());
}

static void test_one_file_one_batch(void) {
    const std::vector<std::string> targets = {"github"};
    NoteEntry e;
    e.name = "a.txt";
    e.size = 10;
    e.mtime = 1;
    e.crc32 = 2;
    e.vol = "int";
    const std::vector<NoteEntry> files = {e};

    const std::vector<std::pair<size_t, size_t>> batches =
        notes_manifest_batches(targets, files);
    TEST_ASSERT_EQUAL_size_t(1, batches.size());
    TEST_ASSERT_EQUAL_size_t(0, batches[0].first);
    TEST_ASSERT_EQUAL_size_t(1, batches[0].second);

    const std::string s = notes_sync_params(targets, files, batches[0].first, batches[0].second);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, s.find("\"targets\":[\"github\"]"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, s.find("\"vol\":\"int\""));

    /* The serialized batch is a valid JSON object with both members. */
    pagerlink::JsonObject obj;
    TEST_ASSERT_TRUE(obj.parse(s));
    TEST_ASSERT_TRUE(obj.has("targets"));
    TEST_ASSERT_TRUE(obj.has("files"));
}

static void test_many_long_names_split(void) {
    const std::vector<std::string> targets = {"github"};
    const std::vector<NoteEntry> files = many_files();
    TEST_ASSERT_EQUAL_size_t(200, files.size());

    const std::vector<std::pair<size_t, size_t>> batches =
        notes_manifest_batches(targets, files);

    for (const std::pair<size_t, size_t> &b : batches) {
        TEST_ASSERT_TRUE(b.first < b.second);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(
            NOTES_SYNC_MAX_PARAMS,
            notes_sync_params(targets, files, b.first, b.second).size());
    }

    /* 200 long names cannot fit in one batch. */
    TEST_ASSERT_TRUE(batches.size() > 1);

    /* Contiguous, in-order cover of all 200 files — each appears exactly once. */
    assert_contiguous_cover(targets, files, batches, NOTES_SYNC_MAX_PARAMS);
}

static void test_custom_max_respected(void) {
    const std::vector<std::string> targets = {"github"};
    const std::vector<NoteEntry> files = many_files();

    const std::vector<std::pair<size_t, size_t>> batches =
        notes_manifest_batches(targets, files, 1000);

    TEST_ASSERT_TRUE(batches.size() > 0);
    for (const std::pair<size_t, size_t> &b : batches) {
        TEST_ASSERT_TRUE(b.first < b.second);
        TEST_ASSERT_LESS_OR_EQUAL_size_t(
            1000, notes_sync_params(targets, files, b.first, b.second).size());
    }

    assert_contiguous_cover(targets, files, batches, 1000);
}

int main(int /*argc*/, char ** /*argv*/) {
    UNITY_BEGIN();
    RUN_TEST(test_empty_list_no_batches);
    RUN_TEST(test_one_file_one_batch);
    RUN_TEST(test_many_long_names_split);
    RUN_TEST(test_custom_max_respected);
    return UNITY_END();
}
