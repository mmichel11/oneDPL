# [DO NOT MERGE] Debugging the KT sort tests

Tools for debugging six KT sort tests:

- `kt_sycl_radix_sort_by_key_out_of_place_dpwi7_wgs1024_int_double`
- `kt_sycl_radix_sort_by_key_dpwi8_wgs1024_char_double`
- `kt_sycl_radix_sort_by_key_out_of_place_dpwi12_wgs512_sycl_half_double`
- `kt_sycl_radix_sort_by_key_dpwi13_wgs512_uint16_double`
- `kt_sycl_radix_sort_dpwi12_wgs1024_int32`
- `kt_sycl_radix_sort_residency_dpwi32_wgs1024_uint32_uint32`

The tests run main's input sizes unchanged.

## Build

```bash
cmake -S <oneDPL> -B build_kt_debug -DCMAKE_CXX_COMPILER=icpx -DCMAKE_BUILD_TYPE=Release \
      -DONEDPL_BACKEND=dpcpp_only -DONEDPL_TEST_ENABLE_KT_SYCL=ON -DONEDPL_TEST_KT_DEBUG_ONLY=ON
make -C build_kt_debug -j6 build-kt_sycl-sort-tests
```

`ONEDPL_TEST_KT_DEBUG_ONLY=ON` generates only the six tests, labels them `kt_debug`, and compiles them with
`_ONEDPL_KT_TRACE=1`. Other builds are unaffected: without the macro the trace functions are empty.

## Run

```bash
# Every test with the host trace and an IGC shader dump, logs in ./kt_debug_logs_<timestamp>/
TIMEOUT=28800 test/kt/debug/run_debug_tests.sh build_kt_debug

# Only some tests
FILTER=residency TIMEOUT=600 test/kt/debug/run_debug_tests.sh build_kt_debug

# Plain ctest, host trace only
cd build_kt_debug && ctest -L kt_debug -V --timeout 28800
```

Outputs, per test (`<logs>/<test>/`):

| File | Content |
|---|---|
| `test.log` | full ctest output |
| `host_trace.log` | only the `kt-host-trace` lines |
| `device_debug.log` | only the `kt-device-debug` lines (device debug builds) |
| `igc_dump/` | IGC shader dump (`.zeinfo`, `.asm`, ...) |
| `shader_info.txt`, `shader_info.csv` | per kernel: `grf_count`, `eu_thread_count`, `numAcc`, SIMD, barriers, SLM, scratch, spills |

Plus `run_info.txt` (compiler, git revision, environment) and `summary.txt` (result, duration, onesweep
`grf_count/eu_thread_count`, the number of `STALL` lines, and the last host trace line of every test that did not pass).

`run_debug_tests.sh` disables the NEO and SYCL compiler caches (`NEO_CACHE_PERSISTENT=0`,
`SYCL_CACHE_PERSISTENT=0`); a cached binary is not recompiled, so IGC would not dump it.

## Host trace (`kt-host-trace`)

Host side only: nothing runs in the kernels. Lines are timestamped in seconds since the first trace line:

- `device:`: name, driver version, `max_compute_units`, `local_mem_size`, Intel topology (slices, subslices, EUs,
  threads per EU, SIMD width) and the relevant environment variables. Printed once.
- `onesweep sort:`: shape of each sort: n, segments, tiles, stages, key and value sizes, work-group size, data
  per work-item, in-place.
- `cooperative groups:`: every term of `__get_num_cooperative_groups`: tiles, SLM requested and reserved,
  `local_mem_size`, XEs, groups per XE (lanes, SLM adjusted), our estimate, the L0 query, and the launched count.
- `onesweep kernel:`: properties of the compiled kernel (max work-group size, private memory, sub-group size).
- `<kernel> stage=N: submitted` / `done in X s` for memset, histogram, scan, onesweep, copyback.
- `test case START` / `DONE` for each input size.

Environment variables:

