#!/usr/bin/env python3
"""Python twin of scripts/check-baselines.ps1 (the Linux image has python3, not pwsh).

Compares a Catch2 JSON report against scripts/automation-baselines.json, keyed by
(name, configuration, invocation). Exit 1 on a missing report, a malformed report,
a run that asserted nothing, a regressed count, or a lane with NO baseline row that
scripts/baseline-exemptions.json does not exempt (configuration + invocation + a
non-empty reason). Keep the two checkers in step.

  check-baselines.py -ReportPath R.json -Configuration Debug -Invocation "~[gpu] linux"
"""
import argparse
import json
import os
import sys


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-ReportPath", "--report-path", dest="report", required=True)
    ap.add_argument("-Configuration", "--configuration", dest="config", required=True,
                    choices=["Debug", "Release", "Dist"])
    ap.add_argument("-Invocation", "--invocation", dest="invocation", required=True)
    ap.add_argument("-BaselinePath", "--baseline-path", dest="baseline", default=None)
    a = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    baseline_path = a.baseline or os.path.join(here, "automation-baselines.json")
    exemption_path = os.path.join(os.path.dirname(os.path.abspath(baseline_path)), "baseline-exemptions.json")

    if not os.path.isfile(a.report):
        print(f"no Catch2 report at {a.report}")
        return 1
    if not os.path.isfile(baseline_path):
        print(f"no baseline file at {baseline_path}")
        return 1
    with open(a.report, encoding="utf-8") as f:
        report = json.load(f)
    with open(baseline_path, encoding="utf-8") as f:
        baseline = json.load(f)
    exemptions = []
    if os.path.isfile(exemption_path):
        with open(exemption_path, encoding="utf-8") as f:
            exemptions = json.load(f).get("exemptions", [])

    totals = (report.get("test-run") or {}).get("totals")
    leaves = {}
    try:
        leaves = {
            "assertions.passed": totals["assertions"]["passed"],
            "assertions.failed": totals["assertions"]["failed"],
            "'test-cases'.passed": totals["test-cases"]["passed"],
            "'test-cases'.failed": totals["test-cases"]["failed"],
        }
    except (TypeError, KeyError):
        print(f"the report at {a.report} has no complete 'test-run.totals'. Catch2 JSON shape has moved.")
        return 1

    assertions = int(leaves["assertions.passed"]) + int(leaves["assertions.failed"])
    cases = int(leaves["'test-cases'.passed"]) + int(leaves["'test-cases'.failed"])
    if assertions <= 0 or cases <= 0:
        print(f"{a.config} ran {cases} case(s) / {assertions} assertion(s). A run that verified nothing is not a pass.")
        return 1

    failed = False
    for name, actual, unit in (("arcanetests.assertions", assertions, "assertions"),
                               ("arcanetests.cases", cases, "cases")):
        entry = next((b for b in baseline.get("baselines", [])
                      if b.get("name") == name and b.get("configuration") == a.config
                      and str(b.get("invocation") or "") == a.invocation), None)
        tag = f"[{a.config}/{a.invocation}]"
        if entry is None:
            exempt = next((e for e in exemptions
                           if e.get("configuration") == a.config and str(e.get("invocation") or "") == a.invocation
                           and str(e.get("reason") or "").strip()), None)
            if exempt:
                print(f"telemetry: {name} {tag} = {actual} {unit} (EXEMPT -- not checked: {exempt['reason']})")
                continue
            print(f"telemetry: {name} {tag} = {actual} {unit} -- NO BASELINE and no entry in "
                  "scripts/baseline-exemptions.json. Measure it and commit a row, or exempt it with a reason.")
            failed = True
            continue
        delta = actual - int(entry["value"])
        if delta < 0:
            print(f"telemetry: {name} {tag} = {actual} {unit}, baseline {entry['value']} -- REGRESSED by {abs(delta)}. "
                  "Coverage was lost, or the baseline needs a reviewed edit.")
            failed = True
        else:
            print(f"telemetry: {name} {tag} = {actual} {unit}, baseline {entry['value']} (+{delta})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
