# NEON VAULT — free play-money arcade

A browser-based neon casino arcade with 13 original games. It's **100% free and for fun**: only fake chips, no real money, no crypto, no deposits or withdrawals, no accounts, no ads and no links to gambling sites.

## Run it

```bash
npm install
npm run dev        # http://localhost:5173
```

Other scripts:

| Command | What it does |
| --- | --- |
| `npm run build` | Type-check and build a static site into `dist/` (host it anywhere) |
| `npm run preview` | Serve the production build |
| `npm test` | Unit tests for the RNG, outcomes and payout tables (RTP checks) |
| `npm run typecheck` | TypeScript only |

To play on your phone, run `npm run dev` and open the "Network" URL that Vite prints. That's plain http over your LAN, which browsers don't treat as a secure context, so the app uses a built-in pure-JS SHA-256 instead of `crypto.subtle` there. Results are identical either way.

## Games

| Game | Notes |
| --- | --- |
| Dice | Over/under slider, linked multiplier / target / win-chance inputs |
| Plinko | 8–16 rows, low/medium/high risk, many balls at once |
| Roulette | European single zero, full table (straight, split, street, corner, six line, dozens, columns, outside), undo / clear / rebet / 2× |
| Crash | Live rounds, curve on a canvas, manual and auto cash-out, bot players |
| Mines | 5×5 grid, 1–24 mines, cash out any time |
| Limbo | Target multiplier with a big count-up |
| Blackjack | Hit / stand / double / split, 3:2 blackjack, dealer stands on soft 17 |
| Craps | Pass / don't pass, come / don't come, odds, field and place bets, real point rules |
| Hi-Lo | Card streak with a multiplier chain |
| Keno | 40 numbers, pick up to 10, 4 risk tables |
| Wheel | 10–50 segments, 3 risk levels |
| Tower | 9 rows, 5 difficulties |
| Road Rush | Original cross-the-road game starring **Bolt** the robo-bunny |

Every game has a bet input with ½ / 2× / Max, plus an **Auto** mode with a bet count, on-win and on-loss bet changes, stop-on-profit and stop-on-loss. Press **Space** to play.

## Core systems

- **Wallet**: 10,000 starting chips and a free refill whenever you hit 0. Balance, stats (total wagered, biggest win, streaks) and per-game history are saved in `localStorage`.
- **"Provably fair" panel** (just for fun): you see the SHA-256 hash of the server seed, can edit the client seed, and each bet uses a nonce. Rotating the pair reveals the old server seed, and the verifier re-computes outcomes with `crypto.subtle` (HMAC-SHA256).
- **Juice**: particle bursts, confetti, coin showers, screen shake, count-ups, flying chips, multiplier pops and big-win banners. Sound is generated with the Web Audio API (no audio files) and has a mute toggle.
- **Settings**: sound, volume, animation intensity, reduced motion, break-reminder interval and a reset button.
- **Wellbeing**: a welcome note that nothing here is real gambling, a friendly "take a break" reminder after long sessions (30 minutes by default), and a "Play money only – no real prizes" badge in the footer.

## Tuning

All payouts and house edges live in **`src/config.ts`**:

- `HOUSE_EDGE` for each original game (1% by default, 2% for Tower and Road Rush).
- Plinko, Keno and Wheel tables are generated from shape parameters, then scaled to `1 - houseEdge` RTP and rounded to friendly numbers (see `src/engine/paytables.ts`). `npm test` checks that every table stays within a 1–3% edge.
- Roulette, Craps and Blackjack use the standard real-world payouts in the config.

## Structure

```
src/
  config.ts            payouts, house edges, limits, bot names
  main.ts              entry
  engine/              shared systems
    fair.ts            seeds, nonce, HMAC-SHA256 floats (crypto.subtle + fallback)
    sha256.ts          pure-JS SHA-256 / HMAC fallback
    outcomes.ts        float → game result (shared with the verifier)
    paytables.ts       multipliers / tables for every game
    wallet.ts          balance, bets, payouts, stats
    store.ts           localStorage persistence
    sound.ts           Web Audio synth
    fx.ts              particles, confetti, shake, count-up, flying chips
    betPanel.ts        bet controls + auto-bet
    util.ts            DOM / math helpers
  ui/                  app shell: top bar, sidebar, lobby, ticker, modals, fairness, settings
  games/               one file per game + registry.ts (lobby metadata, lazy loading)
  styles/              global + per-game CSS
tests/                 vitest unit tests
```

Play money only – no real prizes. Chips have no value and can't be bought, sold or cashed out.
