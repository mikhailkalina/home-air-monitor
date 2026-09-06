// Vendor-supplied register and pin definitions, reproduced verbatim.
// Fixture for scripts/tests/test_pin_literal_check.sh: everything below
// would be a pin-literal violation anywhere else in the tree, but files
// under a vendor/ directory keep their own header (no SPDX line -- see
// scripts/check_sources.sh's EXCLUDE array) and are exempt from every check
// in that script, including this one.

#define CHIP_RESET_GPIO 9

static inline void vendor_reset(void)
{
    gpio_set_direction(9, 1);
}