| Variable | Effect |
|---|---|
| `ONEDPL_KT_TRACE=0` | silence the trace |
| `ONEDPL_KT_TRACE_SYNC=0` | do not wait for each kernel. By default each kernel is waited on and timed, so a kernel which does not complete shows as a `submitted` line without a matching `done` line |
| `ONEDPL_KT_DEBUG_MAX_GROUPS=N` | cap the onesweep work-group count, e.g. to check whether a failure depends on it |

## Device debug (`kt-device-debug`)

Optional instrumentation of the cooperative onesweep kernel, compiled in only with `_ONEDPL_KT_DEVICE_DEBUG`
(CMake: `-DONEDPL_TEST_KT_DEVICE_DEBUG=ON` together with `-DONEDPL_TEST_KT_DEBUG_ONLY=ON`). Without the macro the
kernels are unchanged: they are identical to main (same GRF, instructions and assembly). Use a separate build
directory, so that the plain build still reproduces the original behavior:

```bash
cmake -S <oneDPL> -B build_kt_device_debug ... -DONEDPL_TEST_KT_DEBUG_ONLY=ON -DONEDPL_TEST_KT_DEVICE_DEBUG=ON
make -C build_kt_device_debug -j6 build-kt_sycl-sort-tests
TIMEOUT=600 test/kt/debug/run_debug_tests.sh build_kt_device_debug
```

A lookback which spins on one tile for more than the spin limit gives up: it prints a `STALL` line and raises an
abort flag which makes every other lookback give up as well, so the kernel completes instead of waiting forever, and its
device printf output is flushed. **The sort result of an aborted launch is invalid**, so the test then fails with
wrong results. After an abort, each work-group prints an `ABORTED` line:

```
[kt-device-debug] STALL stage=0 group=0 sub_group=2 bin=64 tile=4 waits_on_tile=3 observed=0x00000000 spins=16777216 started_groups=1 launched_groups=1
[kt-device-debug] ABORTED stage=0 group=0 start_ticket=1 last_completed_tile=6 launched_groups=1 tiles=7
```

| Field | Meaning |
|---|---|
| `tile`, `waits_on_tile` | the tile whose lookback stalled and the earlier tile whose histogram it waits for; with W launched work-groups, tile t is processed by work-group t % W |
| `observed` | value of the waited-for histogram entry; bit 31 set: published, bit 30 set: accumulated |
| `spins` | lookback iterations on that tile |
| `started_groups`, `launched_groups` | work-groups which had started when the stall was detected, out of the launched ones. Fewer started than launched means that the launched work-groups are not all co-resident |
| `start_ticket` | start order of the work-group (1-based); tickets above `started_groups` of the STALL lines started only after the abort |
| `last_completed_tile` | last tile the work-group completed, -1 if none |

At most 16 `STALL` lines are printed per kernel launch.

| Variable | Effect |
|---|---|
| `ONEDPL_KT_DEVICE_DEBUG_SPIN_LIMIT=N` | lookback iterations on one tile before giving up (default 2^24, a few seconds) |

**The instrumentation can change the GRF mode.** A onesweep kernel which already uses 256 GRF may move to 512 GRF,
and 512 GRF has a different number of threads per EU on some drivers, which can hide the problem being debugged.
Check `shader_info.txt` of both builds, and force the GRF size of the plain build if they differ, e.g.:

```bash
SYCL_PROGRAM_APPEND_COMPILE_OPTIONS="-ze-exp-register-file-size 256" TIMEOUT=600 \
    test/kt/debug/run_debug_tests.sh build_kt_device_debug
```

At the forced size IGC spills registers instead, which is slower but keeps the occupancy of the plain build.

## Shader info

`extract_shader_info.py <dump dir> [...] [--csv file]` summarizes any IGC dump. `eu_thread_count` in the `.zeinfo`
is the number of threads per EU the compiler expects for the kernel's GRF mode. Compare it with the work-groups
launched per XE:
`launched / XEs * (work_group_size / 32)` threads per XE must fit in `eu_thread_count * 8`.
