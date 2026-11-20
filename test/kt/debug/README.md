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
| `igc_dump/` | IGC shader dump (`.zeinfo`, `.asm`, ...) |
| `shader_info.txt`, `shader_info.csv` | per kernel: `grf_count`, `eu_thread_count`, `numAcc`, SIMD, barriers, SLM, scratch, spills |

Plus `run_info.txt` (compiler, git revision, environment) and `summary.txt` (result, duration, onesweep
`grf_count/eu_thread_count`, and the last host trace line of every test that did not pass).

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

## Shader info

`extract_shader_info.py <dump dir> [...] [--csv file]` summarizes any IGC dump. `eu_thread_count` in the `.zeinfo`
is the number of threads per EU the compiler expects for the kernel's GRF mode. Compare it with the work-groups
launched per XE:
`launched / XEs * (work_group_size / 32)` threads per XE must fit in `eu_thread_count * 8`.
