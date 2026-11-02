#!/usr/bin/env python3
"""Merges the results.jsonl of every job into one table for the job summary.

Usage: summarize.py <folder holding results.jsonl files> [--summary <file>] [--json <file>]
A check counts as unstable when the same case passed and failed across repeats, with its rate.
known_limits.json lists failures with a proven cause outside Gridline; they do not fail the run but
are shown with their reason. Exit code 1 on any other failure.
"""
import argparse
import collections
import json
import re
from pathlib import Path

DIMS = ("wine", "display", "scale", "host", "format", "mode", "plugin")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("folder")
    ap.add_argument("--summary")
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = []
    for f in sorted(Path(a.folder).rglob("results.jsonl")):
        rows += [json.loads(line) for line in f.read_text().splitlines() if line.strip()]
    limits_file = Path(__file__).parent / "known_limits.json"
    limits = json.loads(limits_file.read_text()) if limits_file.exists() else []

    def limit_of(row):
        for lim in limits:
            if all(re.search(v, str(row.get(k, ""))) for k, v in lim["match"].items()):
                return lim["reason"]
        return None

    cases = collections.defaultdict(list)
    for row in rows:
        cases[tuple(row.get(d, "") for d in DIMS) + (row["check"],)].append(row)
    table, unexplained = [], 0
    for key, runs in sorted(cases.items()):
        statuses = [r["status"] for r in runs]
        fails = statuses.count("fail")
        passes = statuses.count("pass")
        if fails and passes:
            status = "unstable"
        elif fails:
            status = "fail"
        elif passes:
            status = "pass"
        else:
            status = "skip"
        detail = next((r["detail"] for r in runs if r["status"] == "fail"), runs[-1]["detail"])
        reason = limit_of(runs[0]) if fails else None
        if fails and not reason:
            unexplained += 1
        table.append(dict(zip(DIMS, key), check=key[-1], status=status, passed=passes, failed=fails, runs=len(runs), detail=detail, known=reason))

    per = collections.defaultdict(collections.Counter)
    for t in table:
        per[tuple(t[d] for d in DIMS)]["known" if t["known"] else t["status"]] += 1
    out = ["# yabridge test results", "", f"{len(rows)} results, {len(table)} cases, {unexplained} unexplained failures.", "",
           "| Wine | Display | Scale | Host | Format | Mode | Plugin | Pass | Fail | Unstable | Known limit | Skip |", "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for k, c in sorted(per.items()):
        out.append("| " + " | ".join(str(v) for v in k) + f" | {c['pass']} | {c['fail']} | {c['unstable']} | {c['known']} | {c['skip']} |")
    bad = [t for t in table if t["status"] in ("fail", "unstable")]
    if bad:
        out += ["", "## Failures", "", "| Wine | Display | Scale | Host | Format | Mode | Plugin | Check | Rate | Detail |", "|---|---|---|---|---|---|---|---|---|---|"]
        for t in bad[:400]:
            d = str(t["detail"]).replace("|", "/").replace("\n", " ")[:220]
            label = f"known: {t['known']}; " if t["known"] else ""
            out.append(f"| {t['wine']} | {t['display']} | {t['scale']} | {t['host']} | {t['format']} | {t['mode']} | {t['plugin']} | {t['check']} | {t['failed']}/{t['runs']} failed | {label}{d} |")
    skips = [t for t in table if t["status"] == "skip"]
    if skips:
        seen = {}
        for t in skips:
            seen.setdefault((t["check"], str(t["detail"])[:160]), 0)
            seen[(t["check"], str(t["detail"])[:160])] += 1
        out += ["", "## Skipped", "", "| Check | Reason | Cases |", "|---|---|---|"]
        out += [f"| {c} | {d.replace('|', '/')} | {n} |" for (c, d), n in sorted(seen.items())]
    text = "\n".join(out) + "\n"
    print(text)
    if a.summary:
        with open(a.summary, "a") as f:
            f.write(text[:900000])
    if a.json:
        Path(a.json).write_text(json.dumps(table, indent=1))
    if not rows:
        print("no results at all: the jobs did not get as far as testing")
        return 1
    return 1 if unexplained else 0


if __name__ == "__main__":
    raise SystemExit(main())
