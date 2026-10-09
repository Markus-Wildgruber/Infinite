#!/usr/bin/env python3
"""Header fan-in report: which headers force the most recompiles.

For every project header, counts the translation units (TUs) that read it
during the last build, multiplies by how often the header was edited in git
history, and ranks the result: "TU recompiles caused" over the window. This is
the number docs/plans/compile-times/include-split.md tracks.

Dependency sources (whichever the build left behind):
  MSVC / Visual Studio : build/Infinite.dir/<Config>/Infinite.tlog/
                         Microsoft.Build.CPPTasks.CL.read.1.tlog (or CL.read.1.tlog)
  Ninja (macOS, Linux) : `ninja -C <build> -t deps` output, piped to a file

Usage:
  python tools/include-fanin.py --tlog build/Infinite.dir/Release/Infinite.tlog/Microsoft.Build.CPPTasks.CL.read.1.tlog
  ninja -C build -t deps > deps.txt && python tools/include-fanin.py --ninja-deps deps.txt
Options:
  --days N        git history window (default 120)
  --top N         rows to print (default 40)
  --group PATH    also report the "umbrella": headers read by EVERY TU that
                  reads PATH (default src/app/AppCommon.h)
  --max-umbrella N  exit 1 if the umbrella has more than N project headers
                    (the Phase 5 guardrail; off by default)
"""
import argparse
import collections
import os
import subprocess
import sys

SRC_EXT = (".cpp", ".c", ".mm", ".m", ".cc", ".cxx")


def norm(path, root):
    """Repo-relative, forward slashes, lower case (tlog paths are upper case)."""
    p = os.path.normcase(os.path.abspath(path)).replace("\\", "/").lower()
    return p[len(root):] if p.startswith(root) else None


def read_tlog(path, root):
    tus, cur = {}, None
    with open(path, encoding="utf-16") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            if line.startswith("^"):
                # "^A.CPP|B.CPP" when several sources share one record
                cur = norm(line[1:].split("|")[0], root)
                if cur:
                    tus.setdefault(cur, set())
            elif cur:
                h = norm(line, root)
                if h and not h.endswith(SRC_EXT):
                    tus[cur].add(h)
    return tus


def read_ninja_deps(path, root, build_dir):
    # Format: "<obj>: #deps N, deps mtime M (VALID)" then indented dep paths;
    # the first dep is the source file itself.
    tus, cur, first = {}, None, False
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if not line.strip():
                continue
            if not line[0].isspace():
                cur, first = None, True
                continue
            dep = line.strip()
            if not os.path.isabs(dep):
                dep = os.path.join(build_dir, dep)
            rel = norm(dep, root)
            if first:
                first = False
                if rel and rel.endswith(SRC_EXT):
                    cur = rel
                    tus.setdefault(cur, set())
                continue
            if cur and rel and not rel.endswith(SRC_EXT):
                tus[cur].add(rel)
    return tus


def git_churn(repo, days):
    out = subprocess.run(["git", "-C", repo, "log", f"--since={days}.days", "--name-only", "--format=format:"],
                         capture_output=True, text=True, encoding="utf-8", errors="replace").stdout
    churn = collections.Counter()
    for fn in out.splitlines():
        fn = fn.strip()
        if fn.endswith((".h", ".hpp", ".inl")):
            churn[fn.lower()] += 1
    return churn


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--tlog")
    src.add_argument("--ninja-deps")
    ap.add_argument("--build-dir", default="build", help="for relative paths in --ninja-deps output")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    ap.add_argument("--days", type=int, default=120)
    ap.add_argument("--top", type=int, default=40)
    ap.add_argument("--group", default="src/app/AppCommon.h")
    ap.add_argument("--max-umbrella", type=int, default=None)
    a = ap.parse_args()

    root = os.path.normcase(os.path.abspath(a.repo)).replace("\\", "/").lower().rstrip("/") + "/"
    tus = read_tlog(a.tlog, root) if a.tlog else read_ninja_deps(a.ninja_deps, root, os.path.abspath(a.build_dir))
    if not tus:
        sys.exit("no translation units found - wrong file, or the build has not run")

    project = lambda h: h.startswith("src/")
    fanin = collections.Counter(h for hs in tus.values() for h in hs if project(h))
    churn = git_churn(a.repo, a.days)

    rows = sorted(((n * churn.get(h, 0), n, churn.get(h, 0), h) for h, n in fanin.items()), reverse=True)
    total = sum(r[0] for r in rows)
    edits = sum(r[2] for r in rows)
    print(f"TUs: {len(tus)}   project headers read: {len(fanin)}   window: {a.days} days")
    print(f"TU recompiles caused by header edits: {total}   header edits: {edits}   "
          f"avg TUs per header edit: {total / max(1, edits):.0f}")
    print(f"\n{'recompiles':>10} = {'TUs':>4} x {'edits':>5}  header")
    for cost, n, c, h in rows[:a.top]:
        print(f"{cost:10d} = {n:4d} x {c:5d}  {h}")

    group = a.group.lower()
    members = [t for t, hs in tus.items() if group in hs]
    if members:
        umbrella = sorted(h for h in set.intersection(*(tus[t] for t in members)) if project(h))
        cost = sum(r[0] for r in rows if r[3] in set(umbrella))
        counts = sorted(len([h for h in tus[t] if project(h)]) for t in members)
        print(f"\nTUs reading {a.group}: {len(members)}   project headers per such TU: "
              f"min {counts[0]}, median {counts[len(counts) // 2]}, max {counts[-1]}")
        print(f"umbrella (read by every one of them): {len(umbrella)} project headers, "
              f"whose edits cause {100 * cost / max(1, total):.0f}% of all header-driven recompiles")
        by_dir = collections.Counter(os.path.dirname(h) for h in umbrella)
        print("umbrella by directory: " + ", ".join(f"{d} {n}" for d, n in by_dir.most_common()))
        if a.max_umbrella is not None and len(umbrella) > a.max_umbrella:
            print(f"\nFAIL: umbrella has {len(umbrella)} project headers, budget is {a.max_umbrella}")
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
