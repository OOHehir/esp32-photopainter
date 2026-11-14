#include <stdio.h>

int tests_run = 0;
int tests_failed = 0;

void test_json_util(void);
void test_dns_pack(void);
void test_update_policy(void);
void test_rollback_policy(void);
void test_sha_hex(void);
void test_app_gfx(void);
void test_app_ui(void);
void test_cal_tz(void);
void test_cal_parse(void);
void test_cal_rrule(void);
void test_cycle_sched(void);

int main(void)
{
    test_json_util();
    test_dns_pack();
    test_update_policy();
    test_rollback_policy();
    test_sha_hex();
    test_app_gfx();
    test_app_ui();
    test_cal_tz();
    test_cal_parse();
    test_cal_rrule();
    test_cycle_sched();

    printf("\n%d checks, %d failed\n", tests_run, tests_failed);
    return tests_failed == 0 ? 0 : 1;
}
