"""Replays every captured GE draw dump and collects the per-dump reports.

    python run_all.py [--scale N] [--msaa N] [--out DIR] [extra tool options...]

Writes one folder per dump (PNG images + report.json) and a summary.json /
summary.txt next to them. Dumps are read from profiles/chili/analysis.
"""
import glob
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", "..", ".."))
TOOL = os.path.join(ROOT, "out", "chili-gpu", "chili_ge_dump_replay.exe")
ANALYSIS = os.path.join(ROOT, "profiles", "chili", "analysis")
DEFAULT_OUT = os.path.join(ROOT, "profiles", "chili", "progress", "gpu_bringup", "replay")


def is_dump(path):
    try:
        with open(path, "r", errors="replace") as handle:
            head = handle.read(4096)
    except OSError:
        return False
    return "\nreg 0 " in head and "primitive=" in head


def main(argv):
    out = DEFAULT_OUT
    extra = []
    i = 0
    while i < len(argv):
        if argv[i] == "--out":
            out = os.path.abspath(argv[i + 1])
            i += 2
        else:
            extra.append(argv[i])
            i += 1
    dumps = sorted(p for p in glob.glob(os.path.join(ANALYSIS, "*.txt")) if is_dump(p))
    if not dumps:
        print("no dumps found in", ANALYSIS)
        return 1
    os.makedirs(out, exist_ok=True)
    reports = []
    for dump in dumps:
        name = os.path.splitext(os.path.basename(dump))[0]
        run = subprocess.run([TOOL, dump, "--out", out] + extra, capture_output=True, text=True)
        report_path = os.path.join(out, name, "report.json")
        if run.returncode != 0 or not os.path.exists(report_path):
            reports.append({"dump": name, "tool_error": (run.stderr or run.stdout).strip()[-2000:]})
            continue
        with open(report_path, "r") as handle:
            report = json.load(handle)
        report["stderr"] = run.stderr.strip()[-2000:]
        reports.append(report)

    with open(os.path.join(out, "summary.json"), "w") as handle:
        json.dump(reports, handle, indent=1)

    lines = []
    header = "%-46s %9s | %-7s %8s %5s %7s | %-7s %8s %5s %7s  %s" % (
        "dump", "ref px", "sw", "rgb>tol", "max", "alpha", "d3d12", "rgb>tol", "max", "alpha", "d3d12 notes")
    lines.append(header)
    lines.append("-" * len(header))
    for r in reports:
        if "tool_error" in r:
            lines.append("%-46s TOOL ERROR: %s" % (r["dump"][:46], r["tool_error"]))
            continue
        ref_ok = r["reference"]["ok"]

        def cell(path_key, diff_key):
            path = r[path_key]
            status = "ok" if path["ok"] else ("skipped" if not path["ran"] else "FAILED")
            diff = r[diff_key]
            if not ref_ok or not path["ok"]:
                return "%-7s %8s %5s %7s" % (status, "-", "-", "-")
            return "%-7s %8d %5d %7d" % (status, diff["rgb_over_tolerance"], diff["max_rgb"],
                                         diff["alpha_over_tolerance"])

        notes = r["d3d12"]["notes"]
        if r["d3d12"]["fallbacks"]:
            notes += " FALLBACK: " + r["d3d12"]["fallbacks"]
        if r["d3d12"]["error"]:
            notes += " ERROR: " + r["d3d12"]["error"]
        if not ref_ok:
            notes += " REFERENCE FAILED: " + r["reference"]["error"]
        lines.append("%-46s %9s | %s | %s  %s" % (
            r["dump"][:46], r["pixels_changed"]["reference"] if ref_ok else "-",
            cell("frontend_software", "frontend_software_vs_reference"),
            cell("d3d12", "d3d12_vs_reference"), notes.strip()))
    text = "\n".join(lines)
    with open(os.path.join(out, "summary.txt"), "w") as handle:
        handle.write(text + "\n")
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
