# Geometry Dash 2.2 (2.2081) level format: implementation reference

How to read and write GD level strings and `.gmd` files, and the object, trigger and color data needed to generate playable, good-looking levels.

## Confidence tags

| Tag | Meaning |
|---|---|
| **[V-REAL]** | Checked against a level string the game itself wrote (the real downloaded levels embedded in gddocs, see section 1.5). |
| **[V-SRC]** | Agreed on by two or more independent sources: gddocs, the gd-info-explorer 2.2 schema, Geode 2.2081 bindings, G.js (a 2.2 trigger generator, active in 2026), gd.py, gd-sim, OSGD reconstructions, HJfod's gmd-api. |
| **[SRC:x]** | One source only. |
| **[UNVERIFIED]** | My inference or memory. Treat it as a default to test in-game. |

### Sources (all local under `/tmp/claude-0/dl/`)

- `gddocs/` (Wyliemaster community docs)
- `flowvix_gd-info-explorer/src/lib/schema.txt` and `objects.csv`. This is the 2.2 property and object-ID database that gddocs links to as the authoritative 2.2 property list. It has about 3,986 object names and a per-class property schema.
- `bindings/bindings/2.2081/GeometryDash.bro` and `bindings/bindings/include/Geode/Enums.hpp`. Members are annotated with `// property N` and `// property kAxx`.
- `g-js-api_G.js/src/` (a JS level/trigger generator targeting 2.2)
- `nekitdev_gd.py/gd/`
- `camila314_pathfinder/gd-sim/` (a physics simulator validated against the game; hitboxes)
- `iliasHDZ_open-source-gd-project/src/` (OSGD reconstructions)
- `HJfod_gmd-api/src/GMD.cpp` (the code GDShare uses to import and export `.gmd`)
- `Spu7Nix_SPWN-language/levelstring/`
- `alpbayrak031-hash_GD-Remake/` (synthetic `.gmd` samples; see the warning in section 1.5)

---

## 1. Encoding

### 1.1 Inner (decoded) level string  [V-REAL]

```
<header>;<object>;<object>;...;<object>;
```

- `;` separates objects. GD writes a trailing `;` after the last object. Readers must tolerate empty segments.
- The **header** (level start object, section 2) is the first segment. Its keys are strings such as `kS38` and `kA2`, written as `key,value,key,value,...`. It has no object ID.
- Every **object** is `key,value,key,value,...`, with integer keys. GD always writes `1` (ID), `2` (x) and `3` (y) first. The parser does not depend on key order.
- Value formats [V-SRC] (gddocs level-string.md, gd-info-explorer schema):

| Type | Format |
|---|---|
| int / bool | Decimal; bool is `0`/`1` |
| float | Decimal with `.`. Write compactly (`0.5`, `1`, not `1.0000` and not exponent notation). GD itself writes `1` rather than `1.0`. |
| int array (groups, key 57) | `.`-separated, e.g. `12.13.200` |
| HSV | `h a s a v a sChecked a vChecked` with literal `a` separators, e.g. `0a1a1a0a0` (section 5.3) |
| string (text object, key 31) | Base64 of the UTF-8 text |

- Header sub-structures use other separators: `kS38` colors use `|` between channels and `_` inside a channel (section 2.2); `kA14` guidelines use `~`.
- `,` and `;` are therefore forbidden inside values. Base64 never produces them.

### 1.2 Compression  [V-SRC]

The `k4` value is `base64url( gzip( utf8(innerLevelString) ) )`.

- **Compression.** The game writes **gzip**: deflate with a gzip header, magic `1f 8b 08`. Game-written strings begin `H4sIAAAAAAAA…` (gddocs, SPWN `"H4sIAAAAAAAAC"`).
  - Header fields (mtime, XFL, OS) are ignored on load. Python's `gzip.compress` output (`H4sIAAAAAAAC_…`, XFL=02, OS=ff) is fine.
  - GD decodes with zlib `inflateInit2(windowBits = 15 + 32)`, which auto-detects gzip or zlib headers. A zlib-wrapped stream (`78 9c`) also decodes; GDShare's `.lvl` uses that. **Write gzip** to be safe.
  - In C++ with zlib: `deflateInit2(&s, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY)` writes gzip. `inflateInit2(&s, 15 + 32)` reads either.
- **Base64.** Use the URL-safe alphabet: `A–Z a–z 0–9 - _` (`-` replaces `+`, `_` replaces `/`).
  - Emit standard `=` padding; Python's `urlsafe_b64encode` does, and GD accepts it.
  - Decoders must tolerate missing padding. Add `=` until `len % 4 == 0`. gd.py drops one trailing character if `len % 4 == 1`.
  - Strip NUL bytes and whitespace before decoding (gd-remake and SPWN do this).
- **Official-level variant** (LevelData.plist only): the first 13 characters (`H4sIAAAAAAAAA`) are removed. Add them back to decode. You never need this for `.gmd` files.
- **Raw `k4`:** gddocs says the game can also load an uncompressed level string "for compatibility". [UNVERIFIED] for 2.2081 import; always write the compressed form.

### 1.3 `.gmd` file structure (GDShare format)  [V-SRC]

A `.gmd` file is one `GJGameLevel` serialized by GD's `DS_Dictionary` (Robtop's plist dialect, short tags). Here is what HJfod's gmd-api (used by GDShare 1.6 for 2.2081) does:

- **Export:** `level->encodeWithCoder(dict); dict->saveRootSubDictToString()`.
- **Import:**
  1. Replace NULs with spaces.
  2. If the file contains `<plist version="1.0">` without `gjver`, patch it to `<plist version="1.0" gjver="2.0">`. If the `<?xml` prolog is missing, prepend it.
  3. `loadRootSubDictFromString` and `stepIntoSubDictWithKey("root")`.
  4. `GJGameLevel::create()->dataLoaded(dict)`.
  5. Force `m_isEditable=true`, `m_levelType=Editor`, and stars, demon, daily and gauntlet to 0.
  6. If `k4` came out empty, read it with `getStringForKey("k4")`. This handles pre-1.9 files.

Canonical layout, all on one line in practice:

```xml
<?xml version="1.0"?><plist version="1.0" gjver="2.0"><dict>
  <k>kCEK</k><i>4</i>
  <k>k2</k><s>Level Name</s>
  <k>k3</k><s>QmFzZTY0IGRlc2NyaXB0aW9u</s>
  <k>k4</k><s>H4sIAAAAAAAA...</s>
  <k>k5</k><s>CreatorName</s>
  <k>k13</k><t />
  <k>k21</k><i>2</i>
  <k>k16</k><i>1</i>
  <k>k45</k><i>467339</i>
  <k>k50</k><i>45</i>
</dict></plist>
```

**DS_Dictionary tags:**

| Tag | Meaning |
|---|---|
| `<k>` | key |
| `<s>` | string |
| `<i>` | integer |
| `<r>` | real |
| `<t />` | true. False is normally **omitted**. |
| `<d>` | nested dict |

Long tag names (`<key>`, `<string>`, `<integer>`, `<real>`, `<true/>`) also appear in some tools. Write the short form. **XML-escape** `&`, `<` and `>` in names and creators.

**Keys for an importable level with a custom song.** k-key meanings come from gddocs level.md and gd.py `api/levels.py`, which agree [V-SRC]. Whether each key is required is [SRC: gmd-api import code].

