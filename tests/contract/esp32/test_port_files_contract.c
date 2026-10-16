// SPDX-License-Identifier: Apache-2.0
//
// On-target runner for port_files_contract.c: the same body the host runner
// (tests/contract/test_port_files_contract.c) runs against a directory, here
// against adp_sd on the board's microSD card. Built into the test app with
//
//   cd apps/firmware_esp32/test_app && idf.py -T hac_platform build
//
// It needs a FAT-formatted card in the slot; without one the case is
// IGNORED, not failed, and says so. It writes under pfct/ on the card and
// removes its files again (the directories stay). The runner reads back with
// stdio through ESP-IDF's VFS, not through the port, and takes the medium
// away by unmounting the card and gives it back by mounting it again; it
// cannot fill a card, so the "medium full" checks are reported as not
// exercised here -- the host's run holds the port to them
// (docs/adr/0024-sd-card-recordings-and-debug-log.md).

#include <stdbool.h>
#include <stdio.h>

#include "unity.h"

#include "adp_log.h"
#include "adp_sd.h"
#include "contract_print.h"
#include "port_files_contract.h"

#define FULL_PATH_MAX (sizeof(ADP_SD_MOUNT_POINT) + PORT_FILES_PATH_MAX + 1u)

static void report(void *ctx, const char *check, bool ok)
{
    contract_print_check((const contract_print_ctx_t *)ctx, check, ok);
}

static hal_status_t read_back(void *ctx, const char *path, uint8_t *buf, size_t cap, size_t *len)
{
    (void)ctx;
    char full[FULL_PATH_MAX];
    snprintf(full, sizeof(full), "%s/%s", ADP_SD_MOUNT_POINT, path);
    FILE *f = fopen(full, "rb");
    if (f == NULL) {
        return HAL_ERR_NOT_FOUND;
    }
    *len = fread(buf, 1u, cap, f);
    fclose(f);
    return HAL_OK;
}

static void set_present(void *ctx, bool present)
{
    adp_sd_t *sd = (adp_sd_t *)ctx;
    if (present) {
        TEST_ASSERT_EQUAL_INT(HAL_OK, adp_sd_mount(sd, false));
    } else {
        adp_sd_unmount(sd, ADP_SD_EJECTED);
    }
}

TEST_CASE("adp_sd satisfies the port_files contract", "[contract][port_files]")
{
    static adp_log_t log;
    static adp_sd_t sd;
    static bool bus_up;
    adp_log_init(&log);
    if (!bus_up) {
        TEST_ASSERT_EQUAL_INT(HAL_OK, adp_sd_init(&sd, adp_log_port(&log)));
        bus_up = true;
    }
    const hal_status_t mounted = adp_sd_mount(&sd, false);
    if (mounted != HAL_OK) {
        printf("contract port_files adp_sd: no card mounted (status %d, esp_err 0x%x, state %s)\n",
               (int)mounted, (unsigned)sd.last_mount_err, adp_sd_state_name(sd.state));
        TEST_IGNORE_MESSAGE("no FAT card in the slot: the port_files contract was not run");
    }
    adp_sd_card_info_t info;
    if (adp_sd_card_info(&sd, true, &info) == HAL_OK) {
        printf("contract port_files adp_sd: card %s, %llu MB, %llu MB free, %lu kHz\n", info.name,
               (unsigned long long)(info.capacity_bytes / (1024u * 1024u)),
               (unsigned long long)(info.free_bytes / (1024u * 1024u)),
               (unsigned long)info.freq_khz);
    }

    const port_files_contract_env_t env = {
        .read_back = read_back,
        .set_present = set_present,
        .set_full = NULL,
        .ctx = &sd,
    };
    contract_print_ctx_t ctx = {.port = "port_files", .implementation = "adp_sd"};
    const unsigned failures = port_files_contract_run(adp_sd_port(&sd), &env, report, &ctx);
    printf("contract port_files adp_sd: slowest write %lu us, slowest flush %lu us\n",
           (unsigned long)sd.slowest_write_us, (unsigned long)sd.slowest_flush_us);
    adp_sd_unmount(&sd, ADP_SD_EJECTED);
    TEST_ASSERT_EQUAL_UINT(0u, failures);
}
