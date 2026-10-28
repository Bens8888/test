# Roadmap

Ordered by how much each item is expected to improve real layouts.

1. **First in-game round.** Play-test the menu, apply flow, recorder and ghost in GD 2.2081 on
   Windows and macOS, then fix what real use shows. Run `alcli validate` on recorded runs of
   official levels and publish the error table in docs/VALIDATION.md.
2. **Style learning from human levels.** Collect the 60-level dataset, retrain (training/RETRAINING.md),
   and compare against the self-play models on held-out songs and by blind play-testing.
3. **Reward from people.** Add a thumbs up/down button on applied layouts and use the votes as a
   learned reward on top of the bot-verified one.
4. **Wider vocabulary.** Dash orbs, blue/green/red orbs and pads, teleport portals, simple
   move/rotate gameplay, each with simulator support and tests first.
5. **Decoration model.** A second small model for deco placement (glow, outlines, background
   objects) trained on the same dataset.
6. **Re-roll by dragging.** Select a range on the timeline to re-roll exactly that stretch.
7. **Platformer layouts** (a separate generator: no forced auto-scroll, different fairness rules).