| Key | Type | Value to write | Required? | Notes |
|---|---|---|---|---|
| `kCEK` | i | `4` | recommended | Encoder class ID: 4 = GJGameLevel. GD writes it; gmd-api import does not need it. |
| `k2` | s | name | **yes** | Level name. The editor limits names to 20 characters [UNVERIFIED limit]; import does not enforce it. |
| `k4` | s | compressed level string | **yes** | Section 1.2. |
| `k3` | s | base64url(description) | optional | GD stores the description base64-encoded. Old GDShare double-encoded it; modern files are single-encoded. |
| `k5` | s | creator name | optional | |
| `k8` | i | official song index | optional | Main-level track index, 0 = Stereo Madness, 1 = Back On Track, and so on. Used when `k45` is 0 or absent. |
| `k45` | i | custom song ID | for custom song | Newgrounds ID, or a 2.2 music-library ID (library IDs are ≥ 10,000,000 [UNVERIFIED]). Nonzero `k45` overrides `k8`. The player must have the song downloaded or download it from the level page. |
| `k13` | t | true | optional | isEditable. Import forces it true anyway. |
| `k21` | i | `2` | optional | levelType: 1 official, 2 local/editor, 3 saved, 4 online. Import forces Editor. |
| `k16` | i | `1` | optional | Level version. |
| `k50` | i | `45` | recommended | Binary version. The samples and gddocs' newer endpoint docs use 45. The exact 2.2081 value is [UNVERIFIED]; it may be 45–47. Do not write ancient values (< 40), which may trigger legacy conversions [UNVERIFIED]. |
| `k48` | i | object count | optional | Recomputed by the game on save [UNVERIFIED]. |
| `k104` | s | `id1,id2` | optional | Song-ID list for song triggers (2.2). |
| `k105` | s | `id1,id2` | optional | SFX-ID list (2.2). |
| `k80` | i | seconds | optional | Seconds spent editing. |
| `k1`, `k6`, `k60` | i | – | omit | Level ID, user ID and account ID. These are 0 for a local level. |
| `kI1`, `kI2`, `kI3` | r | – | optional | Editor camera x, y and zoom on reopen. |

Other options: `.gmd2` is a zip of `level.data` (this same plist) plus `level.meta` JSON (`song-file`, `song-is-custom`) plus the mp3. `.lvl` is the same plist deflated with zlib (`ccDeflateMemory`).

### 1.4 Verified round trip (Python, `python3 -I`)

Script: `/tmp/claude-0/-home-user/47e9f6ba-8c7e-5408-a561-582830e9e0a7/scratchpad/roundtrip.py`. It builds a header with kS38 colors, objects and triggers, gzip+base64url-encodes them, wraps them in the plist, re-parses, and asserts equality.

```
gzip magic: 1f8b08  b64 prefix: H4sIAAAAAAAC_  padded: False
roundtrip OK; header: kS38,1_20_2_20_3_60_11_255_12_255_13_255_4_-1_6_1000_7_1.0_15_1_18_0_8_1|...
zlib-wrapped also decodes: True
```

(Write `7_1`, not `7_1.0`. Format floats compactly.)

### 1.5 Sample files decoded

**A. `alpbayrak031-hash_GD-Remake/levels/01_first_steps.gmd`.** Decoded with `/tmp/claude-0/-home-user/47e9f6ba-8c7e-5408-a561-582830e9e0a7/scratchpad/decode_gmd.py`.

```
kCEK <i> '4'   k2 <s> 'First Steps'   k5 <s> 'gd-remake'   k13 <t>   k21 <i> '2'   k50 <i> '45'
k4 bytes[0:10]: 1f8b08000000000002ff   (gzip, Python gzip module header)
HEADER: kA13,0,kA15,0,kA16,0,kA6,0,kA7,0,kA17,0,kA18,0,kS39,0,kA2,0,kA3,0,kA8,0,kA4,0,kA9,0,kA10,0,kA11,0
OBJ: 1,8,2,315,3,15        <- spike on ground
OBJ: 1,8,2,705,3,15
OBJ: 1,1,2,1485,3,15       <- block on ground row
OBJ: 1,1,2,1575,3,45       <- block stacked one row up
OBJ: 1,140,2,5415,3,2.5    <- pink pad
...
```

> **Warning:** the GD-Remake `.gmd` files were written by that project's own Python writer (`gd/levelio.py: gmd_document`), not by GD. They show the GDShare container layout but are **not proof** of what the game accepts. They also have **no kS38** and rely on the game's default colors.

**B. Real game-written strings, embedded in gddocs.** These are `endpoints/levels/downloadGJLevel22.md` (level 128) and `uploadGJLevel21.md`. They were decoded with `/tmp/claude-0/-home-user/47e9f6ba-8c7e-5408-a561-582830e9e0a7/scratchpad/dec_str.py` and `/tmp/claude-0/-home-user/47e9f6ba-8c7e-5408-a561-582830e9e0a7/scratchpad/analyze.py`. They date from 2.0/2.1, so the header lacks the 2.2-only keys.

```
kS38,1_40_2_125_3_255_11_255_12_255_13_255_6_1000_7_1_15_1_18_0_8_1|
     1_0_2_102_3_255_11_255_12_255_13_255_6_1001_7_1_15_1_18_0_8_1|
     1_0_2_102_3_255_11_255_12_255_13_255_4_-1_6_1009_7_1_15_1_18_0_8_1|
     1_255_2_255_3_255_11_255_12_255_13_255_6_1002_5_1_7_1_15_1_18_0_8_1|
     1_255_2_255_3_255_11_255_12_255_13_255_6_1004_7_1_15_1_18_0_8_1|
     1_125_2_255_3_0_11_255_12_255_13_255_4_-1_6_1005_5_1_7_1_15_1_18_0_8_1|
     1_0_2_255_3_255_11_255_12_255_13_255_4_-1_6_1006_5_1_7_1_15_1_18_0_8_1|,
kA13,0,kA15,0,kA16,0,kA14,,kA6,0,kA7,0,kA17,0,kA18,0,kS39,0,kA2,0,kA3,0,kA8,0,kA4,0,kA9,0,kA10,0,kA11,0;
1,1,2,285,3,15;1,1,2,315,3,15;1,8,2,555,3,15;1,35,2,825,3,2;1,36,2,1515,3,15;1,39,2,4095,3,6;...
```

Position analysis of the 208 objects (all x ≡ 15 mod 30):

| ID | y mod 30 | Count |
|---|---|---|
| 1 (block) | 15 | 152 |
| 8 (spike) | 15 | 35 |
| 12, 13 (portals) | 15 | 1 each; the ship portal had `13,1` |
| 36 (orb) | 15 | 1 |
| 35 (yellow pad) | **2** | 15 |
| 39 (half spike) | **6** | 3 |

---

## 2. Level header (level start object)

### 2.1 Keys

Sources: gddocs level-start.md, plus the Geode 2.2081 `LevelSettingsObject` member annotations (`// property kAxx`), which confirm every key below [V-SRC].

