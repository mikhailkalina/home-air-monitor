# Sensirion embedded-i2c-scd4x, vendored

Sensirion's own C driver for the SCD4x CO2 sensors, copied here unmodified.
BSD-3-Clause; the licence text is `LICENSE` beside this file, and
`THIRD_PARTY_NOTICES.md` at the repository root records the dependency.

- Source: <https://github.com/Sensirion/embedded-i2c-scd4x>
- Tag: `1.2.0` (2025-07-01)
- Commit: `d953712213957544b5429542381fd7629cbb3786`
- Fetched: 2026-10-14

## What is here

Every file is byte-for-byte as it is at that commit. SHA-256:

| File | SHA-256 |
|---|---|
| `LICENSE` | `df8300acbd0a72ef37303d63d4f252a3df718e56361691eb6d25997a19d86d88` |
| `scd4x_i2c.c` | `18cabcc9365c711c7aba74446bb514efcbac084feb0d7ae90dd9e6bae7b62b13` |
| `scd4x_i2c.h` | `840ccb63910c99b6c1e2d473a58040919c67814d295b6dee6461a5b6d8f0de03` |
| `sensirion_common.c` | `a73ce005b82b47142b50e2076376c1c6dd57315c1d518aafb5c1322f7f02ad78` |
| `sensirion_common.h` | `1eacae5f8381881562d30fd3ef11edd4d5a666b93baf6b3f07f5b6025a0946f2` |
| `sensirion_config.h` | `76a6211089ac94f9b5f54f401f8e329fae2a25c5d8e5eb079c4abad75cfc4dbd` |
| `sensirion_i2c.c` | `e1edf28ca40f542106b4c1fcc96bf29cdd17e134945650225e57ab4423a748cb` |
| `sensirion_i2c.h` | `75eb5af1bea249d238df54e630a09d5694829f793915189f1daf06bc4035056f` |
| `sensirion_i2c_hal.h` | `a086c8c9d6e59baf2584d827476084fa8048d79efd997807b2471aa95f0df65d` |

## What is not

- `sensirion_i2c_hal.c`: a template whose every function is a TODO. This
  project's implementation of `sensirion_i2c_hal.h` is `../scd4x_hal.c`,
  over `port_i2c` and `port_clock`.
- `example-usage/`, `sample-implementations/`, `tests/`, `images/`, the
  changelog and the CI files.

## Rules

- **Do not edit these files.** A change belongs in this project's code
  (`../scd4x_hal.c`, `../scd4x_source.c`), or upstream. The files keep their
  own licence headers: `scripts/check_sources.sh` and the clang-format job
  skip every `vendor/` directory.
- They build with the relaxed options in `cmake/sources_drivers.cmake` and
  are included as SYSTEM headers, so their style cannot fail this project's
  `-Werror` build.
- **To update:** copy the same files from a newer tag, update the table above
  and the version in `THIRD_PARTY_NOTICES.md` in the same commit, and run the
  host tests: the SCD41 model (`platform/host/i2c_models/scd41_model.c`)
  holds the datasheet's timings independently, so a changed wait or command
  shows there.
