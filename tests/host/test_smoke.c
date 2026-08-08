#include "test_util.h"

static void test_harness_reports_pass(void)
{
    CHECK_EQ(1 + 1, 2);
}

int main(void)
{
    RUN(test_harness_reports_pass);
    TEST_MAIN_END();
}