| Key | Type | Meaning / values |
|---|---|---|
| `kS38` | string | Color channels (section 2.2). |
| `kS39` | int | Last color page shown in the editor. `0`. |
| `kA2` | int | Start game mode: 0 cube, 1 ship, 2 ball, 3 UFO, 4 wave, 5 robot, 6 spider, 7 swing. [V-SRC: gddocs enum, gd-sim, gd-info schema] |
| `kA3` | bool | Start mini. |
| `kA4` | int | Start speed. **Non-obvious mapping:** `0` = 1x (normal), `1` = 0.5x (slow), `2` = 2x, `3` = 3x, `4` = 4x. [V-SRC: Geode `enum class Speed {Normal=0, Slow=1, Fast, Faster, Fastest}`, gd.py `Speed`, gd-sim comment "Robtop stores 1x speed as 0 and slow speed as 1"]. gddocs' "actual speed" column is shifted. |
| `kA5` | bool | Legacy Obj-2 blending (pre-1.9). Do not write. |
| `kA6` | int | Background texture index. 0 or 1 = default BG; **clamped to 1–59** [V-SRC: OSGD `createBackground`: `index = MIN(MAX(index,1),59)`]. |
| `kA7` | int | Ground texture index, 1–22 (gd-info `$Ground` g1..g22). 0 behaves as 1 [UNVERIFIED clamp]. |
| `kA8` | bool | Start dual. |
| `kA9` | bool | 0 in the level start. A Start Pos object (ID 31) carries the same kA keys inline. gddocs says set kA9=1 on Start Pos [SRC:gddocs]. |
| `kA10` | bool | 2-player mode. |
| `kA11` | bool | Start with flipped gravity. |
| `kA13` | float | Song offset in **seconds**. |
| `kA14` | string | Editor guidelines `time~color~time~color~`, with colors 0 orange, 0.9 yellow, 1 green. Empty allowed (`kA14,,`). |
| `kA15` / `kA16` | bool | Song fade in / fade out. |
| `kA17` | int | Ground line style, **clamped to 1–3** [V-SRC: OSGD `GJGroundLayer::createLine`]. 1 = default thin line; 2 and 3 are alternates. |
| `kA18` | int | Font index. 0 = default Pusab. |
| `kA19`, `kA26` | int | Start Pos target order / target channel. Start Pos only. |
| `kA20` | bool | Reverse gameplay at start. |
| `kA21` | bool | Disable this Start Pos. Start Pos only. |
| `kA22` | bool | Platformer mode. 0 = classic. |
| `kA23`, `kA24` | bool | Unknown (`m_propertykA23/24`). Real 2.2 levels write `0` [UNVERIFIED]. |
| `kA25` | int | Middleground: 0 none, 1–3 = mg1..mg3. |
| `kA27` | bool | Compatibility: allow multi-rotation. |
| `kA28` | bool | Mirror mode at start. |
| `kA29` | bool | Rotate gameplay at start. |
| `kA31` | bool | Compatibility: enable player squeeze. |
| `kA32` | bool | Compatibility: fix gravity bug. |
| `kA33` | bool | Compatibility: fix negative scale hitboxes. |
| `kA34` | bool | Compatibility: fix robot jump (1 = fixed). |
| `kA35` | bool | Reset camera. Start Pos only. |
| `kA36` | int | Spawn group (group spawned at level start). |
| `kA37` | bool | Compatibility: dynamic level height. |
| `kA38` | bool | Compatibility: sort groups (spawn order left to right). |
| `kA39` | bool | Compatibility: fix radius collision. |
| `kA40` | bool | Compatibility: enable misc 2.2 changes. |
| `kA41` | bool | Compatibility: allow static rotate. |
| `kA42` | bool | Compatibility: reverse sync. |
| `kA43` | bool | Platformer no-time-penalty. |
| `kA44` | int | Unknown (`m_propertykA44`). |
| `kA45` | bool | Compatibility: decrease boost slide. |
| `kA46` | bool | Don't reset (`m_dontReset`). |
| `kA47` | int | Next free ID (`m_nextFreeID`), an editor bookkeeping value. |
| `kA48` | bool | Enable impulse fix (`m_enableImpulseFix`). |
| `kS29`–`kS37`, `kS1`–`kS20` | – | Legacy (pre-2.0 / pre-1.9) colors. Do not write; kS38 supersedes them. |

**Compatibility flags (kA27, kA31–kA34, kA37–kA42, kA45, kA48).** Whether a missing key means "legacy" or "modern" is [UNVERIFIED]; GD may decide from the key's absence or from `k50`. For a newly generated 2.2 level, **write them explicitly** with modern values: `kA32,1,kA33,1,kA34,1,kA37,1,kA38,1,kA39,1,kA40,1,kA41,1,kA42,1`. The modern values of `kA27`, `kA31`, `kA45` and `kA48` are uncertain; `1` is my best guess [UNVERIFIED]. For cube/ship gameplay without rotate triggers, these flags barely matter, except `kA40`.

### 2.2 kS38 color channel string  [V-REAL] (format) [V-SRC] (key meanings: gddocs color-string, gd.py `color_channels.py`)

`kS38,<ch>|<ch>|...|<ch>|`. Each channel is `k_v_k_v_...`. The value ends with `|` and is followed by the header's `,`.

| Sub-key | Meaning | Typical value |
|---|---|---|
| `1`, `2`, `3` | R, G, B (0–255), the "from" color = the channel's color | |
| `4` | Player color: `-1` or `0` = none, `1` = P1, `2` = P2. Both -1 and absent appear in real strings. | `-1` |
| `5` | Blending (additive, `GL_SRC_ALPHA, GL_ONE`) | Present as `5_1` only when on |
| `6` | **Channel ID** (required) | 1–999 custom; 1000+ special |
| `7` | Opacity (0–1 float) | `1` |
| `8` | "Toggle opacity" flag; always `1` in real data | `1` |
| `9` | Copy-color source channel ID. Nonzero = copy color. | |
| `10` | Copy-color HSV string, e.g. `0a1a1a0a0` | |
| `11`, `12`, `13` | "To" RGB (color-action target). Real data always has `255_255_255`. | `255` |
| `14` | Delta time (action state) | omit |
| `15` | "To" opacity | `1` |
| `16` | Duration (action state) | omit |
| `17` | Copy opacity (with key 9) | |
| `18` | Unknown; always `0` | `0` |

Real-string channel template, safe to emit for every channel:
`1_R_2_G_3_B_11_255_12_255_13_255_4_-1_6_ID[_5_1]_7_1_15_1_18_0_8_1`

Copy-color channel (e.g. channel 2 = channel 1 darkened):
`1_255_2_255_3_255_11_255_12_255_13_255_4_-1_6_2_7_1_15_1_18_0_8_1_9_1_10_0a1a0.6a0a0`

Game defaults, which are also what you get when kS38 is omitted [V-REAL]:

| Channel | Default |
|---|---|
| BG 1000 | 40,125,255 |
| G1 1001 | 0,102,255 |
| G2 1009 | 0,102,255 |
| Line 1002 | 255,255,255, blending |
| Obj 1004 | 255,255,255 |
| P1 1005 | 125,255,0, blending |
| P2 1006 | 0,255,255, blending |

The middleground channels MG 1013 / MG2 1014 are absent from 2.1 strings. Write them explicitly when kA25 ≠ 0 [UNVERIFIED default].

### 2.3 Minimal valid 2.2 header

Absolute minimum (structurally identical to real strings; the game supplies default colors):

```
kA13,0,kA15,0,kA16,0,kA14,,kA6,0,kA7,0,kA17,0,kA18,0,kS39,0,kA2,0,kA3,0,kA8,0,kA4,0,kA9,0,kA10,0,kA11,0
```

**Recommended 2.2 header for generation.** Key order mimics real files; values are my choices. Line breaks are for reading only; write it on one line.

```
kS38,1_20_2_20_3_60_11_255_12_255_13_255_4_-1_6_1000_7_1_15_1_18_0_8_1|
1_30_2_30_3_90_11_255_12_255_13_255_4_-1_6_1001_7_1_15_1_18_0_8_1|
1_30_2_30_3_90_11_255_12_255_13_255_4_-1_6_1009_7_1_15_1_18_0_8_1|
1_255_2_255_3_255_11_255_12_255_13_255_4_-1_6_1002_5_1_7_1_15_1_18_0_8_1|
1_255_2_255_3_255_11_255_12_255_13_255_4_-1_6_1004_7_1_15_1_18_0_8_1|
1_40_2_40_3_110_11_255_12_255_13_255_4_-1_6_1013_7_1_15_1_18_0_8_1|,
kA13,0,kA15,0,kA16,0,kA14,,kA6,1,kA7,1,kA25,0,kA17,1,kA18,0,kS39,0,kA2,0,kA3,0,kA8,0,kA4,0,
kA9,0,kA10,0,kA22,0,kA20,0,kA28,0,kA29,0,kA11,0,
kA32,1,kA33,1,kA34,1,kA37,1,kA38,1,kA39,1,kA40,1,kA41,1,kA42,1
```

### 2.4 Start Pos (ID 31)  [V-SRC: gddocs; gd-sim parses kA keys out of the ID-31 object string]

`1,31,2,<x>,3,<y>,kA2,<mode>,kA3,<mini>,kA4,<speed>,kA8,<dual>,kA9,1,kA10,0,kA11,<flip>,kA20,0,kA28,0,kA21,0`

Visual keys (kA6, kA7, kA13–kA16, kA18, kS*) do not work in a Start Pos.

---

## 3. Object property keys

### 3.1 Coordinates  [V-REAL] + [V-SRC]

