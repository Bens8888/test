# Retraining the pattern models

The models choose which valid pattern fits each musical moment (see docs/DECISIONS.md D8-D10).
Retraining changes the *style* of the layouts. Correctness never depends on the model; the
simulator checks every placement.

## 1. Setup (once)

- Python 3.10+ with `numpy` and `torch`. A CPU is enough; training takes seconds to minutes.
  ```bash
  pip install numpy torch
  ```
- Build the core tools (no Geode needed):
  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DAL_CORE_ONLY=ON && cmake --build build -j
  ```

## 2. Data

**Self-play** (no files needed): random synthetic songs, the bot-verified best third kept.
```bash
./build/tools/aldata selfplay --out training/data/selfplay.jsonl --songs 300 --variants 6 --keep 0.34 --seed 1001
```
300 songs take about 5 minutes on a 4-thread laptop CPU (1,800 levels).

**Real levels** (style learning): 60 levels (30 minimum, 100 better) with their songs. Spread
them roughly like this: 10 Easy/Normal, 15 Hard/Harder, 15 Insane and 20 demons. Collect them with
**Training → Add this level** in the mod, or export them with GDShare into this layout:
```
dataset/levels/<difficulty>/<Name>.gmd     difficulty: easy, normal, hard, harder, insane,
dataset/levels/<difficulty>/<Name>.mp3       easy_demon, medium_demon, hard_demon, insane_demon, extreme_demon
dataset/levels/<difficulty>/<Name>.json    optional: {"difficulty": 7, "style": "flow"}
```
Then extract them:
```bash
./build/tools/aldata extract-dir path/to/dataset/levels --out training/data/real.jsonl
```
Each level goes through the same steps:
1. It is filtered: platformer and 2-player levels, too short, a missing or wrong song, or a
   level the bot cannot beat are rejected with the reason printed.
2. The bot solves it.
3. Every jump's surrounding geometry is labelled with the closest pattern (labeller accuracy on
   known levels: 80% exact, 95% terrain).

## 3. Train

```bash
./build/tools/aldata vocab training/models/vocab.json
python3 training/train_patterns.py --data training/data/selfplay.jsonl training/data/real.jsonl \
    --vocab training/models/vocab.json --out training/models/mine --epochs 12 --per-style
```
- Real levels are few next to self-play data. To weight them up, repeat the file on the command
  line (for example `real.jsonl real.jsonl real.jsonl`).
- Augmentation runs automatically: tempo shift of the click gaps, intensity jitter, random crops.
- Output:
  - `model_all.alm`, plus `model_<style>.alm` for every style with at least 20 sequences;
  - `parity_*.json`, test vectors;
  - `train_report_*.json`, with data sizes, validation accuracy, hardware and wall time.

The reference run (training/models/EVALUATION.md) took **15.7 s** for the general model and 8
style fine-tunes, on an Intel Xeon @ 2.30 GHz with 4 threads, CPU only.

## 4. Check that the new model is better (optional, recommended)

Score levels on held-out songs, once with the rules and once with your model, using the same seed:
```bash
./build/tools/aldata selfplay --out /tmp/e.jsonl --songs 40 --variants 3 --keep 0.01 --seed 4242
./build/tools/aldata selfplay --out /tmp/e.jsonl --songs 40 --variants 3 --keep 0.01 --seed 4242 --model training/models/mine/model_all.alm
```
Compare `mean_reward` and `mean_dropped` in the `SUMMARY` lines. For several self-play rounds
with automatic evaluation, use `python3 training/selfplay_loop.py --rounds 2`.

## 5. Use it in the mod

Copy the `.alm` files into the mod's save folder under `models/` (**Settings → Save folder**),
then press **Training → Reload models**. Models in the save folder override the shipped ones.
To ship them with a build instead, replace `resources/models/*.alm`.

## Changing the model

Edit `training/autolayout/pattern_model.py` (Python) and `core/src/model/model.cpp` (C++)
together. The file format and input layout must match exactly. Then refresh the parity fixture,
which `tests/test_model.cpp` checks on every build:
```bash
python3 training/make_test_fixture.py --vocab training/models/vocab.json
```
If the pattern library changes, the vocabulary changes too and every model must be retrained (the
mod refuses models whose vocabulary size does not match).
