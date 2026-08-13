#! /usr/bin/env python3
"""Run and inspect the test-harness estate.

The estate is declared in harness/registry.toml -- the rigs that verify the
firmware but are not ctest unit specs: differentials, virtual-time simulations,
sanitizer stress runs, and fuzzers, in Rust, C++ and bash alike.

    ./dbt harness list              # the estate, with prerequisites resolved
    ./dbt harness run <name>        # build + run one harness
    ./dbt harness doctor            # what is missing, and how to get it
    ./dbt harness ci --tier pr      # matrix JSON for the workflows
    ./dbt harness clean             # drop the shared cargo target dir
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import util

sys.path.insert(0, str(Path(__file__).parent))

import harness_registry as hr

# One shared cargo target dir for every Rust harness. Cargo namespaces artifacts
# by fingerprint, so packages with divergent [patch] sections coexist safely; the
# cost is that concurrent harness runs serialize on cargo's target-dir lock.
SHARED_TARGET_SUBDIR = Path("target") / "harness"


def argparser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog="harness",
        description="Run and inspect the test-harness estate (harness/registry.toml)",
    )
    parser.group = "Development"
    sub = parser.add_subparsers(dest="command")

    p_list = sub.add_parser("list", help="Show the estate")
    p_list.add_argument(
        "--markdown", action="store_true", help="Emit the README table instead"
    )

    p_run = sub.add_parser("run", help="Build and run one harness")
    p_run.add_argument("name", help="Harness name (see `dbt harness list`)")
    p_run.add_argument(
        "--skip-checks",
        action="store_true",
        help="Run even if a prerequisite probe fails",
    )

    sub.add_parser("doctor", help="Report missing prerequisites")

    p_ci = sub.add_parser("ci", help="Emit matrix JSON for a CI tier")
    p_ci.add_argument("--tier", choices=["pr", "nightly"], required=True)

    sub.add_parser("clean", help="Remove the shared cargo target dir")

    return parser


def _load(root: Path) -> list[hr.Harness]:
    try:
        return hr.load_registry(root)
    except (OSError, ValueError) as exc:
        print(f"harness: {exc}", file=sys.stderr)
        raise SystemExit(1) from exc


def cmd_list(root: Path, markdown: bool) -> int:
    entries = _load(root)

    if markdown:
        print(hr.markdown_table(entries), end="")
        return 0

    width = max(len(h.name) for h in entries)
    for h in entries:
        missing = [r for r in h.requires if not hr.probe(r, root)]
        mark = "OK " if not missing else "-- "
        note = "" if not missing else f"  (needs: {', '.join(missing)})"
        print(
            f"{mark} {h.name:<{width}}  {h.status:<13} {h.ci:<7} {h.runtime:<6}{note}"
        )
        print(f"    {' ' * width}  {h.gates}")
    return 0


def cmd_doctor(root: Path) -> int:
    entries = _load(root)
    needed = {r for h in entries for r in h.requires}
    missing = sorted(r for r in needed if not hr.probe(r, root))

    if not missing:
        print("All harness prerequisites satisfied.")
        return 0

    print("Missing prerequisites:\n")
    for req in missing:
        blocked = [h.name for h in entries if req in h.requires]
        print(f"  {req}")
        print(f"    fix:     {hr.PROBES[req]}")
        print(f"    blocks:  {', '.join(blocked)}\n")
    return 1


def cmd_run(root: Path, name: str, skip_checks: bool) -> int:
    entries = _load(root)
    match = next((h for h in entries if h.name == name), None)
    if match is None:
        print(f"harness: unknown harness '{name}'", file=sys.stderr)
        print(f"known: {', '.join(h.name for h in entries)}", file=sys.stderr)
        return 1

    if match.status == "unavailable":
        print(f"warning: '{name}' is marked unavailable -- {match.gates}\n")

    missing = [r for r in match.requires if not hr.probe(r, root)]
    if missing and not skip_checks:
        print(f"harness: '{name}' needs {', '.join(missing)}", file=sys.stderr)
        for req in missing:
            print(f"  {req}: {hr.PROBES[req]}", file=sys.stderr)
        print("\nRun with --skip-checks to try anyway.", file=sys.stderr)
        return 1

    env = dict(os.environ, **match.env)
    env.setdefault("CARGO_TARGET_DIR", str(root / SHARED_TARGET_SUBDIR))
    workdir = root / match.path

    for phase, argv in (("build", match.build), ("run", match.run)):
        if not argv:
            continue
        print(f"==> {name}: {phase}: {' '.join(argv)}")
        result = subprocess.run(argv, cwd=workdir, env=env, check=False)
        if result.returncode != 0:
            print(f"harness: '{name}' {phase} failed", file=sys.stderr)
            return result.returncode

    return 0


def cmd_ci(root: Path, tier: str) -> int:
    entries = _load(root)
    names = [h.name for h in entries if h.ci == tier and h.status != "unavailable"]
    print(json.dumps({"harness": names}))
    return 0


def cmd_clean(root: Path) -> int:
    import shutil

    target = root / SHARED_TARGET_SUBDIR
    if target.is_dir():
        shutil.rmtree(target)
        print(f"removed {target}")
    else:
        print(f"nothing to remove at {target}")
    return 0


def main() -> int:
    args = argparser().parse_args()
    root = Path(util.get_git_root()).absolute()

    match args.command:
        case "list":
            return cmd_list(root, args.markdown)
        case "run":
            return cmd_run(root, args.name, args.skip_checks)
        case "doctor":
            return cmd_doctor(root)
        case "ci":
            return cmd_ci(root, args.tier)
        case "clean":
            return cmd_clean(root)
        case _:
            argparser().print_help()
            return 1


if __name__ == "__main__":
    sys.exit(main())