- **Units:** 30 units = 1 block (grid cell). x grows to the right; y grows upward.
- **Ground top is y = 0.** A block resting on the ground has **y = 15**; the next row up has y = 45, and so on. Evidence:
  - real level strings (blocks and spikes at y=15 on the floor row);
  - gd-sim: player starts at (0, 15), `floor = 0`;
  - OSGD `GJGroundLayer::getGroundY() → 0`.
  Internally GD adds a +90 screen offset, which is invisible in the string.
- **Grid placement:** cell centers are at `15 + 30k` on both axes. The editor's level origin is x = 0, and the player starts at x = 0 [SRC: gd-sim].
- **Bottom-snapped objects:** some objects sit at the bottom of their cell instead of its center. In real strings, pad 35 is at **y = 30k + 2** and half spike 39 at **y = 30k + 6** [V-REAL].
  - Other pads (140, 67, 1332, 3005) presumably also use y = 30k + 2 [UNVERIFIED]. GD-Remake uses 2.5 for pad 140.
  - The editor's per-object offset is `EditorUI::offsetForKey(id)`, which is not decompiled in these sources.
  - Medium spike 103 and slab 40 offsets are [UNVERIFIED]. Slab 40 is believed to sit at the top of its cell, at y = 30k + 23 (hitbox 30×14).
  - The game never re-snaps loaded objects, so any float y is valid. Choose y so that the **hitbox** (section 7) touches the surface you want.
- **Rotation (key 6):** degrees, **clockwise positive** [SRC:gddocs]. A ceiling spike is `8` with `6,180`, or flipY `5,1`.
- **Scale:**
  - `32` = uniform scale (legacy, still read).
  - `128` / `129` = scaleX / scaleY (2.2).
  - gd-sim multiplies all three for hitboxes, so do not set both 32 and 128/129 unless you mean the product.
- **Time ↔ x.** Player horizontal speed in units/s [V-SRC: gd.py `SpeedMagic`, GD-Remake from GDP, gd-sim]:

| Speed | 0.5x | 1x | 2x | 3x | 4x |
|---|---|---|---|---|---|
| units/s | 251.16 | 311.58 | 387.42 | 468.00 | 576.00 |

  For a music-synced trigger or obstacle at song time t (constant speed): `x = (t - kA13) * v`. Use piecewise segments across speed portals.
- **Triggers** (non-touch, non-spawn) fire when the player's x reaches the trigger's x. Their y does not matter [V-SRC: G.js lays triggers out on an x line at an arbitrary y]. Put them above the gameplay area (for example y ≥ 600) to keep the editor readable.

### 3.2 Common object keys

Sources: gd-info-explorer `@Common` (2.2) cross-checked with the gddocs table and Geode `GameObject` property annotations [V-SRC unless tagged].

| Key | Name | Type / notes |
|---|---|---|
| 1 | Object ID | int (required) |
| 2, 3 | x, y | float (required) |
| 4, 5 | flip X, flip Y | bool |
| 6 | rotation | float, degrees, CW+ |
| 13 | "checked" / preview | bool. On game-mode portals this is the editor bounds-guide checkbox (`m_shouldPreview`); real ship portals carry `13,1`. Gameplay effect: none known. |
| 19 | legacy 1.9 color ID | Do not write. It overrides 21/22. |
| 20 | editor layer 1 | int, editor-only (default 0) |
| 61 | editor layer 2 | int, editor-only |
| 21 | main (base) color channel | int channel ID |
| 22 | secondary (detail) color channel | int |
| 41 / 43 | main HSV enabled / main HSV string | bool / HSV |
| 42 / 44 | detail HSV enabled / detail HSV string | bool / HSV |
| 497 | single-color type: 0 default, 1 base, 2 detail | int |
| 24 | Z layer | enum (section 6) |
| 25 | Z order | int (section 6) |
| 115 | ord (2.2 order) | int |
| 32 | uniform scale | float |
| 128, 129 | scale X, scale Y | float |
| 131, 132 | warp Y angle, warp X angle | float |
| 33 | single group (legacy) | int |
| 57 | groups | `.`-list of up to 10 group IDs (1–9999) |
| 274 | parent groups (pink groups) | `.`-list |
| 34 | group parent | bool |
| 279 | area parent | bool |
| 108 | linked group (editor link controls) | int |
| 64 | don't fade | bool |
| 67 | don't enter | bool |
| 116 | no effects (e.g. no portal flash) | bool |
| 96 | no glow | bool |
| 507 | no particles | bool |
| 103 | high detail (hidden in LDM) | bool |
| 121 | no touch (no hitbox) | bool |
| 134 | passable | bool |
| 135 | hide | bool |
| 136 / 289 | non-stick X / Y | bool |
| 495 | extra sticky | bool |
| 137 | ice block | bool |
| 193 | grip slope | bool |
| 509 / 496 | don't boost X / Y | bool |
| 511 | extended collision | bool |
| 369 | center effect | bool |
| 356 | scale stick | bool |
| 372 | no audio scale | bool |
| 117 | reverse (orbs/pads) | bool |
| 99 | multi activate (orbs/pads, classic) | bool |
| 444 | no multi activate (platformer) | bool |
| 343 | enter channel | int |
| 446 | material | int |
| 534 | control ID | int |
| 170 | channel | int |
| 36 | "interactible" / active trigger | bool. G.js writes `36,1` on triggers; real triggers carry it [UNVERIFIED meaning]. Recommended on every trigger. |
| 155, 156 | main / detail color "key index" | int. Written by the game on all objects; optional, the game recomputes it [UNVERIFIED]. |
| 54 | yellow teleport portal y-offset (747) | float |
| 31 | text (ID 914) | base64 (G.js uses standard base64; URL-safe is safer [UNVERIFIED]) |

Rotating objects (saws, gears): `97` custom rotation speed (deg/s) and `98` disable rotation.

Animated objects: `106` randomize start, `107` animation speed, `122` use speed, `123` animate on trigger, `126`, `127`, `214`, `462` single frame, `592` offset animation.

### 3.3 Trigger-common keys  [V-SRC: gd-info `@Trigger`, Geode EffectGameObject, G.js]

| Key | Meaning |
|---|---|
| 11 | Touch triggered: fires when the player touches the trigger's hitbox instead of crossing its x. |
| 62 | Spawn triggered: only fires when spawned (a spawn trigger, or another trigger activating its group). It needs a group in 57. |
| 87 | Multi-trigger: a spawn-triggered trigger can fire more than once. G.js sets 62=1 and 87=1 on all group-contained triggers. |
| 30 | Easing (enum below) |
| 85 | Easing rate (default **2**, as in G.js) |
| 280 | Ignore group parent |
| 281 | Ignore linked objects |
| 36 | Set `1` (see 3.2) |

**Easing enum (key 30)** [V-SRC]:

| Value | Easing | Value | Easing | Value | Easing |
|---|---|---|---|---|---|
| 0 | none | 7 | bounce in-out | 14 | sine in |
| 1 | ease in-out | 8 | bounce in | 15 | sine out |
| 2 | ease in | 9 | bounce out | 16 | back in-out |
| 3 | ease out | 10 | exponential in-out | 17 | back in |
| 4 | elastic in-out | 11 | exponential in | 18 | back out |
| 5 | elastic in | 12 | exponential out | | |
| 6 | elastic out | 13 | sine in-out | | |

### 3.4 Move trigger 901 keys  [V-SRC]

| Key | Meaning |
|---|---|
| 51 | Target group |
| 28, 29 | Move X / Y offset in **GD units (30 per block)**. G.js and SPWN take "10 per block" input and multiply by 3 when writing [V-SRC]. |
| 10 | Duration (s) |
| 30, 85 | Easing, easing rate |
| 58, 59 | Lock to player X / Y |
| 141, 142 | Lock to camera X / Y |
| 143, 144 | Mod X / Y (multiplier for player/camera lock) |
| 100 | Target mode (move to a target) |
| 71 | Target-position group (the object to move to) |
| 395 | Center group for target mode |
| 138, 200 | Target P1 / P2 |
| 101 | X/Y-only: 0 both, 1 X only, 2 Y only (Geode `MoveTargetType`) |
| 394 | Direction mode |
| 396 | Direction distance |
| 397 | Dynamic mode |
| 393 | Small step. Editor-display granularity; recommend leaving it unset. Its effect on the stored 28/29 is [UNVERIFIED]. |
| 544 | Silent (instant, no timing) |

