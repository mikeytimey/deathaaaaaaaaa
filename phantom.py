"""
phantom.py - the Phantom Click console.

    python phantom.py                         interactive menu (pick a level, edit its ranges)
    python phantom.py levels                  list levels
    python phantom.py show bloodbath          death ranges for a level
    python phantom.py set bloodbath 51 3      3 ticks of buffer in 50.5-51.5%
    python phantom.py set bloodbath 51 2 delay   hold EVERY click in 50.5-51.5% exactly 2 ticks
    python phantom.py set bloodbath 51 off    no buffer in that range
    python phantom.py clear bloodbath 51      back to the default (or "all")
    python phantom.py map bloodbath           open the level map in your browser

Levels can be picked by part of their name, their ID, or their number in "levels".
Changes are picked up by the game on your next attempt - no restart needed.
"""
import argparse
import json
import math
import os
import statistics
import sys
import tempfile
import webbrowser
from pathlib import Path

MOD_ID = "local.phantom-click"

# ------------------------------------------------------------------ colours --
os.system("")  # turns on colour codes in the Windows console
USE_COLOR = sys.stdout.isatty() and "--no-color" not in sys.argv
STATUS_COLOR = {"on": "92", "learning": "93", "deaths": "91", "delay": "94", "off": "90"}


def c(text, code):
    return f"\033[{code}m{text}\033[0m" if USE_COLOR else str(text)


def bold(text):
    return c(text, "1")


def dim(text):
    return c(text, "90")


# --------------------------------------------------------------------- data --
def default_dir():
    base = os.environ.get("LOCALAPPDATA", str(Path.home()))
    return Path(base) / "GeometryDash" / "geode" / "mods" / MOD_ID


def read_json(path, default):
    try:
        return json.loads(Path(path).read_text(encoding="utf-8"))
    except FileNotFoundError:
        return default
    except json.JSONDecodeError as e:
        sys.exit(f"{path} is damaged ({e}). Delete it or fix it, then try again.")


def write_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, suffix=".tmp")
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
    os.replace(tmp, path)  # atomic: the game never sees a half-written file


def safe_name(key):
    return "".join(ch if ch.isalnum() else "_" for ch in key)


def range_index(pct, width):
    return math.floor(pct / width + 0.5)


def range_bounds(idx, width):
    return idx * width - width / 2, idx * width + width / 2


def fmt_pct(v):
    return f"{v:.4g}"


def range_label(idx, width):
    lo, hi = range_bounds(idx, width)
    return f"{fmt_pct(lo)}-{fmt_pct(hi)}%"


def range_key(idx, width):
    return fmt_pct(idx * width)  # "51", "51.5"


class Store:
    def __init__(self, folder):
        self.dir = Path(folder)
        self.reload()

    def reload(self):
        self.db = read_json(self.dir / "deaths.json", None)
        if self.db is None:
            sys.exit(f"No Phantom Click data in {self.dir}\n"
                     "Play a level with the mod installed and die once, then run this again.")
        self.rules_path = self.dir / "ranges.json"
        self.rules = read_json(self.rules_path, {})

    def levels(self):
        out = [(key, lv) for key, lv in self.db.items()]
        out.sort(key=lambda kv: len(kv[1].get("deaths", [])), reverse=True)
        return out

    def find(self, query):
        levels = self.levels()
        q = str(query).strip().lower()
        if q.isdigit() and 1 <= int(q) <= len(levels) and q not in self.db:
            return levels[int(q) - 1]
        for key, lv in levels:
            if key.lower() == q:
                return key, lv
        hits = [(k, lv) for k, lv in levels if q in (lv.get("name") or k).lower()]
        if len(hits) == 1:
            return hits[0]
        if not hits:
            sys.exit(f'No level matching "{query}". Run "python phantom.py levels" to see them.')
        names = ", ".join(lv.get("name") or k for k, lv in hits[:6])
        sys.exit(f'"{query}" matches several levels ({names}). Be more specific or use the number.')

    def level_rules(self, key):
        return self.rules.get(key, {})

    def set_rule(self, key, idx, width, ticks=None, mode=None):
        lv_rules = self.rules.setdefault(key, {})
        rule = lv_rules.setdefault(range_key(idx, width), {})
        if ticks is not None:
            rule["ticks"] = ticks
        if mode is not None:
            rule["mode"] = mode
        self.save_rules()

    def clear_rule(self, key, idx=None, width=1.0):
        if idx is None:
            self.rules.pop(key, None)
        else:
            lv_rules = self.rules.get(key, {})
            target = idx * width
            for k in list(lv_rules):
                try:
                    if abs(float(k) - target) < 1e-9 or range_index(float(k), width) == idx:
                        del lv_rules[k]
                except ValueError:
                    pass
            if not lv_rules:
                self.rules.pop(key, None)
        self.save_rules()

    def save_rules(self):
        write_json(self.rules_path, self.rules)

    def trails(self, key):
        return read_json(self.dir / "trails" / f"{safe_name(key)}.json", {})


# ------------------------------------------------------------ level analysis --
def spot_range(spot):
    hits = spot.get("rangeHits") or {}
    if hits:
        return int(max(hits.items(), key=lambda kv: kv[1])[0])
    return range_index(spot.get("pct", 0), 1.0)


