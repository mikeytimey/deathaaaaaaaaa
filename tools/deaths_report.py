"""
deaths_report.py - print Death Buffer's saved data in CMD.

    python deaths_report.py                 all levels, worst spots first
    python deaths_report.py bloodbath       only levels whose name contains "bloodbath"
    python deaths_report.py --file path\\to\\deaths.json

By default it reads %LOCALAPPDATA%\\GeometryDash\\geode\\mods\\local.death-buffer\\deaths.json
"""
import argparse
import json
import os
import sys
from collections import Counter
from pathlib import Path

MOD_ID = "local.death-buffer"


def default_path():
    base = os.environ.get("LOCALAPPDATA", str(Path.home()))
    return Path(base) / "GeometryDash" / "geode" / "mods" / MOD_ID / "deaths.json"


def bar(n, top, width=30):
    return "#" * max(1, round(width * n / top)) if top else ""


def report(name, lv, top_spots, top_pcts):
    deaths = lv.get("deaths", [])
    spots = sorted(lv.get("spots", []), key=lambda s: s.get("deaths", 0), reverse=True)
    practice = sum(1 for d in deaths if d.get("practice"))
    print(f"\n=== {name}  -  {len(deaths)} deaths ({len(deaths) - practice} normal, {practice} practice)")
    if not deaths:
        return

    print("\n  Death spots                 buffer")
    for s in spots[:top_spots]:
        if s.get("target") is None:
            status = "not learned (get past it once)"
        else:
            status = f"learned, used {s.get('buffered', 0)}x, passed {s.get('passes', 0)}x"
        print(f"  {s.get('pct', 0):6.2f}%  {s.get('deaths', 0):5} deaths   {status}")

    counts = Counter(int(d.get("pct", 0)) for d in deaths)
    top = max(counts.values())
    print("\n  Deaths by percent")
    for pct, n in sorted(counts.items(), key=lambda kv: kv[1], reverse=True)[:top_pcts]:
        print(f"  {pct:3}%  {n:5}  {bar(n, top)}")


def main():
    ap = argparse.ArgumentParser(description="Show Death Buffer stats")
    ap.add_argument("level", nargs="?", default="", help="part of a level name to filter by")
    ap.add_argument("--file", type=Path, default=default_path(), help="path to deaths.json")
    ap.add_argument("--spots", type=int, default=10, help="death spots to show per level")
    ap.add_argument("--pcts", type=int, default=10, help="percent rows to show per level")
    args = ap.parse_args()

    if not args.file.exists():
        sys.exit(f"No data yet at {args.file}\nPlay a level with the mod on, die once, then run this again.")

    data = json.loads(args.file.read_text(encoding="utf-8"))
    levels = [(lv.get("name") or key, lv) for key, lv in data.items()]
    if args.level:
        levels = [(n, lv) for n, lv in levels if args.level.lower() in n.lower()]
        if not levels:
            sys.exit(f'No level matching "{args.level}".')

    levels.sort(key=lambda item: len(item[1].get("deaths", [])), reverse=True)
    for name, lv in levels:
        report(name, lv, args.spots, args.pcts)
    print()


if __name__ == "__main__":
    main()
