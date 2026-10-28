# Pattern model evaluation

All numbers measured on the development machine: Intel Xeon @ 2.30 GHz, 4 threads, CPU only
(no GPU), PyTorch 2.14.1, Python 3.13.

## Data (round 0)
`aldata selfplay --songs 300 --variants 6 --keep 0.34 --seed 1001`: 300 random synthetic songs,
1,800 bot-verified levels (3 failed the designed-input verification and were discarded), the best
third by reward kept: 897 sequences, 47,509 pattern decisions. Wall time 5 min 5 s.

## Training
`train_patterns.py --epochs 12 --per-style`: GRU (hidden 96, embedding 24, window 16), 66,294
parameters (259 KB per file), general model + 8 style fine-tunes. **Wall time 25.4 s.**

| | validation (89 unseen songs) |
|---|---|
| most frequent pattern per gamemode (baseline) | 8.7% top-1 |
| model | 18.8% top-1, 62.6% top-5, NLL 2.56 |

The targets are samples from the rule-based style model (stochastic by design), so top-1
accuracy is bounded well below 100%; what matters is the level quality below.

## Does the model make better levels? (rejection-sampling self-play)
Same held-out songs and seeds for every row; reward = music alignment + clicks that matter
- dropped clicks + difficulty following the music + variety - fairness risks (see `data::reward`).

| held-out set | rules | round-0 model | round-1 | round-2 |
|---|---|---|---|---|
| 30 songs x 3 variants (seed 777) | 1.273 | **1.386** | 1.356 | 1.321 |
| 40 songs x 3 variants (seed 4242) | 1.101 | **1.219** | - | - |

The round-0 model (trained on the best third of the rule generator's output) improves the reward
by about 9-11% at the same dropped-click rate (3.5%) and music alignment (~91%), all levels
beatable. Further self-play rounds (the model generating its own training data) did **not**
improve it further with this reward — they drift slightly — so the shipped models are round 0.
Real human levels (style learning) are what is expected to move the model further; see
training/README.md.
