# Changelog

## v0.2.0
- Demons are harder, closer to real ones (measured against a Bloodbath run logged in GD): normal
  speed with the hazards packed close, spikes along the whole path (on the ground, on faces and on
  short pillars grown from the structures), and a spike for every click a badly timed press would
  still get through
- Nothing hangs from the sky any more: ground parts without a ceiling put only one connected band
  over the player, and every style keeps shapes from hanging from nothing
- Some stretches join their shapes into one big structure (the gaps between them are filled)
- Nothing is built behind the start or after the end, and the path no longer climbs far off the
  top of the screen (a flipped cube or spider with nothing above it)
- Classic structures learned from about 680 levels (was about 70): the most liked rated Easy to
  Insane levels join RobTop's levels, with wider spacing as in those levels
- Orbs, pads and gravity portals by rating: Easy only yellow orbs and pads, Normal adds pink and
  blue, Hard red, Harder green and the dash orb, Insane black
- Easy and Normal ground parts have at most two spikes in a row, Hard and Harder three
- Path platforms that overlap, and shapes that touch, are drawn as one outline: no edges crossing
  inside a shape and no double lines between neighbours; plain blocks take the layout's look
- Structures keep out of the path's platforms
- The bent thorn spikes 178 and 179 become the plain half and small spike
- Classic cube parts keep the sky open: everything stands on the ground, nothing hangs over the
  player unless there is a ceiling
- Pillars that cannot reach the floor or the bound are closed at the end, and lone edge lines
  left over from a cut-off pillar are dropped
- Difficulty matches real ratings: Auto picks Harder to Insane (it used to land near Easy demon),
  Insane clicks about twice a second at its busiest (Extreme about five times), with wider timing
  windows and fewer gamemode changes below demon ratings
- Wave below demon ratings: no mini, the full timing window, corridors about a block and a half
  to each wall, turns no faster than about four a second, and the path keeps its walls clear of
  the floor and ceiling so they are never cut off
- Classic structure style, learned from RobTop's levels and 31 rated Harder and Insane levels: one
  block set, plain shapes, the usual look below demon ratings (Auto picks it there). No style
  keeps a source shape whose blocks lie over each other any more
- A layout that fails its final check loses only the hazards the run dies on, after a lighter
  retry, instead of all its structures
- One-page menu: song, clicks (music, tap-along or a recording), difficulty, speed and structure
  style, then Generate, Place in level, New layout or Remove. The old tabs (style, gamemodes,
  sections, playtest, training, settings), the ghost, variants, seed and re-roll are gone, and
  options an older version saved are no longer read
- Hazards never overlap each other or sink into blocks, and pillars reach the floor or the next
  face instead of ending open in the air
- Wave corridors are closed at both ends; a stretch with a long wait for a click is not a wave
  (it would slide along the floor)
- More cube and ship, as in real layouts; flying paths keep off the edges of the screen
- Structure library learned from 35 levels (was 18)
- Structures come from real levels now: a trained library of modules from Bidal Wave and shapes
  shared between top levels, with Rail, Slope, Compact and Spiky styles
- Each gamemode stretch repeats a few shapes in a rhythm, hugging the player's path
- Wave parts are an outline on both sides that follows every stroke; tight corridors set by the
  difficulty, steep mini wave on fast parts
- Every stretch has a connected band above and below the path with hazards lining its faces, so
  structures read as a corridor (about twice the objects per screen)
- Fixed: hard mini wave parts lost almost all their walls
- Fixed: wave walls had holes around portals, and spikes piled up on each other and on saws
- Spikes all stand straight; floors, ceilings and the sides of steps are lined with them too
- Fixed: paths skimming the floor upside down, where the smallest timing error killed
- Fixed: a portal right after another portal could sit at the edge of the path and be missed
- Gamemode stretches last a few seconds instead of 1-3 clicks
- New menu rows: Structures, Speed (slow / normal / fast); outline look by default
- Physics fixed against runs logged in GD: a blue pad facing the other way stays armed, the spider's
  area is 270 units high, a pad gives the robot no held boost, yellow, pink and blue pads are 25
  wide, a landing counts before a mode portal, a flipped cube dies a tenth of a unit into the floor
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