def spot_ready(spot):
    return bool(spot.get("passes")) and spot.get("totalMisses", 0) >= 1


def miss_pattern(spot):
    passes, misses, speed = spot.get("passes", []), spot.get("misses", []), spot.get("speed", 0)
    if not passes or not misses or speed <= 0:
        return ""
    right = sorted(passes)[len(passes) // 2]
    offs = [(m - right) / speed for m in misses]
    early = [o for o in offs if o < -0.5]
    late = [o for o in offs if o > 0.5]
    parts = []
    if early:
        parts.append(f"{len(early)} early ~{-statistics.median(early):.1f}t")
    if late:
        parts.append(f"{len(late)} late ~{statistics.median(late):.1f}t")
    return ", ".join(parts)


def analyse(store, key, lv):
    """Everything about a level's death ranges, sorted by deaths."""
    width = float(lv.get("rangeWidth") or 1.0)
    rules = store.level_rules(key)
    rule_by_idx = {}
    for k, r in rules.items():
        try:
            rule_by_idx[range_index(float(k), width)] = r
        except ValueError:
            pass

    ranges = {}
    for d in lv.get("deaths", []):
        idx = range_index(d.get("pct", 0), width)
        r = ranges.setdefault(idx, {"idx": idx, "deaths": 0, "practice": 0, "spots": [], "ys": []})
        r["deaths"] += 1
        r["practice"] += 1 if d.get("practice") else 0
        r["ys"].append(d.get("y", 105))
    for s in lv.get("clicks", []):
        idx = spot_range(s)
        r = ranges.setdefault(idx, {"idx": idx, "deaths": 0, "practice": 0, "spots": [], "ys": []})
        r["spots"].append(s)
    for idx in rule_by_idx:  # ranges you set up before dying there
        ranges.setdefault(idx, {"idx": idx, "deaths": 0, "practice": 0, "spots": [], "ys": []})

    out = []
    for idx, r in ranges.items():
        rule = rule_by_idx.get(idx, {})
        ticks = rule.get("ticks")
        custom = ticks is not None or "mode" in rule
        mode = rule.get("mode", "smart")
        learned = sum(1 for s in r["spots"] if spot_ready(s))
        eff = 2 if ticks is None else int(ticks)  # 2 = the mod's default setting
        if mode == "delay" and eff > 0:
            status = "delay"
        elif eff <= 0:
            status = "off"
        elif learned:
            status = "on"
        elif r["spots"]:
            status = "learning"
        else:
            status = "deaths"
        lo, hi = range_bounds(idx, width)
        out.append({
            "idx": idx, "lo": lo, "hi": hi, "center": idx * width, "label": range_label(idx, width),
            "deaths": r["deaths"], "practice": r["practice"], "clicks": len(r["spots"]), "learned": learned,
            "ticks": ticks, "eff": eff, "mode": mode, "custom": custom, "status": status,
            "pattern": "; ".join(p for p in (miss_pattern(s) for s in r["spots"]) if p),
            "y": statistics.median(r["ys"]) if r["ys"] else None,
            "spots": r["spots"],
        })
    out.sort(key=lambda r: (-r["deaths"], r["center"]))
    return width, out


STATUS_TEXT = {
    "on": "buffer ON",
    "learning": "learning - pass it once",
    "deaths": "no click to blame yet",
    "delay": "delay: every click held",
    "off": "buffer off",
}


def print_levels(store):
    levels = store.levels()
    print()
    print(bold("  #  Level                         Deaths  Ranges  Buffer ON  Custom"))
    for i, (key, lv) in enumerate(levels, 1):
        _, ranges = analyse(store, key, lv)
        on = sum(1 for r in ranges if r["status"] in ("on", "delay"))
        custom = len(store.level_rules(key))
        name = (lv.get("name") or key)[:28]
        print(f"  {i:>2} {name:<29} {len(lv.get('deaths', [])):>6}  {len(ranges):>6}  {c(f'{on:>9}', '92')}  {custom:>6}")
    print()


def print_level(store, key, lv, limit=25):
    width, ranges = analyse(store, key, lv)
    deaths = lv.get("deaths", [])
    best = (store.trails(key).get("best") or {}).get("endPct")
    print()
    title = f" {lv.get('name') or key} "
    print(bold(c(f"=={title}" + "=" * max(4, 58 - len(title)), "95")))
    extra = f"   best run {best:.2f}%" if best is not None else ""
    print(f"  {len(deaths)} deaths in {len(ranges)} ranges ({fmt_pct(width)}% wide){extra}")
    if not ranges:
        print("  No deaths yet.\n")
        return
    top = max(r["deaths"] for r in ranges) or 1
    print()
    print(bold("  Range          Deaths                     Clicks  Ticks  Status"))
    for r in ranges[:limit]:
        bar = "#" * max(1 if r["deaths"] else 0, round(16 * r["deaths"] / top))
        ticks = f"{r['eff']}{'*' if r['custom'] else ' '}"
        if r["mode"] == "delay":
            ticks += "d"
        status = c(STATUS_TEXT[r["status"]], STATUS_COLOR[r["status"]])
        line = (f"  {r['label']:<13} {r['deaths']:>5} {c(f'{bar:<17}', '91')}"
                f"  {r['learned']:>2}/{r['clicks']:<2}  {ticks:<5}  {status}")
        if r["pattern"]:
            line += dim(f"  ({r['pattern']})")
        print(line)
    if len(ranges) > limit:
        print(dim(f"  ...and {len(ranges) - limit} more"))
    print(dim("  Clicks = learned/total clicks you died to there.  * = your own setting.  d = delay mode."))
    print()


def parse_ticks(text):
    t = text.lower()
    if t in ("off", "0"):
        return 0
    if t.isdigit() and 0 <= int(t) <= 20:
        return int(t)
    raise ValueError(f'"{text}" isn\'t a tick count (0-20 or "off")')


def apply_set(store, key, lv, pct_text, ticks_text, mode=None):
    width = float(lv.get("rangeWidth") or 1.0)
    try:
        pct = float(pct_text.rstrip("%"))
    except ValueError:
        raise ValueError(f'"{pct_text}" isn\'t a percent')
    idx = range_index(pct, width)
    if ticks_text.lower() in ("reset", "default", "clear"):
        store.clear_rule(key, idx, width)
        return f"{range_label(idx, width)} is back to the default."
    ticks = parse_ticks(ticks_text)
    if mode is not None and mode not in ("smart", "delay"):
        raise ValueError('mode must be "smart" or "delay"')
    store.set_rule(key, idx, width, ticks, mode or "smart")
    what = ("buffer off" if ticks == 0 else
            f"every click held exactly {ticks} tick(s)" if mode == "delay" else
            f"early clicks caught up to {ticks} tick(s)")
    return f"{range_label(idx, width)}: {what}. The game uses it from your next attempt."


# ----------------------------------------------------------------- the map --
def build_map_data(store, key, lv):
    width, ranges = analyse(store, key, lv)
    fit = lv.get("fit") or {}
    a = b = None
    n, sx, sy, sxx, sxy = (fit.get(k, 0) for k in ("n", "sx", "sy", "sxx", "sxy"))
    d = n * sxx - sx * sx
    if n >= 2 and abs(d) > 1e-6:
        a = (n * sxy - sx * sy) / d
        b = (sy - a * sx) / n
        if a <= 0:
            a = b = None
    if a is None and lv.get("levelLength"):
        a, b = 100.0 / lv["levelLength"], 0.0

    trails = store.trails(key)
    spots = []
    for s in lv.get("clicks", []):
        spots.append({"x": s.get("x"), "pct": s.get("pct"), "right": s.get("rightTick"),
                      "passes": s.get("passes", []), "misses": s.get("misses", []),
                      "ready": spot_ready(s), "range": spot_range(s), "pattern": miss_pattern(s),
                      "missN": s.get("totalMisses", 0), "passN": s.get("totalPasses", 0),
                      "caught": s.get("buffered", 0)})
    for r in ranges:
        r.pop("spots", None)
    return {
        "name": lv.get("name") or key, "key": key, "width": width, "a": a, "b": b,
        "deaths": [[d.get("x", 0), d.get("y", 105), d.get("pct", 0), bool(d.get("practice"))]
                   for d in lv.get("deaths", [])],
        "ranges": ranges, "spots": spots,
        "best": trails.get("best"), "recent": trails.get("recent", []),
    }


def write_map(store, key, lv, out=None, open_it=True):
    data = build_map_data(store, key, lv)
    html = MAP_HTML.replace("__TITLE__", data["name"].replace("<", "")).replace(
        "__DATA__", json.dumps(data, separators=(",", ":")).replace("</", "<\\/"))
    out = Path(out) if out else store.dir / "maps" / f"{safe_name(data['name'])}.html"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(html, encoding="utf-8")
    if open_it:
        webbrowser.open(out.resolve().as_uri())
    return out


MAP_HTML = r"""<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>__TITLE__ - Phantom Click map</title>
<style>
:root{--bg:#0b0d14;--panel:#141826;--line:#252b3d;--text:#e6e9f2;--mute:#8a93ad;--on:#3dff7a;--learn:#ffbf33;--dead:#ff4d4d;--delay:#4d8cff;--off:#9aa0ad;--accent:#c77dff}
*{box-sizing:border-box}html,body{margin:0;height:100%;background:var(--bg);color:var(--text);font:13px/1.4 system-ui,Segoe UI,Roboto,sans-serif;overflow:hidden}
header{display:flex;flex-wrap:wrap;gap:10px 18px;align-items:center;padding:10px 16px;background:var(--panel);border-bottom:1px solid var(--line)}
h1{margin:0;font-size:16px;letter-spacing:.3px}h1 b{color:var(--accent)}
.stats{display:flex;gap:14px;color:var(--mute)}.stats strong{color:var(--text)}
.toggles{display:flex;flex-wrap:wrap;gap:6px;margin-left:auto}
.toggles label{display:flex;align-items:center;gap:5px;padding:3px 9px;border:1px solid var(--line);border-radius:99px;cursor:pointer;user-select:none;color:var(--mute)}
.toggles input{accent-color:var(--accent);margin:0}.toggles label:has(input:checked){color:var(--text);border-color:#3a4260}
.sw{width:9px;height:9px;border-radius:2px;display:inline-block}
#stage{position:absolute;left:0;right:0;top:var(--top,52px);bottom:96px}
canvas{display:block;width:100%;height:100%}
#strip{position:absolute;left:0;right:0;bottom:24px;height:72px;border-top:1px solid var(--line);background:var(--panel);cursor:pointer}
footer{position:absolute;left:0;right:0;bottom:0;height:24px;padding:4px 16px;color:var(--mute);font-size:11.5px;background:var(--panel);border-top:1px solid var(--line);display:flex;gap:16px}
footer .legend{margin-left:auto;display:flex;gap:12px}
#tip{position:fixed;pointer-events:none;background:#0f1320ee;border:1px solid #3a4260;border-radius:8px;padding:8px 10px;min-width:180px;max-width:300px;display:none;box-shadow:0 6px 24px #0008;font-size:12.5px}
#tip h3{margin:0 0 4px;font-size:13px}#tip .row{display:flex;justify-content:space-between;gap:12px;color:var(--mute)}#tip .row span:last-child{color:var(--text)}
button{background:#1d2335;color:var(--text);border:1px solid var(--line);border-radius:6px;padding:3px 10px;cursor:pointer}button:hover{border-color:#3a4260}
</style></head><body>
<header>
  <h1><b>&#9670; Phantom Click</b> &middot; <span id="lvl"></span></h1>
  <div class="stats" id="stats"></div>
  <div class="toggles" id="toggles"></div>
  <button id="fit" title="Back to the worst range (F)">Reset view</button>
</header>
<div id="stage"><canvas id="cv"></canvas></div>
<canvas id="strip"></canvas>
<footer><span>Drag to move &middot; wheel to zoom &middot; click the strip to jump &middot; arrow keys pan</span>
<span class="legend">
<span><i class="sw" style="background:var(--on)"></i> buffer ON</span><span><i class="sw" style="background:var(--learn)"></i> learning</span>
<span><i class="sw" style="background:var(--dead)"></i> deaths only</span><span><i class="sw" style="background:var(--delay)"></i> delay</span>
<span><i class="sw" style="background:var(--off)"></i> off</span><span><i class="sw" style="background:var(--accent)"></i> caught click</span></span></footer>
<div id="tip"></div>
<script>
const D = __DATA__;
const COL = {on:'#3dff7a', learning:'#ffbf33', deaths:'#ff4d4d', delay:'#4d8cff', off:'#9aa0ad'};
const STATUS = {on:'buffer ON', learning:'learning - pass it once', deaths:'no click to blame yet', delay:'delay: every click held', off:'buffer off'};
const $ = s => document.querySelector(s);
const cv = $('#cv'), ctx = cv.getContext('2d'), strip = $('#strip'), sctx = strip.getContext('2d'), tip = $('#tip');

// percent <-> x
const hasFit = D.a != null;
const pctAt = x => hasFit ? D.a * x + D.b : null;
const xAt = p => hasFit ? (p - D.b) / D.a : null;
for (const r of D.ranges) {
  if (hasFit) { r.x0 = xAt(r.lo); r.x1 = xAt(r.hi); }
  else {
    const xs = D.deaths.filter(d => d[2] >= r.lo && d[2] < r.hi).map(d => d[0]);
    if (xs.length) { r.x0 = Math.min(...xs) - 15; r.x1 = Math.max(...xs) + 15; }
  }
}

// header
$('#lvl').textContent = D.name;
const best = D.best ? D.best.endPct : null;
const onN = D.ranges.filter(r => r.status === 'on' || r.status === 'delay').length;
$('#stats').innerHTML = `<span><strong>${D.deaths.length}</strong> deaths</span><span><strong>${D.ranges.length}</strong> ranges</span>` +
  `<span><strong style="color:var(--on)">${onN}</strong> buffered</span>` + (best != null ? `<span>best <strong>${best.toFixed(2)}%</strong></span>` : '');
const layers = {ranges:['Ranges',1], deaths:['Deaths',1], best:['Best run',1], recent:['Recent attempts',1], clicks:['Clicks',1], ticks:['Right ticks',1]};
const show = {};
for (const [k,[label,on]] of Object.entries(layers)) {
  show[k] = !!on;
  const l = document.createElement('label');
  l.innerHTML = `<input type="checkbox" ${on?'checked':''}> ${label}`;
  l.querySelector('input').onchange = e => { show[k] = e.target.checked; draw(); };
  $('#toggles').appendChild(l);
}
const fitTop = () => document.documentElement.style.setProperty('--top', document.querySelector('header').offsetHeight + 'px');

// world bounds
const allPts = [];
const addAttempt = a => { if (!a) return; for (const s of a.segments) for (let i=0;i<s.length;i+=2) allPts.push([s[i], s[i+1]]); };
addAttempt(D.best); D.recent.forEach(addAttempt);
for (const d of D.deaths) allPts.push([d[0], d[1]]);
const minX = Math.min(0, ...allPts.map(p => p[0])), maxX = Math.max(100, ...allPts.map(p => p[0]));
const ys = allPts.map(p => p[1]).sort((a,b) => a-b);
const midY = ys.length ? ys[Math.floor(ys.length/2)] : 200;
const levelEndX = hasFit ? Math.max(maxX, xAt(100)) : maxX;

// camera: x/y = world point at screen centre, s = pixels per unit
const cam = {x:0, y:midY + 60, s:1};
function resetView() {
  const H = cv.clientHeight;
  cam.s = Math.max(0.35, H / 520);
  const worst = D.ranges.slice().sort((a,b) => b.deaths - a.deaths)[0];
  cam.x = worst && worst.x0 != null ? (worst.x0 + worst.x1) / 2 : (D.best ? D.best.segments[0][0] + cv.clientWidth / cam.s / 2 : 0);
  cam.y = worst && worst.y != null ? worst.y + 40 : midY + 60;
  draw();
}
const sx = x => (x - cam.x) * cam.s + cv.clientWidth / 2;
const sy = y => cv.clientHeight / 2 - (y - cam.y) * cam.s;
const wx = px => (px - cv.clientWidth / 2) / cam.s + cam.x;
const wy = py => cam.y - (py - cv.clientHeight / 2) / cam.s;

function sizeCanvas(c) {
  const r = devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
  if (c.width !== Math.round(w*r) || c.height !== Math.round(h*r)) { c.width = Math.round(w*r); c.height = Math.round(h*r); }
  c.getContext('2d').setTransform(r,0,0,r,0,0);
}

function polyline(segs, color, width, alpha) {
  ctx.strokeStyle = color; ctx.lineWidth = width; ctx.globalAlpha = alpha; ctx.lineJoin = 'round'; ctx.lineCap = 'round';
  const left = wx(-50), right = wx(cv.clientWidth + 50);
  for (const s of segs) {
    ctx.beginPath(); let started = false;
    for (let i = 0; i + 1 < s.length; i += 2) {
      const x = s[i], y = s[i+1];
      if (x < left && i + 2 < s.length && s[i+2] < left) continue;
      if (x > right && started) { ctx.lineTo(sx(x), sy(y)); break; }
      if (!started) { ctx.moveTo(sx(x), sy(y)); started = true; } else ctx.lineTo(sx(x), sy(y));
    }
    ctx.stroke();
  }
  ctx.globalAlpha = 1;
}

function niceStep(span, target) {
  const raw = span / target, p = Math.pow(10, Math.floor(Math.log10(raw)));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= raw) return m * p;
  return 10 * p;
}

function draw() {
  sizeCanvas(cv);
  const W = cv.clientWidth, H = cv.clientHeight;
  ctx.clearRect(0,0,W,H);

  // block grid
  if (cam.s > 0.6) {
    ctx.strokeStyle = '#ffffff'; ctx.globalAlpha = Math.min(0.06, (cam.s - 0.6) * 0.06); ctx.lineWidth = 1;
    ctx.beginPath();
    for (let x = Math.floor(wx(0)/30)*30; x < wx(W); x += 30) { ctx.moveTo(sx(x)+.5, 0); ctx.lineTo(sx(x)+.5, H); }
    for (let y = Math.floor(wy(H)/30)*30; y < wy(0); y += 30) { ctx.moveTo(0, sy(y)+.5); ctx.lineTo(W, sy(y)+.5); }
    ctx.stroke(); ctx.globalAlpha = 1;
  }

  // ranges
  if (show.ranges) for (const r of D.ranges) {
    if (r.x0 == null) continue;
    const a = sx(r.x0), b = sx(r.x1);
    if (b < 0 || a > W) continue;
    const col = COL[r.status];
    ctx.fillStyle = col; ctx.globalAlpha = Math.min(0.06 + 0.012 * r.deaths, 0.22); ctx.fillRect(a, 0, Math.max(1, b - a), H);
    ctx.globalAlpha = 0.5; ctx.fillRect(a, 0, 1, H); ctx.fillRect(b - 1, 0, 1, H);
    ctx.globalAlpha = 1;
    // labels only where they fit inside the band
    const head = `${+r.center.toFixed(2)}%`, sub = `x${r.deaths} · ${r.status === 'delay' ? 'delay ' + r.eff + 't' : r.eff > 0 ? r.eff + 't' : 'off'}`;
    ctx.textAlign = 'center';
    ctx.font = '600 12px system-ui,Segoe UI,sans-serif';
    if (ctx.measureText(head).width + 6 < b - a) {
      ctx.fillStyle = col; ctx.fillText(head, (a+b)/2, 18);
      ctx.font = '11px system-ui,Segoe UI,sans-serif';
      if (ctx.measureText(sub).width + 6 < b - a) { ctx.fillStyle = '#e6e9f2'; ctx.fillText(sub, (a+b)/2, 33); }
    }
  }

  // trails
  if (show.recent) D.recent.forEach((t, i) => { if (t !== D.best) polyline(t.segments, '#ffffff', 1.2, 0.32 - i * 0.04); });
  if (show.best && D.best) polyline(D.best.segments, '#3dff7a', 2.2, 0.85);

  // clicks on the best run
  if (show.clicks && D.best) for (const [x, y, buf] of D.best.clicks) {
    const px = sx(x), py = sy(y);
    if (px < -10 || px > W + 10) continue;
    ctx.beginPath(); ctx.arc(px, py, buf ? 5 : 3.2, 0, 7);
    ctx.fillStyle = buf ? '#c77dff' : '#5ad8ff'; ctx.fill();
    if (buf) { ctx.strokeStyle = '#fff'; ctx.lineWidth = 1; ctx.stroke(); }
  }

  // learned right ticks: posts
  if (show.ticks) for (const s of D.spots) {
    if (s.right == null) continue;
    const px = sx(s.right);
    if (px < -10 || px > W + 10) continue;
    const y = trailY(s.right);
    ctx.strokeStyle = s.ready ? '#3dff7a' : '#ffbf33'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.moveTo(px, sy(y) + 26); ctx.lineTo(px, sy(y) - 26); ctx.stroke();
    ctx.beginPath(); ctx.moveTo(px, sy(y) - 26); ctx.lineTo(px + 10, sy(y) - 21); ctx.lineTo(px, sy(y) - 16); ctx.fillStyle = ctx.strokeStyle; ctx.fill();
    // misses as ticks before/after it
    if (s.passes.length && cam.s > 0.8) for (const m of s.misses) {
      ctx.fillStyle = '#ff4d4d'; ctx.globalAlpha = .7; ctx.fillRect(sx(m) - 1, sy(y) + 28, 2, 7); ctx.globalAlpha = 1;
    }
  }

  // deaths
  if (show.deaths) {
    ctx.lineWidth = 1.6; ctx.strokeStyle = '#ff4d4d';
    for (const [x, y, , practice] of D.deaths) {
      const px = sx(x), py = sy(y);
      if (px < -8 || px > W + 8) continue;
      const r = 4.5; ctx.globalAlpha = practice ? 0.45 : 0.85;
      ctx.beginPath(); ctx.moveTo(px-r, py-r); ctx.lineTo(px+r, py+r); ctx.moveTo(px-r, py+r); ctx.lineTo(px+r, py-r); ctx.stroke();
    }
    ctx.globalAlpha = 1;
  }

  // percent ruler
  if (hasFit) {
    const p0 = pctAt(wx(0)), p1 = pctAt(wx(W)), step = niceStep(p1 - p0, Math.max(3, W / 110));
    ctx.fillStyle = '#0b0d14cc'; ctx.fillRect(0, H - 22, W, 22);
    ctx.font = '11px system-ui,Segoe UI,sans-serif'; ctx.textAlign = 'center';
    for (let p = Math.max(0, Math.ceil(p0 / step) * step); p <= Math.min(100, p1); p += step) {
      const px = sx(xAt(p));
      ctx.fillStyle = '#8a93ad'; ctx.fillRect(px, H - 22, 1, 5);
      ctx.fillText(`${+p.toFixed(2)}%`, px, H - 7);
    }
  }
  drawStrip();
}

function trailY(x) {
  const src = D.best || D.recent[0];
  if (src) for (const s of src.segments) for (let i = 2; i + 1 < s.length; i += 2) {
    if (s[i-2] <= x && x <= s[i]) { const t = s[i] > s[i-2] ? (x - s[i-2]) / (s[i] - s[i-2]) : 0; return s[i-1] + (s[i+1] - s[i-1]) * t; }
  }
  return midY;
}

// overview strip: deaths per range over the whole level
const stripPct = px => px / strip.clientWidth * 100;
function drawStrip() {
  sizeCanvas(strip);
  const W = strip.clientWidth, H = strip.clientHeight;
  sctx.clearRect(0,0,W,H);
  const top = Math.max(1, ...D.ranges.map(r => r.deaths));
  for (const r of D.ranges) {
    const a = r.lo / 100 * W, b = r.hi / 100 * W, h = (H - 18) * r.deaths / top;
    sctx.fillStyle = COL[r.status]; sctx.globalAlpha = .85;
    sctx.fillRect(a, H - 14 - h, Math.max(1.5, b - a - 1), h);
  }
  sctx.globalAlpha = 1;
  if (D.best) { sctx.fillStyle = '#3dff7a'; sctx.fillRect(0, H - 4, best / 100 * W, 2); }
  sctx.fillStyle = '#8a93ad'; sctx.font = '10px system-ui,Segoe UI,sans-serif';
  for (let p = 0; p <= 100; p += 10) { sctx.textAlign = p === 0 ? 'left' : p === 100 ? 'right' : 'center'; sctx.fillText(p + '%', p / 100 * W, H - 4 + (D.best ? -4 : 0)); }
  if (hasFit) { // viewport
    const a = pctAt(wx(0)) / 100 * W, b = pctAt(wx(cv.clientWidth)) / 100 * W;
    sctx.strokeStyle = '#c77dff'; sctx.lineWidth = 1.5; sctx.strokeRect(a, 1, Math.max(3, b - a), H - 16);
  }
}

// interaction
let drag = null;
cv.addEventListener('mousedown', e => { drag = {x:e.clientX, y:e.clientY, cx:cam.x, cy:cam.y}; cv.style.cursor = 'grabbing'; });
addEventListener('mouseup', () => { drag = null; cv.style.cursor = ''; });
addEventListener('mousemove', e => {
  if (drag) { cam.x = drag.cx - (e.clientX - drag.x) / cam.s; cam.y = drag.cy + (e.clientY - drag.y) / cam.s; draw(); }
  hover(e);
});
cv.addEventListener('wheel', e => {
  e.preventDefault();
  const r = cv.getBoundingClientRect(), mx = e.clientX - r.left, my = e.clientY - r.top;
  const bx = wx(mx), by = wy(my);
  cam.s = Math.min(12, Math.max(0.02, cam.s * Math.exp(-e.deltaY * 0.0015)));
  cam.x = bx - (mx - cv.clientWidth / 2) / cam.s; cam.y = by + (my - cv.clientHeight / 2) / cam.s;
  draw();
}, {passive:false});
function jumpTo(e) { if (!hasFit) return; const r = strip.getBoundingClientRect(); cam.x = xAt(stripPct(e.clientX - r.left)); draw(); }
strip.addEventListener('mousedown', e => { jumpTo(e); const mv = ev => jumpTo(ev); addEventListener('mousemove', mv); addEventListener('mouseup', () => removeEventListener('mousemove', mv), {once:true}); });
addEventListener('keydown', e => {
  const k = e.key, step = cv.clientWidth / cam.s / 6;
  if (k === 'ArrowRight') cam.x += step; else if (k === 'ArrowLeft') cam.x -= step;
  else if (k === 'ArrowUp') cam.y += step / 2; else if (k === 'ArrowDown') cam.y -= step / 2;
  else if (k === '+' || k === '=') cam.s *= 1.25; else if (k === '-') cam.s /= 1.25;
  else if (k === 'f' || k === 'F') return resetView(); else return;
  draw();
});
$('#fit').onclick = resetView;
addEventListener('resize', () => { fitTop(); draw(); });

function hover(e) {
  const r = cv.getBoundingClientRect(), mx = e.clientX - r.left, my = e.clientY - r.top;
  if (mx < 0 || my < 0 || mx > r.width || my > r.height || drag) { tip.style.display = 'none'; return; }
  const x = wx(mx), y = wy(my);
  let html = '';
  const near = show.deaths && D.deaths.find(d => Math.abs(sx(d[0]) - mx) < 6 && Math.abs(sy(d[1]) - my) < 6);
  if (near) html += `<h3 style="color:var(--dead)">Death at ${near[2].toFixed(2)}%</h3>${near[3] ? '<div class="row"><span>practice</span><span>yes</span></div>' : ''}`;
  const rg = show.ranges && D.ranges.find(q => q.x0 != null && x >= q.x0 && x < q.x1);
  if (rg) {
    const sp = D.spots.filter(s => s.range === rg.idx);
    html += `<h3 style="color:${COL[rg.status]}">${rg.label}</h3>` +
      row('Deaths', rg.deaths + (rg.practice ? ` (${rg.practice} practice)` : '')) +
      row('Buffer', rg.status === 'delay' ? `delay, ${rg.eff} tick(s)` : rg.eff > 0 ? `${rg.eff} tick(s)${rg.custom ? ' (yours)' : ''}` : 'off') +
      row('Status', STATUS[rg.status]) +
      row('Clicks learned', `${rg.learned}/${rg.clicks}`);
    for (const s of sp.slice(0, 3)) html += row(`click ${s.pct.toFixed(2)}%`, `${s.missN} miss / ${s.passN} pass` + (s.caught ? ` · ${s.caught} caught` : ''));
    if (rg.pattern) html += `<div style="margin-top:4px;color:var(--learn)">${rg.pattern}</div>`;
    html += `<div style="margin-top:6px;color:var(--mute)">Change it: python phantom.py set "${D.name}" ${+rg.center.toFixed(2)} &lt;ticks&gt;</div>`;
  } else if (hasFit) html += `<div class="row"><span>here</span><span>${pctAt(x).toFixed(2)}%</span></div>`;
  if (!html) { tip.style.display = 'none'; return; }
  tip.innerHTML = html; tip.style.display = 'block';
  const tw = tip.offsetWidth, th = tip.offsetHeight;
  tip.style.left = Math.min(e.clientX + 14, innerWidth - tw - 8) + 'px';
  tip.style.top = Math.min(e.clientY + 14, innerHeight - th - 8) + 'px';
}
const row = (a, b) => `<div class="row"><span>${a}</span><span>${b}</span></div>`;

fitTop(); requestAnimationFrame(() => { fitTop(); resetView(); });
</script></body></html>
"""


# --------------------------------------------------------------- interactive --
BANNER = r"""
    ____  __                __                     _________      __  
   / __ \/ /_  ____ _____  / /_____  ____ ___     / ____/ (_)____/ /__
  / /_/ / __ \/ __ `/ __ \/ __/ __ \/ __ `__ \   / /   / / / ___/ //_/
 / ____/ / / / /_/ / / / / /_/ /_/ / / / / / /  / /___/ / / /__/ ,<   
/_/   /_/ /_/\__,_/_/ /_/\__/\____/_/ /_/ /_/   \____/_/_/\___/_/|_|
"""


def interactive(store):
    print(c(BANNER, "95"))
    print(dim("  the console - edit death ranges, open level maps.  Ctrl+C to quit."))
    while True:
        store.reload()  # re-read: you may have played since
        print_levels(store)
        try:
            pick = input(bold("Level number or name (q to quit): ")).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if pick.lower() in ("q", "quit", "exit"):
            return
        if not pick:
            continue
        try:
            key, lv = store.find(pick)
        except SystemExit as e:
            print(c(str(e), "91"))
            continue
        level_menu(store, key, lv)


LEVEL_HELP = f"""{bold('Commands')}
  {c('51 3', '96')}          3 ticks of buffer in the 51% range (50.5-51.5%)
  {c('51 2 delay', '96')}    hold EVERY click there exactly 2 ticks
  {c('51 off', '96')}        no buffer in that range
  {c('51 reset', '96')}      back to the default
  {c('all 3', '96')}         same setting for every range on this level ({c('all reset', '96')} to undo)
  {c('map', '96')}           open the level map in your browser
  {c('b', '96')}             back to levels"""


def level_menu(store, key, lv):
    while True:
        store.reload()
        lv = store.db.get(key, lv)
        print_level(store, key, lv)
        print(LEVEL_HELP)
        try:
            cmd = input(bold(f"{lv.get('name') or key} > ")).strip()
        except (EOFError, KeyboardInterrupt):
            print()
            return
        if not cmd:
            continue
        parts = cmd.split()
        word = parts[0].lower()
        try:
            if word in ("b", "back", "q"):
                return
            if word == "map":
                out = write_map(store, key, lv)
                print(c(f"Map opened: {out}", "92"))
            elif word == "all" and len(parts) >= 2:
                width, ranges = analyse(store, key, lv)
                if parts[1].lower() in ("reset", "default", "clear"):
                    store.clear_rule(key)
                    print(c("Every range is back to the default.", "92"))
                else:
                    ticks = parse_ticks(parts[1])
                    mode = parts[2].lower() if len(parts) > 2 else "smart"
                    for r in ranges:
                        store.set_rule(key, r["idx"], width, ticks, mode)
                    print(c(f"{len(ranges)} range(s) set to {ticks} tick(s) {mode}.", "92"))
            elif len(parts) >= 2:
                mode = parts[2].lower() if len(parts) > 2 else None
                print(c(apply_set(store, key, lv, parts[0], parts[1], mode), "92"))
            else:
                print(c('Try "51 3", "51 off", "map" or "b".', "93"))
        except ValueError as e:
            print(c(str(e), "91"))
        input(dim("(enter to continue)"))


# ------------------------------------------------------------------- main --
def main():
    ap = argparse.ArgumentParser(description="Phantom Click console", add_help=True)
    ap.add_argument("--dir", type=Path, default=default_dir(), help="Phantom Click save folder")
    ap.add_argument("--no-color", action="store_true", help="plain text output")
    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("levels", help="list levels")
    p = sub.add_parser("show", help="death ranges for a level")
    p.add_argument("level")
    p.add_argument("--all", action="store_true", help="show every range")
    p = sub.add_parser("set", help="set a range's ticks")
    p.add_argument("level")
    p.add_argument("pct", help="any percent inside the range, e.g. 51")
    p.add_argument("ticks", help="0-20, off, or reset")
    p.add_argument("mode", nargs="?", choices=["smart", "delay"], default=None)
    p = sub.add_parser("clear", help="reset a range (or all) to the default")
    p.add_argument("level")
    p.add_argument("pct", help='a percent, or "all"')
    p = sub.add_parser("map", help="open the level map in your browser")
    p.add_argument("level")
    p.add_argument("--out", type=Path, help="where to save the page")
    p.add_argument("--no-open", action="store_true", help="just write the file")
    args = ap.parse_args()

    store = Store(args.dir)
    try:
        if args.cmd is None:
            interactive(store)
        elif args.cmd == "levels":
            print_levels(store)
        elif args.cmd == "show":
            key, lv = store.find(args.level)
            print_level(store, key, lv, limit=10_000 if args.all else 25)
        elif args.cmd == "set":
            key, lv = store.find(args.level)
            print(c(apply_set(store, key, lv, args.pct, args.ticks, args.mode), "92"))
        elif args.cmd == "clear":
            key, lv = store.find(args.level)
            if args.pct.lower() == "all":
                store.clear_rule(key)
                print(c("Every range is back to the default.", "92"))
            else:
                print(c(apply_set(store, key, lv, args.pct, "reset"), "92"))
        elif args.cmd == "map":
            key, lv = store.find(args.level)
            out = write_map(store, key, lv, args.out, not args.no_open)
            print(c(f"Map {'opened' if not args.no_open else 'written'}: {out}", "92"))
    except ValueError as e:
        sys.exit(c(str(e), "91"))


if __name__ == "__main__":
    main()