---

## 4. Object IDs

Unless tagged, names come from gd-info-explorer `objects.csv` (2.2) and classes from `schema.txt` [V-SRC]. Hitbox sizes (section 7) come from gd-sim.

### 4.1 Basic blocks (solid unless noted)

| ID | Name | Notes |
|---|---|---|
| 1 | Black Gradient Square | The default block. 30×30 solid. [V-REAL] |
| 2 | Grid Patterned Top Square | |
| 3 | Grid Patterned Outer Corner | |
| 4 | Grid Patterned Inner Corner | |
| 5 | Grid Patterned Inner Square | **Decoration, no hitbox.** Not in gd-sim's solid list. |
| 6 | Grid Patterned Top Pillar | |
| 7 | Grid Patterned Pillar | |
| 83 | Grid Patterned Square | |
| 40 | Black Gradient Single Slab | 30×14 solid |
| 369, 370 | Black Gradient Slab middle / side | 30×14 |
| 195, 196 | Black Gradient small square / small slab | 15×15, 15×8 |
| 62, 65, 66, 68 | Wavy Black Slab / right edge / left edge / single | 30×16 |
| 63 | Wavy Black Slab outer corner | 30×30 |
| 64 | Wavy Black Slab inner corner | 15×15 |
| 69 | Cross Black Square | |
| 70–75 | Beveled set | 73 "inner" = deco |
| 76–82 | Mechanical set | 79 absent; 80 inner = deco |
| 90–96 | Black (plain) set | 94 inner is in the solid list |
| 116–122 | Brick set | 120 inner = deco |
| 160–169 | Solid-stripe set | |
| 207–213 | Colored Square set (colorable via 21) | 211 inner = deco |
| 215, 219, 220 | Colored slab / small slab / small square | |
| 247–254 | Colored Grid Patterned set | 251 inner = deco |
| 255–261 | Colored Beveled set | 259 inner = deco |
| 263–268 | Mechanical Colored set | 266 inner = deco |
| 269–275 | Colored Bricks set | 273 non-solid |
| 467 | Outline Square (2.0 thin outline set) | Solid |
| 468 | Outline Top | 30×1.5 solid line |
| 469 | Outline Outer Corner | |
| 470 | Outline Top Pillar | |
| 471 | Outline Pillar | |
| 472 | Outline Inner Corner | [UNVERIFIED solidity] |
| 475 | Outline Slope Side | 30×1.5 |
| 661 | Outline Small Square | 15×15 |
| 662–664 | Outline Slab / middle / side | 30×15 |
| 1155–1157 | Small Outline outer corner / top pillar / pillar | 15×15 |
| 1202 | Thick Outline Top | 30×3 |
| 1203 | Thick Outline Outer Corner | 30×30 |
| 1204 | Thick Outline Top Pillar | 30×30 |
| 1209, 1210 | Thick Outline Pillar / Square | 30×30 |
| 1220 | Thicker Outline Top | 30×6 |
| 1221, 1222, 1226 | Thicker Outline outer corner / top pillar / pillar | 30×30 |
| 1227 | Thicker Outline Square | 30×7 per gd-sim |
| 1260, 1262, 1264 | Outer Outline Side / thick / thicker | 30×1.5 / 3 / 6 |
| 1903–1905 | Colored Gradient single slab / middle / side | 30×14 |
| 1910, 1911 | Colored Gradient small block / small slab | |
| 143 | Breakable Brick | Breakable |
| 146, 147, 204, 206 | Invisible square / slab / small slab / small square | Solid but invisible |

2.2 adds no new standard tile set. Its new IDs (about 2000–4600) are mostly deco: pixel art, "Bubbly Liquid" blocks 2703+, particles 3800–3990, collectible pixel items 4401–4539. For generated levels, use the 2.0/2.1 colorable sets above.

### 4.2 Spikes and hazards  [V-SRC]

| ID | Name | Hitbox w×h |
|---|---|---|
| 8 | Black Gradient Spike (default spike) | 6×12 [V-REAL position y=15] |
| 39 | Half spike | 6×5.6 [V-REAL y = 30k+6] |
| 103 | Small (medium-height) spike | 4×7.6 |
| 392 | Tiny spike | 2.6×4.8 |
| 144, 145, 205, 459 | Invisible spike / small / half / tiny | |
| 216, 217, 218, 458 | Colored spike / half / small / tiny | |
| 177, 178, 179 | Ice spikes | |
| 9 | Non-colorable spike pit ("ground spikes") | 9×10.8, **hazard** |
| 61 | Wavy pit hazard | 9×7.2 |
| 135 | Thorn pit hazard | 14.1×20 |
| 243, 244 | Wavy pit right / left edge | |
| 363, 364 | Spike slope hazard / wide | |
| 365, 421, 422, 768 | Spike / wide-spike hazards | |
| 1711–1714 | Colored thorn pits | |
| 1715 | Spike Black Pit Hazard | **Hazard, not decoration** (9×10.8) |
| 1716–1733 | Colored-era pit / spike / square hazards | See gd-sim table |
| 191, 198, 199, 393, 1889–1892 | **Fake** spikes | No hitbox |
| 18–21 | Large / medium / small / very small **Decorative** Spikes | No hitbox; ground-edge deco |

### 4.3 Saws (circular hitbox; gd-sim radius)  [V-SRC]

| ID | Name | Radius |
|---|---|---|
| 88 | Large black sawblade | 32.3 |
| 89 | Medium black sawblade | 21.6 |
| 98 | Small black sawblade | 12 |
| 186 | Large outline blade | 32.3 |
| 187 | Medium outline blade | 21.96 |
| 188 | Small outline blade | 12.6 |
| 740, 741, 742 | Invisible blades L / M / S | 32.3 / 21.96 / 12.6 |
| 1705 | Large saw | 32.3 |
| 1706 | Medium saw | 21.6 |
| 1707 | Small saw | 12 |
| 183–185 | Large / medium / small blade | 15.66 / 20.4 / 2.85 |
| 397–399, 1708–1710 | Spike blades | 28.9 / 17.44 / 12.9 |
| 675–680, 1734–1736 | Gear blades | 32 / 17.51 / 12.48 |
| 1619, 1620 | Scythe blades | 25 / 15 |

All of these are `RotatingObject` (keys 97/98).

### 4.4 Orbs  [V-SRC: csv names + Geode GameObjectType + G.js]

| ID | Orb |
|---|---|
| 36 | Yellow jump orb [V-REAL] |
| 141 | Pink orb |
| 1333 | Red orb |
| 84 | Blue gravity orb |
| 1022 | Green gravity orb |
| 1330 | Black "drop" orb |
| 1704 | Green dash orb (`DashOrb`: 586 speed, 588 end boost, 590 max duration, 587 allow collide, 589 stop slide) |
| 1751 | Pink gravity-dash orb |
| 1594 | Toggle orb (Collectible-like: 51 group, 56 activate) |
| 3004 | Spider orb |
| 3027 | Teleport orb (teleport keys 51, 55, 345–354, …) |

Hitbox is 36×36 in GD-Remake; gd-sim does not list orbs [UNVERIFIED].

### 4.5 Pads  [V-SRC]

| ID | Pad | Hitbox |
|---|---|---|
| 35 | Yellow | 25×4; y = 30k+2 [V-REAL] |
| 140 | Pink | 25×5 |
| 1332 | Red | |
| 67 | Blue gravity | 25×6 |
| 3005 | Spider | |

### 4.6 Portals  [V-SRC: csv, G.js `obj_ids.portals`, gd-sim, Geode GameObjectType]

**Game mode** (hitbox 34×86, y = cell center, typically 30k+15):

| ID | Mode |
|---|---|
| 12 | Cube |
| 13 | Ship |
| 47 | Ball |
| 111 | UFO |
| 660 | Wave |
| 745 | Robot |
| 1331 | Spider |
| 1933 | Swing |

