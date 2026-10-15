# Bosch BME68x Sensor API, vendored

Bosch Sensortec's own C driver for the BME680 and BME688, copied here
unmodified. BSD-3-Clause; the licence text is `LICENSE` beside this file, and
`THIRD_PARTY_NOTICES.md` at the repository root records the dependency.

- Source: <https://github.com/boschsensortec/BME68x_SensorAPI>
- Tag: `v4.4.8` (the files say 2023-02-07; the tag's commit is 2023-05-17),
  the newest tag when fetched
- Commit: `80ea120a8b8ac987d7d79eb68a9ed796736be845`
- Fetched: 2026-10-15

## What is here

Every file is byte-for-byte as it is at that commit. SHA-256:

| File | SHA-256 |
|---|---|
| `LICENSE` | `56df116398e0b72a12ae353d93002d29175583590283ced2c2510e3d8075546f` |
| `bme68x.c` | `bad525ea57fe57a5d7dda19d287781bcbab50fdbf2dedea737c16b206cbe0157` |
| `bme68x.h` | `09ed6babf92955ced360790958e2e2f03efb85bc4f0a8a864a4151a905eaf0ca` |
| `bme68x_defs.h` | `5f59c0bafbcc416c091a1a1cc40bb7362f0b48ea02ac7fa5197813f491de01aa` |

## What is not

- `examples/` and its `common/` (COINES glue for Bosch's evaluation board),
  `README.md`. Bosch ships no HAL to replace: the API takes its bus and its
  delay as function pointers, and `../bme68x_hal.c` supplies them over
  `port_i2c` and `port_clock`.
- BSEC. It is a separate, proprietary library; see `THIRD_PARTY_NOTICES.md`.

## How it is built

- With `BME68X_USE_FPU`, the default of `bme68x_defs.h`: compensated values
  come out as `float`, which the ESP32-S3 computes in hardware.
  `../bme68x_source.c` refuses to build without it.
- `bme68x.c` includes `<stdio.h>` and calls nothing from it. That include
  would fail `scripts/check_sources.sh` in first-party driver code; vendored
  code is exempt.

## Rules

- **Do not edit these files.** A change belongs in this project's code
  (`../bme68x_hal.c`, `../bme68x_source.c`), or upstream. The files keep their
  own licence headers: `scripts/check_sources.sh` and the clang-format job
  skip every `vendor/` directory.
- They build with the relaxed options in `cmake/sources_drivers.cmake` and
  are included as SYSTEM headers, so their style cannot fail this project's
  `-Werror` build.
- **To update:** copy the same files from a newer tag, update the table above
  and the version in `THIRD_PARTY_NOTICES.md` in the same commit, and run the
  host tests. The BME680 model (`platform/host/i2c_models/bme680_model.c`)
  takes the measurement time from this version's `bme68x_get_meas_dur()`
  (it is in no datasheet), so a newer version that changes it must change the
  model too -- the model's header says where.
