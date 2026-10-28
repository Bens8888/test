# Installing AutoLayout

1. Install Geode for Geometry Dash **2.2081** (https://geode-sdk.org), Windows or macOS.
2. Get `bens8888.autolayout.geode`:
   - from a build you made yourself (`docs/BUILD.md`), or
   - from GitHub: repository **Actions** tab → the latest successful **Build AutoLayout** run →
     artifact **Build Output** (a zip containing the `.geode` file).
3. Put the `.geode` file into GD's `geode/mods` folder (Geode menu → Mods → the folder button
   opens it), or drag it onto the GD window, and restart the game.
4. AutoLayout depends on `geode.node-ids` (≥ 1.22.0); Geode installs it automatically.

Backups, recordings, presets, the training dataset and retrained models live in the mod's save
folder (AutoLayout → Settings → **Save folder** opens it). Geode may delete that folder when you
uninstall the mod and choose to remove its data, so copy anything you want to keep first.