Flight bounds [SRC:gd-sim]: `floor = max(0, 30·ceil((portalY − (H/2 + 30))/30))`, `ceiling = floor + H`, where H = 300 for ship/UFO/wave (swing also 300 per GD-Remake) and 240 for ball (spider 240 [UNVERIFIED]). A portal at y = 45…165 therefore gives floor 0 and ceiling 300.

**Other portals:**

| Kind | IDs |
|---|---|
| Gravity (25×75) | 10 blue = normal (down), 11 yellow = flipped (up), 2926 green = toggle |
| Size | 99 green = normal, 101 pink = mini |
| Dual | 286 dual on, 287 dual off (exit) |
| Mirror | 45 orange = mirror on, 46 blue = mirror off |
| Speed | 200 yellow 0.5x, 201 blue 1x, 202 green 2x, 203 pink 3x, 1334 red 4x |
| Teleport | 747 linked teleport pair (key 54 = y-offset of the orange exit; 749 is the internal exit ID, not placeable), 2902 unlinked blue teleport portal |

2.2 has no "speed trigger": speed changes are portals only. Game-mode portals also accept camera-mode keys 111–114 (free mode) [SRC:gddocs].

### 4.7 Special objects

| ID | Object |
|---|---|
| 31 | Start position |
| 1329 | User coin (max 3 per level; gmd-api/OSGD `addObjectFromVector` rejects a 4th) |
| 142 | Secret (official) coin. **Rejected** when loaded in the editor. |
| 914 | Text (key 31 base64) |
| 1615 | Counter label |
| 2063 | Checkpoint |
| 1816 | Collision block |
| 1755, 1813, 1829, 1859 | D, J, S, H modifier blocks |
| 2069 / 3645 | Force block / force circle |
| 34 | 1.0 level end trigger (legacy) |
| 1931 | Old end trigger / end wall |

### 4.8 Slopes (solid; gd-sim 45° = 30×30, "wide" 26.6° = 60×30)

| Set | 45° slope | Wide slope |
|---|---|---|
| Black Gradient | 371 | 372 |
| Grid Patterned | 1743 | 1744 |
| Cornerless grid | 289 | 291 |
| Wavy black | 294 | 295 |
| Mechanical | 305 | 307 |
| Mechanical colored | 349 | 351 |
| Beveled black | 1745 | 1746 |
| Black | 1747 | 1748 |
| Brick | 1749 | 1750 |
| Outline | 1338 | 1339 |
| Non-colorable outline | 665 | 666 |
| Invisible | 673 | 674 |
| Colored Gradient | 1906 | 1907 |

Slope hazards: 363/364, 1717/1718 (spike slopes), 1723/1724 (wavy).

Orientation is via 4/5/6. The default orientation (which way it rises) is [UNVERIFIED]; check visually in-game.

### 4.9 Decoration that looks good in 2.2

