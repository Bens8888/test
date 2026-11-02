# Changelog

## v0.2.0
- Structures come from real levels now: a trained library of modules from Bidal Wave and shapes
  shared between top levels, with Rail, Slope, Compact and Spiky styles
- Each gamemode stretch repeats a few shapes in a rhythm, hugging the player's path
- Wave parts are an outline on both sides that follows every stroke; tight corridors set by the
  difficulty, steep mini wave on fast parts
- Every stretch has a connected band above and below the path with hazards lining its faces, so
  structures read as a corridor (about twice the objects per screen)
- Fixed: hard mini wave parts lost almost all their walls
- Gamemode stretches last a few seconds instead of 1-3 clicks
- New menu rows: Structures, Speed (slow / normal / fast); outline look by default
- Physics fixed against runs logged in GD: a blue pad facing the other way stays armed, the spider's
  area is 270 units high, a pad gives the robot no held boost, yellow, pink and blue pads are 25
  wide
- Recorder: ticks were doubled (the game counts two per physics step)
- In-game self test: selftest.txt in the save folder plays a level with its designed clicks and logs
  every step

## v0.1.0
- Editor button and the AutoLayout popup.
- Test pattern placement (every gamemode portal, blocks, spikes, orbs, pads) as one undo step.
- Tap along (like GD's Guideline Creator): tap to the song, taps shown as guidelines, takes merged, Clear, Undo take, Start Offset.
- Recordings: the level ends after the last click; applying a layout sets the song offset it was built for; playtests started mid-level are synced to the song's real position; strict faithfulness is the default.
- A take that starts late in the song makes a level that starts shortly before the first click (song offset), not after a long empty run.
- Gameplay modelled on reference layouts: block look by default (platforms too), floating block groups (hanging blocks with spikes, slabs, T shapes, steps, columns), spikes in groups of 1-3 facing the path, denser framing in flying stretches.
- Harder and tighter, like real layouts: walls that follow the path (hanging and rising pillars with spike tips), timing windows per difficulty about 25% tighter, automatic difficulty around Insane to Easy Demon.
- Wave rebuilt from presets like wave layouts: triangles under every peak and over every dip, spiked slabs for small zig-zags, nothing random inside; every music click turns the wave (press, then the next click releases) for long even strokes; wave prefers slower rhythms and 2x speed.
