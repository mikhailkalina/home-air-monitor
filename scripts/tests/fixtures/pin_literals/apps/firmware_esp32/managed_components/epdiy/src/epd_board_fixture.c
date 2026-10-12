/*
 * Deliberately carries NO SPDX header and several pin literals: this fixture
 * stands in for a dependency fetched by ESP-IDF's component manager into
 * managed_components/, which lands under apps/ and so inside
 * hac_find_source_files()'s scan.
 *
 * Every construct below would be a violation in first-party code. The test
 * asserts none of them is reported, which is what proves the
 * managed_components/ exclusion in check_sources.sh works -- and, because
 * the test also asserts the total violation count is unchanged, that the
 * exclusion is narrow enough not to swallow the real violations in
 * platform/other_board/adp_violations.c.
 */

#define SOME_VENDOR_GPIO 7

void fixture_touch_pins(void)
{
    gpio_set_direction(9, 1);
    gpio_set_level(GPIO_NUM_9, 0);
}

static const struct {
    int device_address;
} cfg = {.device_address = 0x5A};
