#!/bin/bash
# [DO NOT MERGE] Runs the KT sort tests selected for debugging (ctest label "kt_debug") one at a time, each with:
#   - the oneDPL host trace (kt-host-trace lines on stderr, see include/.../kt/internal/kt_debug_trace.h)
#   - an IGC shader dump, summarized by extract_shader_info.py (grf_count, eu_thread_count, numAcc, spills, ...)
#   - the kt-device-debug lines, if the build has the device debug (ONEDPL_TEST_KT_DEVICE_DEBUG=ON)
#
# Usage: [TIMEOUT=28800] [FILTER=<test name regex>] [NAME=<run name>] [NOTES=<text>] \
#        ./run_debug_tests.sh <build dir> [output dir]
#   NAME:  appended to the default output directory (kt_debug_logs_<timestamp>_<NAME>) and recorded in run_info.txt
#   NOTES: free text recorded in run_info.txt and summary.txt, e.g. what was changed for this run
# The build directory must be configured with -DONEDPL_TEST_KT_DEBUG_ONLY=ON and built with
# 'make build-kt_sycl-sort-tests'. All ONEDPL_KT_* variables in the environment are passed through to the tests.
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="$(cd "${1:?usage: $0 <build dir> [output dir]}" && pwd)"
TIMEOUT="${TIMEOUT:-28800}"
FILTER="${FILTER:-.}"
NAME="${NAME:-}"
NOTES="${NOTES:-}"
# The run name is used in a directory name: keep letters, digits, '.', '_' and '-'
SAFE_NAME="$(printf '%s' "$NAME" | tr -c 'A-Za-z0-9._-' '_')"
OUT="${2:-$PWD/kt_debug_logs_$(date +%Y%m%d_%H%M%S)${SAFE_NAME:+_$SAFE_NAME}}"
mkdir -p "$OUT"
COMPILER="$(grep -m1 'CMAKE_CXX_COMPILER:' "$BUILD/CMakeCache.txt" | cut -d= -f2)"
DEVICE_DEBUG="$(grep -m1 '^ONEDPL_TEST_KT_DEVICE_DEBUG:' "$BUILD/CMakeCache.txt" | cut -d= -f2)"

{
    echo "name: ${NAME:-<none>}"
    echo "notes: ${NOTES:-<none>}"
    echo "date: $(date)"
    echo "host: $(hostname)"
    echo "oneDPL: $(git -C "$HERE" rev-parse HEAD 2>/dev/null) ($(git -C "$HERE" rev-parse --abbrev-ref HEAD 2>/dev/null))"
    echo "build: $BUILD (device debug: ${DEVICE_DEBUG:-OFF})"
    echo "compiler: $COMPILER"
    "$COMPILER" --version 2>&1 | head -1
    echo "timeout: ${TIMEOUT}s, filter: $FILTER"
    echo "--- environment"
    env | grep -E '^(LD_LIBRARY_PATH|PATH|ONEDPL_|ZE_|ZEX_|NEO|SYCL_|UR_|ONEAPI_|IGC_|OCL_)' | sort
} > "$OUT/run_info.txt"

mapfile -t TESTS < <(cd "$BUILD" && ctest -N -L kt_debug | sed -n 's/^ *Test *#[0-9]*: //p' | grep -E "$FILTER")
echo "${#TESTS[@]} tests, logs in $OUT"
{
    echo "name: ${NAME:-<none>}"
    echo "notes: ${NOTES:-<none>}"
    echo "build: $BUILD (device debug: ${DEVICE_DEBUG:-OFF})"
    printf '%-9s %9s  %-80s %-30s %s\n' result seconds test onesweep_grf/eu_thread_count device_debug_stalls
} > "$OUT/summary.txt"

for name in "${TESTS[@]}"; do
    dir="$OUT/${name%.pass}"
    mkdir -p "$dir/igc_dump"
    echo "=== $name"
    start=$(date +%s)
    # The compiler caches are disabled: a cached binary is not recompiled, so IGC would not dump it
    (cd "$BUILD" && NEO_CACHE_PERSISTENT=0 SYCL_CACHE_PERSISTENT=0 \
        IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir="$dir/igc_dump" \
        ctest -R "^${name//./\\.}\$" -V --timeout "$TIMEOUT" --output-on-failure) > "$dir/test.log" 2>&1
    elapsed=$(( $(date +%s) - start ))
    if grep -q "Passed" "$dir/test.log"; then result=PASS
    elif grep -q "Timeout" "$dir/test.log"; then result=TIMEOUT
    elif grep -q "Not Run\|Skipped" "$dir/test.log"; then result=SKIP
    else result=FAIL; fi

    python3 "$HERE/extract_shader_info.py" "$dir/igc_dump" --csv "$dir/shader_info.csv" > "$dir/shader_info.txt" 2>&1
    onesweep=$(python3 -c 'import csv, sys
rows = csv.DictReader(open(sys.argv[1]))
print(" ".join(sorted({r["grf_count"] + "/" + r["eu_thread_count"] for r in rows if "onesweep_kernel" in r["kernel"]})))' \
        "$dir/shader_info.csv" 2>/dev/null)
    grep "kt-host-trace" "$dir/test.log" > "$dir/host_trace.log"
    grep "kt-device-debug" "$dir/test.log" > "$dir/device_debug.log"
    stalls=$(grep -c "kt-device-debug\] STALL" "$dir/device_debug.log")
    last=$(tail -1 "$dir/host_trace.log" | sed 's/^[0-9]*: //')
    printf '%-9s %9s  %-80s %-30s %s\n' "$result" "$elapsed" "$name" "${onesweep:-?}" "$stalls" \
        | tee -a "$OUT/summary.txt"
    [ "$result" != PASS ] && echo "    last host trace line: $last" | tee -a "$OUT/summary.txt"
done

# Device and driver as reported by the first test which printed them
first_device=$(cat "$OUT"/*/host_trace.log 2>/dev/null | grep -m3 "\] device" | sed 's/^[0-9]*: \[kt-host-trace *[0-9.]*\] //')
if [ -n "$first_device" ]; then
    { echo "--- device (from the host trace)"; echo "$first_device"; } >> "$OUT/run_info.txt"
fi

echo
echo "===== $OUT/summary.txt"
cat "$OUT/summary.txt"