None of these have a hitbox (not in gd-sim's lists).

- **Glow** (best on a blending color channel; set 21 to a channel with `5_1`):
  - 503 straight medium glow, 504 outer corner, 505 inner corner;
  - 1009–1013 small/large glow corners and straight;
  - 1291–1293 half/small glows;
  - 1754 thin line glow;
  - 1269–1274 and 1758–1763 slope glows;
  - 1886–1888 glow orbs (M/S/L).
- **Pulsing (beat-reactive):** 50 filled circle, 51 circle, 52 heart, 53 diamond, 54 star, 60 music note, 132/460/494 large arrows, 133 "!", 136 "?", 148 square, 149 triangle, 150 cross, 236 large hollow circle, 495–497 large square / hollow square / circle.
- **Clouds:** 129–131, 222–224 (round), 48/49 fading clouds.
- **Rotating deco:** 85–87 and 97 gears; 137–139 wheels; 154–156 spike wheels (these are hazards per name: "Spike Wheel" [UNVERIFIED hitbox]); 180–182 cartwheels.
- **Chains and rods:** 15–17 rods, 41/106/107/110 chains.
- **Ground edge spikes deco:** 18–21.
- **3D lines (3DL channel 1003):** 506–520.

### 4.10 Triggers (IDs)  [V-SRC: csv names, gd-info classes, G.js `obj_ids.triggers`]

| ID | Trigger | ID | Trigger |
|---|---|---|---|
| 899 | Color | 901 | Move |
| 1006 | Pulse | 1007 | Alpha |
| 1049 | Toggle | 1268 | Spawn |
| 1346 | Rotate | 1347 | Follow |
| 1520 | Shake | 1585 | Animate |
| 1814 | Follow player Y | 1595 | Touch |
| 1611 | Count | 1811 | Instant count |
| 1615 | Counter label (not a trigger) | 1616 | Stop |
| 1612 / 1613 | Hide / show player | 1812 | On death |
| 1815 | Collision | 1817 | Pickup |
| 1818 / 1819 | BG effect on / off | 1912 | Random |
| 2068 | Advanced random | 1913 | Camera zoom |
| 1914 | Camera static | 1916 | Camera offset |
| 2015 | Camera rotate | 2062 | Camera edge |
| 2925 | Camera mode | 2016 | Camera guide |
| 2901 | Gameplay offset (camera) | 2900 | Gameplay rotation ("arrow") |
| 1917 | Reverse | 1932 | Player control |
| 1934 | Song | 3605 | Edit song |
| 3602 / 3603 | SFX / edit SFX | 1935 | Time warp |
| 2066 | Gravity | 2067 | Scale |
| 2899 | Options | 2903 | Gradient |
| 2904 | Shader setup | 2905 | Shockwave |
| 2907 | Shock line | 2909 | Glitch |
| 2910 | Chromatic | 2911 | Chromatic glitch |
| 2912 | Pixelate | 2913 | Lens circle |
| 2914 | Radial blur | 2915 | Motion blur |
| 2916 | Bulge | 2917 | Pinch |
| 2919 | Grayscale | 2920 | Sepia |
| 2921 | Invert color | 2922 | Hue |
| 2923 | Edit color | 2924 | Split screen |
| 3600 | End | 3022 | Teleport |
| 3029 / 3030 / 3031 | Change BG / ground / MG | 3606 / 3612 | BG speed / MG speed |
| 2999 | Edit MG | 3016 / 3660 / 3661 | Advanced follow / edit / retarget |
| 3006–3015 | Area move / rotate / scale / fade / tint and their edit versions | 3024 | Area stop |
| 3017–3021, 3023 | Enter move / rotate / scale / fade / tint / stop | 22–28, 55–59 | Legacy enter-effect objects |
| 1915 | No enter effect | 3032 / 3033 | Keyframe / keyframe animation |
| 3604 | Event | 3607 | Sequence |
| 3608 | Spawn particle | 3609 | Instant collision |
| 3613 | UI | 3614 / 3615 / 3617 | Time / time event / time control |
| 3618 | Reset | 3619 / 3620 / 3641 | Item edit / item compare / item persistence |
| 3640 | Collision state block | 3642 | BPM guide |
| 3643 | Toggle block | 3655 | Object control |
| 3662 | Link visible | 3663 | Percent |
| 32 / 33 | Enable / disable ghost trail | 29 / 30 | Legacy BG / ground color triggers (1.x, key 14 tint ground); use 899 instead |

### 4.11 Properties of the triggers we use

Every trigger also takes the common keys `2, 3, 36, 11, 62, 87, 57` (section 3.3). Sources [V-SRC]: gd-info `schema.txt` class blocks, cross-checked with Geode `EffectGameObject` annotations and G.js builders.

**Color 899** (`@ColorTrigger`; G.js `color.set`)

| Key | Meaning | Typical |
|---|---|---|
| 23 | Target color channel | 1–999, 1000 BG, 1001 G1, 1009 G2, 1013 MG, 1014 MG2, 1002 Line, 1003 3DL, 1004 Obj |
| 7, 8, 9 | R, G, B | 0–255 |
| 10 | Duration (s) | 0–2 |
| 35 | Opacity | `1` |
| 17 | Blending | bool |
| 15 / 16 | Use player color 1 / 2 | bool |
| 50 | Copy color from channel | int |
| 49 | Copy HSV string | `0a1a1a0a0` |
| 60 | Copy opacity | bool |
| 210 | Legacy HSV off | bool |

Key 14 (tint ground) is **not** a 899 key. It only exists on legacy BG trigger 29. Static channels cannot be targets: 1005 P1, 1006 P2, 1007 LBG, 1010 black, 1011 white, 1012 lighter [UNVERIFIED for 1005–1007, documented as "static" by gddocs].

Example: `1,899,2,300,3,600,36,1,23,1000,7,255,8,40,9,90,10,0.5,35,1`

**Move 901:** see section 3.4.

**Pulse 1006** (`@PulseTrigger`)

| Key | Meaning |
|---|---|
| 51 | Target: a channel ID or a group ID, depending on 52 |
| 52 | Target type: 0 = channel, 1 = group |
| 45 / 46 / 47 | Fade in / hold / fade out (s) |
| 7, 8, 9 | RGB (when 48 = 0) |
| 48 | Mode: 0 = color, 1 = HSV |
| 49 | HSV string (mode 1) |
| 50 | Copy color ID (HSV mode can base on a channel) |
| 86 | Exclusive (cancels other pulses on the target) |
| 65 / 66 | Main only / detail only (group target) |
| 210 | Static HSV off |

Example beat flash on BG: `1,1006,2,x,3,600,36,1,51,1000,52,0,7,255,8,255,9,255,45,0,46,0.05,47,0.3`

**Alpha 1007:** 51 group, 35 opacity (0–1), 10 duration.

**Toggle 1049:** 51 group, 56 activate (1 = enable, 0 = disable).

**Spawn 1268**

| Key | Meaning |
|---|---|
| 51 | Group to spawn |
| 63 | Delay (s) |
| 556 | Delay ± (random) |
| 441 | Spawn ordered |
| 442 | Remaps `old.new.old.new` |
| 581 | Reset remap |
| 102 | Preview disable |

**Rotate 1346**

| Key | Meaning |
|---|---|
| 51 | Target |
| 71 | Center group |
| 68 | Degrees |
| 69 | Times 360 |
| 70 | Lock object rotation |
| 10 | Duration |
| 30 / 85 | Easing / rate |
| 100 | Aim mode |
| 394 | Follow mode |
| 401 | Rotation target ID |
| 402 | Rotation offset |
| 138 / 200 | Aim at P1 / P2 |
| 397 | Dynamic |
| 403 | Dynamic easing |
| 516–519 | Min/max x/y IDs |

**Follow 1347:** 51 target, 71 follow group, 72 / 73 x / y mod, 10 duration.

**Follow player Y 1814:** 51 target, 10 duration, 90 speed, 91 delay, 92 y-offset, 105 max speed.

**Shake 1520:** 75 strength (1–5 typical), 84 interval (0 = every frame; 0.03–0.1 typical), 10 duration.

**Animate 1585:** 51 group, 76 animation ID.

**Stop 1616:** 51 group (or control ID with 535), 580 mode (0 stop, 1 pause, 2 resume).

**Random 1912:** 51 group 1, 71 group 2, 10 chance (%).

**Touch 1595:** 51 group, 81 hold mode, 82 toggle (0 none, 1 on, 2 off), 89 dual mode, 198 P1/P2 only (0, 1, 2).

**Count 1611:** 80 item ID, 51 group, 77 target count, 56 activate, 104 multi activate.

**Camera zoom 1913:** 371 zoom (1 = default; for example 0.8 = zoomed out, 1.3 = zoomed in; editor range about 0.4–3 [UNVERIFIED]), 10 duration, 30 / 85 easing.

**Camera static 1914** (`@StaticCameraTrigger`; G.js `camera_static`)

| Key | Meaning |
|---|---|
| **71** | Target group (center object). **Not** 51. |
| 10 | Duration |
| 30 / 85 | Easing / rate |
| 101 | X/Y only (0 both, 1 x, 2 y) |
| 212 | Follow |
| 213 | Follow easing |
| 110 | Exit static |
| 465 | Exit instant |
| 453 / 454 | Smooth velocity / modifier |

**Camera offset 1916:** 28 / 29 x / y offset (units, 30 per block; G.js writes `x*3` from 10-per-block input), 10 move time, 30 / 85 easing, 101 x/y only.

**Camera rotate 2015:** 68 degrees, 10 move time, 30 / 85 easing, 70 add (relative), 394 snap 360.

**Camera edge 2062:** 51 target group, 164 edge (1 left, 2 right, 3 up, 4 down).

**Camera mode 2925:** 111 free mode, 112 edit camera settings, 113 easing, 114 padding, 370 disable grid snap.

**Gameplay offset 2901:** no specific keys are listed in the 2.2 schema [UNVERIFIED]; it probably uses 28 / 29. Avoid it.

**Song 1934**

| Key | Meaning |
|---|---|
| 392 | Song ID |
| 406 | Volume |
| 404 | Speed |
| 432 | Channel |
| 413 | Loop |
| 408 / 410 | Start / end (ms) |
| 409 / 411 | Fade in / out (ms) |
| 399 | Prep |
| 400 | Load prep |
| 595 | Don't reset |

**Gravity 2066:** 148 gravity, 138 / 200 / 201 P1 / P2 / PT.

**Player control 1932:** 138 / 200 P1 / P2, 540 stop jump, 541 stop move, 542 stop rotation, 543 stop slide.

**Options 2899:** each key takes 0 unset, 1 on, -1 off. Keys: 161 hide ground, 195 hide MG, 162 / 163 hide P1 / P2, 165 / 199 disable P1 / P2 controls, 160 unlink dual gravity, 532 hide attempts, 159 streak additive, 575 audio on death, 576 no death SFX, 573 / 574 respawn time, 593 boost slide.

**Gradient 2903:** 174 blending (0 normal, 1 add, 2 multiply, 3 invert), 202 layer (TriggerLayer enum: bg, mg, b5, b4, b3, b2, b1, p, t1, t2, t3, t4, g, ui, max), 203 / 204 / 205 / 206 the U/BL, D/BR, L/TL and R/TR vertex group IDs, 207 vertex mode, 208 disable, 209 gradient ID, 456 preview opacity. Where the colors come from (the vertex objects?) is [UNVERIFIED].

**End 3600:** 51 spawn group, 71 target position group, 460 no effects, 461 no SFX, 487 instant.

**Time warp 1935:** 120 time mod.

**Shader setup 2904:** 192 disable all, 188 no player particles, 196 / 197 lowest / highest layer. Individual shader keys (175–194, 290, 291, 512–515) are listed in gd-info `schema.txt`.

---

## 5. Color channels and palettes

### 5.1 Channel IDs  [V-SRC: gddocs level-colors, G.js constants]

| ID | Channel | Triggerable (899 target) |
|---|---|---|
| 1–999 | Custom (2.2 max 999) | yes |
| 1000 | BG | yes |
| 1001 | G1 (ground) | yes |
| 1009 | G2 (ground 2) | yes |
| 1013 | MG | yes |
| 1014 | MG2 | yes |
| 1002 | Line | yes |
| 1003 | 3DL | yes |
| 1004 | Obj (default outline/spike color of the black objects) | yes |
| 1005 / 1006 | P1 / P2 (static, player icon colors) | no |
| 1007 | LBG (light BG, derived from BG and P1) | no |
| 1010 | Black | no |
| 1011 | White | no |
| 1012 | Lighter | no |

**LBG** formula: take BG's HSV, subtract 20 from saturation, then blend P1 toward it by V/100 [SRC:gddocs].

### 5.2 Copy color (2.2)

A channel (kS38 keys `9`, `10`, `17`) or a color trigger (keys `50`, `49`, `60`) can copy another channel live, with an HSV adjustment. The copy follows the source when the source changes, so one BG color trigger can recolor a whole derived palette. Opacity is copied only with `17_1` (header) or `60,1` (trigger). Blending is never copied.

### 5.3 HSV string  [V-SRC]

`h a s a v a sChecked a vChecked`, e.g. `-20a0.8a1.2a0a0`.

- h is −180…180.
- s and v are multipliers 0…2 when "checked" = 0, or additive −1…1 when checked = 1.
- Identity is `0a1a1a0a0`.

To use it on objects: `41,1,43,<hsv>` for main, `42,1,44,<hsv>` for detail.

### 5.4 Palettes

These are design suggestions [design, not data]. They follow GD convention: G1 darker than BG, Line white with blending, and Obj contrasting with the BG for readable spikes.

| Theme | BG 1000 | G1 1001 | G2 1009 | Line 1002 | Obj 1004 | Accent ch.1 (blend) | MG 1013 |
|---|---|---|---|---|---|---|---|
| Classic blue (game default) | 40,125,255 | 0,102,255 | 0,102,255 | 255,255,255 | 255,255,255 | 0,200,255 | – |
| Night neon | 18,10,40 | 40,15,90 | 25,10,60 | 255,255,255 | 255,90,220 | 0,230,255 | 30,15,70 |
| Sunset | 255,110,60 | 120,30,60 | 90,20,45 | 255,230,200 | 255,255,255 | 255,200,0 | 200,70,70 |
| Toxic | 10,30,10 | 20,70,20 | 10,45,10 | 180,255,120 | 160,255,60 | 120,255,0 | 15,50,15 |
| Ice | 140,200,255 | 40,90,160 | 30,70,130 | 255,255,255 | 230,250,255 | 120,220,255 | 90,150,220 |
| Monochrome / boss | 15,15,15 | 35,35,35 | 25,25,25 | 255,255,255 | 255,40,40 | 255,60,60 | 30,30,30 |

Good practice:

- Put colorable blocks on custom channels (`21` base, `22` detail), for example base = 1, detail = 2, where 2 copies 1 with V×0.6.
- Glow deco goes on a separate blending channel.
- Drive changes per section with 899 triggers (duration 0.5–1 s).
- Use pulse 1006 on 1000 or 1 to beat-flash.

---

## 6. Editor layers and Z order

- **Editor layers** `20` and `61` are editor-only visibility filters; they have no gameplay or render effect. Use them to organize, e.g. 0 gameplay, 1 deco, 2 triggers.
- **Z layer (`24`)** [V-SRC: Geode `enum class ZLayer`; gddocs example `24,7` = T2]:

| Layer | B5 | B4 | B3 | B2 | B1 | (default) | T1 | T2 | T3 | T4 |
|---|---|---|---|---|---|---|---|---|---|---|
| Value | −5 | −3 | −1 | 1 | 3 | 0 | 5 | 7 | 9 | 11 |

  - 0 (or absent) means "use the object's built-in default layer" (`m_defaultZLayer`).
  - `PortalBack = 4` is internal: the back half of portals draws behind the player.
  - The player draws between B1 and T1, so objects on T1+ draw over the player.
- **Per-object default layers** are not documented in these sources [UNVERIFIED]. My understanding: solids and hazards default to B1 or below the player; glows and most deco default to B2 (behind blocks); portals are split back/front.
- **Recommendation:** omit 24 for gameplay objects. Set it explicitly for deco:
  - background deco: B3 or B4;
  - glows behind blocks: B2;
  - glow overlays and particles in front: T1;
  - UI/text: T2.
- **Z order (`25`)**: an int that orders objects within one layer (higher draws on top). GD gives some objects nonzero defaults (`m_defaultZOrder`). The editor range is about −100…100 [UNVERIFIED for 2.2].
- Blending channels render in separate (additive) batches. Additive-blended deco on dark BGs gives the "glow" look.

---

## 7. Hitbox and collision notes

Source: gd-sim `src/Objects/Object.cpp`, sizes in units before the object's own scale (`size *= 32 × 128/129`) [SRC: gd-sim, "validated against the game"].

- **Object type enum** (Geode `GameObjectType`):
  - 0 Solid, 2 Hazard, 3/4 inverse/normal gravity portal, 5 ship portal, 6 cube portal, 7 Decoration;
  - 8/9 yellow/pink pad, 10 gravity pad, 11/12/13 yellow/pink/gravity ring;
  - 14/15 mirror, 16 ball, 17/18 size, 19 UFO, 20 Modifier, 21 Breakable;
  - 22 SecretCoin, 23/24 dual/solo, 25 Slope, 26 wave, 27 robot, 28 teleport, 29 green ring;
  - 30 Collectible, 31 UserCoin, 32 DropRing (black), 33 spider portal, 34/35 red pad/ring;
  - 36 custom ring, 37 dash ring, 38 gravity dash ring, 39 collision object, 40 special;
  - 41 swing portal, 42 gravity toggle portal, 43/44 spider orb/pad, 45 enter effect, 46 teleport orb, 47 animated hazard.
  - The type is intrinsic to the ID; it is not stored in the string.
- **Solids:** the 30×30 IDs in 4.1, slabs 30×14 (40), 30×16 (62, 65, 66, 68), and so on.
  - Landing on top is safe.
  - Death happens when the player's **inner** box (0.3 × the outer box, 9×9 for a normal cube) intersects a solid [SRC: GDP via GD-Remake].
  - Cube step-up snapping exists, which makes one-block steps forgiving.
- **Hazards:** spike 8 is only **6×12** at the object center. A spike on the floor (y=15) spans x±3, y 9–21, which is far smaller than its sprite.
  - Saws use circles with the radii in 4.3. The 2.2 compat flag kA39 "fix radius collision" adds a circular player hitbox for these.
- **Passable / no-hitbox:** all deco (glows, pulsing, clouds, 18–21 deco spikes, fake spikes 191/198/199/393/1889–1892), "inner" block pieces (5, 73, 80, 120, 211, 251, 259, 266, 273), triggers.
  - Any object with `121,1` (no touch) or `134,1` (passable) is also passable.
- **Player sizes:**
  - cube/ship/ball/UFO/robot: 30×30;
  - wave: 10×10;
  - spider: 27 (OSGD);
  - mini: ×0.6.
- **Pads, orbs, portals** trigger on outer-box overlap:
  - pads 25×4–6;
  - orbs about 36×36 [UNVERIFIED];
  - game-mode portals 34×86, gravity 25×75, size 31×90, speed 35×44 (0.5x) / 33–69×56.
- **Level bounds:** above y ≈ 1476 is death per gd-sim [SRC: gd-sim; may not hold in 2.2 with kA37 dynamic height].
- **Level length** (end of a classic level) is derived from the last object's x. GD-Remake estimates +340 units [UNVERIFIED]. Use an End trigger (3600) for a deterministic end.
- **Editor object limit:** 80,000 objects without the high-object flag (`k69`) [UNVERIFIED for 2.2081].

---

## 8. C++ implementation checklist

1. **Build the inner string.** Header, then `;` + each object as `id,x,y,...`, then a trailing `;`.
   - Format floats with up to 4–6 significant decimals and trim trailing zeros. Never use exponent notation or a locale comma.
   - Join groups with `.`.
2. **Compress.** gzip it (zlib `deflateInit2` with windowBits 31), then base64url-encode with padding.
3. **Wrap in the plist.** `<?xml version="1.0"?><plist version="1.0" gjver="2.0"><dict>` … `</dict></plist>` with `kCEK=4, k2, k3(base64url), k4, k5, k13=<t />, k21=2, k16=1, k45 or k8, k50=45`. XML-escape the text fields.
4. **Read.** Parse `<k>/<s>/<i>/<r>/<t/>` tolerantly (short and long tag names), take `k4`, add padding, base64url-decode, then inflate with windowBits 47 (gzip or zlib auto-detect).
   - If `k4` already contains `;` or starts with `kS` or `kA`, treat it as raw.
   - Split on `;`. The first segment is the header if its first key is not numeric.
5. **Do not emit** legacy keys: 19, kS29–kS37, kA5, triggers 29/30.
6. **Always write `36,1` on triggers.** Add `62,1,87,1` when the trigger is spawn-only, and give it a group in 57.
