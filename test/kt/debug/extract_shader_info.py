#!/usr/bin/env python3
# [DO NOT MERGE] Summarizes the register file and resource usage of every kernel in IGC shader dumps
# (IGC_ShaderDumpEnable=1 IGC_DumpToCustomDir=<dir>).
#
# From the .zeinfo files (what the driver reads): grf_count, simd_size, barrier_count, eu_thread_count, slm_size and
# per-thread scratch / private memory. From the .asm files (what the compiler produced): numGRF, numAcc, the spill
# lines and the instruction count.
#
# Usage: extract_shader_info.py <dump dir> [<dump dir> ...] [--csv <file>]
import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

ZEINFO_ENV_KEYS = ("grf_count", "simd_size", "barrier_count", "eu_thread_count", "slm_size", "private_size",
                   "spill_size", "has_stack_calls", "indirect_stateless_count")


def demangle(names):
    if not names or not shutil.which("c++filt"):
        return {n: n for n in names}
    out = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.splitlines()
    return {n: d.replace("typeinfo name for ", "") for n, d in zip(names, out)}


def short_name(demangled):
    """Kernel kind followed by its leading template arguments, e.g. __radix_sort_onesweep_kernel<..., 8, 7, 1024>"""
    m = re.search(r"__impl::(\w+)<(.*)", demangled)
    if not m:
        return demangled[:110]
    args, depth, current = [], 0, ""
    for ch in m.group(2):
        if ch == "<":
            depth += 1
        elif ch == ">":
            if depth == 0:
                break
            depth -= 1
        if ch == "," and depth == 0:
            args.append(current.strip())
            current = ""
        else:
            current += ch
    args.append(current.strip())
    simple = [a.replace("oneapi::dpl::experimental::kt::gpu::__impl::", "") for a in args if len(a) < 40]
    return f"{m.group(1)}<{', '.join(simple[:6])}>"


def parse_zeinfo(path):
    kernels, current, section, buffers, top_level = [], None, None, None, None
    for line in open(path, errors="replace"):
        top = re.match(r"(\w+):", line)
        if top:
            top_level = top.group(1)
            current = None
            continue
        if top_level != "kernels":
            continue
        name = re.match(r"\s{2}- name:\s+(\S+)", line)
        if name:
            current = {"name": name.group(1), "zeinfo": os.path.basename(path)}
            kernels.append(current)
            section = None
            continue
        if current is None:
            continue
        key = re.match(r"\s{4}(\w+):\s*$", line)
        if key:
            section = key.group(1)
            continue
        if section == "execution_env":
            kv = re.match(r"\s{6}(\w+):\s+(.+?)\s*$", line)
            if kv and kv.group(1) in ZEINFO_ENV_KEYS:
                current[kv.group(1)] = kv.group(2)
        elif section == "per_thread_memory_buffers":
            item = re.match(r"\s{6}- type:\s+(\S+)", line)
            if item:
                buffers = item.group(1)
            size = re.match(r"\s{8}size:\s+(\d+)", line)
            if size and buffers:
                current[f"ptmb_{buffers}"] = size.group(1)
    return kernels


def parse_asm(path):
    info = {"asm": os.path.basename(path), "spills": []}
    with open(path, errors="replace") as f:
        for i, line in enumerate(f):
            if line.startswith("//.kernel "):
                info["name"] = line.split()[1]
            elif line.startswith("//.thread_config"):
                for k, v in re.findall(r"(\w+)=(\d+)", line):
                    info[k] = v
            elif line.startswith("//.spill"):
                info["spills"].append(line[3:].strip())
            elif line.startswith("//.instCount"):
                info["instCount"] = line.split()[1]
            if i > 200 and "name" in info and "numGRF" in info:
                break
    return info


def summarize(dump_dir):
    kernels = []
    for z in sorted(glob.glob(os.path.join(dump_dir, "**", "*.zeinfo"), recursive=True)):
        kernels.extend(parse_zeinfo(z))
    asm_by_name = {}
    for a in sorted(glob.glob(os.path.join(dump_dir, "**", "*_entry_*.asm"), recursive=True)):
        info = parse_asm(a)
        if "name" in info:
            asm_by_name.setdefault(info["name"], info)
    names = demangle(sorted({k["name"] for k in kernels}))
    rows = []
    for k in kernels:
        a = asm_by_name.get(k["name"], {})
        rows.append({
            "dump": dump_dir,
            "kernel": short_name(names.get(k["name"], k["name"])),
            "grf_count": k.get("grf_count", "?"),
            "asm_numGRF": a.get("numGRF", "?"),
            "numAcc": a.get("numAcc", "?"),
            "simd": k.get("simd_size", "?"),
            "barriers": k.get("barrier_count", "0"),
            "eu_thread_count": k.get("eu_thread_count", "?"),
            "slm_size": k.get("slm_size", "0"),
            "scratch": k.get("ptmb_scratch", k.get("spill_size", "0")),
            "private": k.get("ptmb_private_space", k.get("private_size", "0")),
            "instCount": a.get("instCount", "?"),
            "spills": "; ".join(a.get("spills", [])) or "none",
            "mangled": k["name"],
        })
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dump_dirs", nargs="+")
    parser.add_argument("--csv", help="also write all rows to this CSV file")
    args = parser.parse_args()

    columns = ["kernel", "grf_count", "asm_numGRF", "numAcc", "simd", "barriers", "eu_thread_count", "slm_size",
               "scratch", "private", "instCount", "spills"]
    all_rows = []
    for d in args.dump_dirs:
        rows = summarize(d)
        all_rows.extend(rows)
        print(f"== {d}: {len(rows)} kernels")
        if not rows:
            continue
        unique = {tuple(r[c] for c in columns): r for r in rows}
        widths = {c: max(len(c), *(len(str(r[c])) for r in unique.values())) for c in columns}
        print("  " + "  ".join(c.ljust(widths[c]) for c in columns))
        for r in sorted(unique.values(), key=lambda r: r["kernel"]):
            print("  " + "  ".join(str(r[c]).ljust(widths[c]) for c in columns))
    if args.csv:
        import csv
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=["dump", *columns, "mangled"])
            writer.writeheader()
            writer.writerows(all_rows)
    return 0 if all_rows else 1


if __name__ == "__main__":
    sys.exit(main())
