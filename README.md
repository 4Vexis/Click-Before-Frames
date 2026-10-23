# Click Before Frames

Geode mod for Geometry Dash 2.2081 on Android, by vVexis.

Click a bit too early and the mod holds your click until it's safe to fire. If your click was fine, it doesn't touch it.

Made for practice and fun. Don't use it on leaderboard or submitted runs.

## How it works

- **Cube, ball, robot, spider:** an early click in the air gets simulated ahead. If it would kill you but a slightly later click wouldn't, the click waits. Ground clicks are never delayed.
- **Ship, UFO, swing, wave:** very short taps are stretched to a minimum length.
- Dual mode isn't supported yet.

## The V button

Tap the floating **V** to open the menu, drag it to move it. You can pick the mode (OFF, LOW, MED, HIGH), turn the mod on or off per gamemode, check the stats, and open settings. The pause menu has a V button too.

## Install

1. Get the `.geode` file from the latest build in the Actions tab.
2. Put it in `Android/media/com.geode.launcher/game/geode/mods`.
3. Open the game with the Geode launcher.

## Notes

- Early version, so it won't catch every click.
- Portals and triggers right after a click can throw off the prediction.
- Other input mods may clash with it.
