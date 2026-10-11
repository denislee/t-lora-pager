/**
 * @file      test_main.cpp
 * @brief     Unit tests for hal/result.{h,cpp}.
 *
 * Confirms that every HalError enumerator yields a non-empty, non-"Unknown
 * error." message from hal_error_string(). This is what guards against the
 * silent "added a new error variant and forgot to update the switch" class
 * of bug — the default case in result.cpp returns "Unknown error." for any
 * value the switch doesn't cover, so a missing case is detectable here.
 */
#include <unity.h>
#include <cstring>

#include "hal/result.h"

void setUp(void) {}
void tearDown(void) {}

static const HalError ALL_ERRORS[] = {
    HalError::Ok,
    HalError::InvalidArgument,
    HalError::NotSupported,
    HalError::InternalError,
    HalError::StorageFull,
    HalError::WifiOffline,
    HalError::HttpError,
    HalError::Unauthorized,
    HalError::HubDisabled,
    HalError::HubUnreachable,
};

static void test_every_error_has_message(void)
{
    for (HalError e : ALL_ERRORS) {
        const char *msg = hal_error_string(e);
        TEST_ASSERT_NOT_NULL(msg);
        TEST_ASSERT_TRUE(strlen(msg) > 0);
        TEST_ASSERT_TRUE(strcmp(msg, "Unknown error.") != 0);
    }
}

static void test_ok_message(void)
{
    TEST_ASSERT_EQUAL_STRING("OK", hal_error_string(HalError::Ok));
}

static void test_unknown_fallback(void)
{
    /* Cast a value outside the enum to confirm the default branch. */
    HalError bogus = static_cast<HalError>(0xFE);
    TEST_ASSERT_EQUAL_STRING("Unknown error.", hal_error_string(bogus));
}

static void test_result_value_path(void)
{
    HalResult<int> ok(42);
    TEST_ASSERT_TRUE(ok.is_ok());
    TEST_ASSERT_FALSE(ok.is_err());
    TEST_ASSERT_TRUE(static_cast<bool>(ok));
    TEST_ASSERT_EQUAL(42, ok.value());
    TEST_ASSERT_EQUAL(0, (int)ok.error()); /* Ok == 0 */
}

static void test_result_error_path(void)
{
    HalResult<int> err(HalError::StorageFull);
    TEST_ASSERT_FALSE(err.is_ok());
    TEST_ASSERT_TRUE(err.is_err());
    TEST_ASSERT_FALSE(static_cast<bool>(err));
    TEST_ASSERT_EQUAL((int)HalError::StorageFull, (int)err.error());
}

int main(int /*argc*/, char ** /*argv*/)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_error_has_message);
    RUN_TEST(test_ok_message);
    RUN_TEST(test_unknown_fallback);
    RUN_TEST(test_result_value_path);
    RUN_TEST(test_result_error_path);
    return UNITY_END();
}
