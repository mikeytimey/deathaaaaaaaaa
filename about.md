# Death Buffer

Tracks every death on every level (exact percent and position) and buffers early clicks **only at the spots you keep dying**.

1. Die at a spot. It's logged.
2. Get past that spot once (practice mode works). The click that got you past it becomes the "right tick".
3. From then on, a click up to 2 ticks early at that spot is held and fires on the right tick. A 1-tick window becomes 3 ticks.

Every other click is left alone. Data is saved to `deaths.json` in the mod's save folder. Turn on Geode's console to watch deaths and buffered clicks as they happen.

Runs made with this mod are input-assisted and don't count for leaderboards or list submissions.
