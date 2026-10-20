# Death Buffer (Geode mod, Geometry Dash 2.2081)

Logs your deaths on every level (exact %) and turns a 1-tick window into a 3-tick window,
but only at the spots you've died. Everywhere else your clicks are untouched.

## How it works

1. **You die** → the death is saved with the exact percent and x position. Deaths within
   1 block of each other are grouped into one *spot*.
2. **You get past a spot alive** (normal or practice) → the last click before that spot is
   saved as the spot's *right tick*.
3. **Next attempts** → a click 1–2 ticks *before* the right tick is held back and fires on
   the right tick. On-time and late clicks go through as normal. Late clicks can't be fixed
   because the mod can't jump earlier than you click.

The buffer can only learn a spot after you've passed it once. Practice mode is the quickest
way to teach it.

## Build it (no compiler needed)

1. Create a new GitHub repository and upload everything in this folder.
2. On GitHub, click **Add file → Create new file**, name it `.github/workflows/build.yml`,
   paste in the contents of `workflow-build.yml` from this folder, and commit.
   (GitHub's uploader skips files whose names start with a dot, so this one is made on GitHub.)
3. Open the **Actions** tab. The *Build Geode Mod* workflow runs automatically (about 5–10 min).
4. Open the finished run and download the **Death Buffer** artifact. Unzip it to get
   `local.death-buffer.geode`.

Building locally instead: install the Geode SDK and CLI, then run `geode build` in this folder.

## Install

1. Geometry Dash **2.2081** with **Geode 5.11.2 or newer (5.x)** installed.
2. Copy `local.death-buffer.geode` into
   `C:\Program Files (x86)\Steam\steamapps\common\Geometry Dash\geode\mods\`
3. Start the game. Settings: Geode → Mods → Death Buffer → ⚙.

## Watch it live (console)

In Geode's own settings (Geode → Mods → Geode → ⚙), turn on **Show Platform Console**. A
console window opens next to the game with lines like:

```
Tracking "Bloodbath": 212 death(s) logged, 31 death spot(s), 9 with the buffer ready
[buffer] held a click 2 tick(s) at 47.31% so it fired on the right tick
[death] 63.12%  (spot ~63.10%: 14 death(s), buffer learns this spot once you get past it)
[learned] spot 63.10%: right click is at x=9481.6 - buffer is ON here now
```

Nothing is printed or saved while you're alive mid-attempt. Lines are held until you die or
leave, so logging never causes a stutter. Leaving a level prints a summary of your worst spots.

## The data

Saved to `%LOCALAPPDATA%\GeometryDash\geode\mods\local.death-buffer\deaths.json`. To see it in CMD:

```
python tools\deaths_report.py              all levels
python tools\deaths_report.py bloodbath    one level
```

## Settings

| Setting | Default | What it does |
|---|---|---|
| Buffer early clicks | on | Turns the buffer off without stopping death tracking |
| Ticks early allowed | 2 | 2 = a 3-tick window |
| Deaths before buffering | 1 | Deaths needed at a spot before the buffer turns on there |
| Same-spot distance | 1 block | Deaths this close count as one spot |
| Click lookback | 4 blocks | How far before a spot to look for the click that got you past |
| Count practice deaths | on | Log practice-mode deaths too |
| Console log | on | Print deaths, learned spots, buffered clicks and summaries |

## Notes

- Platformer levels are skipped.
- Player 1's jump only. In 2-player mode, player 2's input is left alone.
- If you use Click Between Frames, test with it on and off. Both mods touch input timing.
- Buffered runs are input-assisted and don't count for leaderboards or list submissions.
