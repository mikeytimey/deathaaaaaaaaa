# ◆ Phantom Click

Learns the clicks you die to. Click a few ticks early there, and the mod **catches the click**: GD only gets it on the **right tick**.

- **Death ranges:** every death counts toward a 1% range, so 51% means 50.5-51.5%.
- **Per-range ticks:** set each range from the console tool (`python phantom.py`). Changes apply on your next attempt.
- **Smart, delay or off:** smart holds early clicks until the learned right tick. Delay holds every click in a range a fixed number of ticks. Off disables it.
- **Overlay:** range bands, deaths, learned right ticks and your best and last runs, drawn inside the level.
- **Level map:** pan and zoom your trajectories in the browser with `python phantom.py map <level>`.

Every death is logged with its exact percent. On-time clicks, late clicks, and clicks away from your death spots are never touched.

Runs made with this mod are input-assisted and don't count for leaderboards or list submissions.
