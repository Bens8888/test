# Pattern model evaluation

All numbers measured on the development machine: Intel Xeon @ 2.30 GHz, 4 threads, CPU only
(no GPU), PyTorch 2.14.1, Python 3.13.

## Data (shipped models)
`aldata selfplay --songs 300 --variants 6 --keep 0.34 --seed 1001`: 300 random synthetic songs,
1,800 bot-verified levels (0 failed the designed-input verification), the best third by reward
kept: 900 sequences, 46,112 pattern decisions. Wall time 4 min 51 s.

## Training
`train_patterns.py --epochs 12 --per-style`: GRU (hidden 96, embedding 24, window 16), 66,294
parameters (259 KB per file), general model + 8 style fine-tunes. **Wall time 15.7 s.**
Report: [train_report_final.json](train_report_final.json).

| | validation (90 unseen songs) |
|---|---|
| most frequent pattern per gamemode (baseline) | 9.1% top-1 |
| model | 19.1% top-1, 62.3% top-5, NLL 2.58 |

The targets are samples from the rule-based style model (stochastic by design), so top-1
accuracy is bounded well below 100%; what matters is the level quality below.

PyTorch/C++ parity of the shipped `model_all.alm` (test `model_shipped_parity`): identical step
inputs, max logit difference 2.4e-6.

## Does the model make better levels? (rejection-sampling self-play)
Same held-out songs and seeds for every row; reward = music alignment + clicks that matter
- dropped clicks + difficulty following the music + variety - fairness risks (see `data::reward`).
Every level in every row was beaten by the bot with the designed clicks.

| held-out set | rules | shipped model | previous model (round 0) |
|---|---|---|---|
| 30 songs x 3 variants (seed 777) | 1.438 | **1.506** (+4.7%) | 1.497 |
| 40 songs x 3 variants (seed 4242) | 1.273 | **1.383** (+8.6%) | 1.382 |

Dropped clicks (2.7%) and music alignment (~91%) are the same with and without the model.

### History
- Round 0 (before the last simulator and generator fixes) improved the reward of the rules of
  that time by 9-11%. Since then the rule-based generator itself improved (the rules score
  1.44 / 1.27 now, up from 1.27 / 1.10), so the remaining gain from the model is smaller.
- Further self-play rounds (the model generating its own training data) did **not** improve the
  model with this reward; they drift slightly. The shipped models are one round on fresh data
  from the current generator: about as good as round 0, and consistent with the current
  simulator.

Real human levels (style learning) are what is expected to move the model further; see
training/RETRAINING.md.
