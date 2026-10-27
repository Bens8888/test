/** Play-chip wallet: balance, wagers, payouts, stats & per-game history. */
import { WALLET } from '../config';
import { store, type HistoryItem } from './store';
import { Emitter, round2 } from './util';

export interface SettleEvent {
  game: string;
  bet: number;
  payout: number;
  mult: number;
  profit: number;
  detail?: string;
}

export const walletEvents = {
  balance: new Emitter<{ from: number; to: number }>(),
  settled: new Emitter<SettleEvent>(),
  insufficient: new Emitter<number>(),
};

function setBalance(v: number) {
  const from = store.data.balance;
  store.data.balance = Math.max(0, round2(v));
  walletEvents.balance.emit({ from, to: store.data.balance });
  store.touch();
}

export const wallet = {
  get balance() {
    return store.data.balance;
  },
  canAfford(amount: number) {
    return amount > 0 && amount <= store.data.balance + 1e-9;
  },
  /** Validates and clamps a bet amount for UI inputs. */
  normalize(amount: number) {
    if (!Number.isFinite(amount) || amount < 0) return 0;
    return Math.min(round2(amount), WALLET.maxBet);
  },
  /**
   * Take a wager from the balance. Returns false (and emits `insufficient`) if the
   * player can't cover it. Counts towards "total wagered".
   */
  placeBet(game: string, amount: number): boolean {
    amount = round2(amount);
    if (amount < WALLET.minBet || amount > WALLET.maxBet || !this.canAfford(amount)) {
      walletEvents.insufficient.emit(amount);
      return false;
    }
    setBalance(store.data.balance - amount);
    const s = store.data.stats;
    s.wagered = round2(s.wagered + amount);
    const g = store.gameStats(game);
    g.wagered = round2(g.wagered + amount);
    return true;
  },
  /** Add more to an existing wager (double down, split, craps odds…). */
  addToBet(game: string, amount: number): boolean {
    return this.placeBet(game, amount);
  },
  /** Give back a wager that never resolved (e.g. leaving a table mid-round). */
  refund(game: string, amount: number) {
    amount = round2(amount);
    if (amount <= 0) return;
    setBalance(store.data.balance + amount);
    const s = store.data.stats;
    s.wagered = round2(Math.max(0, s.wagered - amount));
    const g = store.gameStats(game);
    g.wagered = round2(Math.max(0, g.wagered - amount));
  },
  /**
   * Resolve a round: credits `payout` (total returned incl. stake, 0 for a loss) and
   * records stats/history. `bet` is the total stake of the round.
   */
  settle(game: string, bet: number, payout: number, detail?: string, multOverride?: number): SettleEvent {
    bet = round2(bet);
    payout = round2(Math.min(payout, WALLET.maxPayout));
    const mult = multOverride ?? (bet > 0 ? payout / bet : 0);
    if (payout > 0) setBalance(store.data.balance + payout);
    const s = store.data.stats;
    const g = store.gameStats(game);
    const win = payout > bet + 1e-9;
    const push = Math.abs(payout - bet) < 1e-9;
    s.bets++;
    g.bets++;
    s.won = round2(s.won + payout);
    g.won = round2(g.won + payout);
    if (win) {
      s.wins++;
      g.wins++;
      s.streak = s.streak > 0 ? s.streak + 1 : 1;
      s.bestWinStreak = Math.max(s.bestWinStreak, s.streak);
    } else if (!push) {
      s.losses++;
      s.streak = s.streak < 0 ? s.streak - 1 : -1;
      s.worstLossStreak = Math.max(s.worstLossStreak, -s.streak);
    }
    const profit = round2(payout - bet);
    if (profit > s.biggestWin.amount) s.biggestWin = { amount: profit, game, mult };
    if (win && mult > s.biggestMult.mult) s.biggestMult = { mult, game };
    if (profit > g.biggestWin) g.biggestWin = profit;
    if (win && mult > g.biggestMult) g.biggestMult = mult;
    const item: HistoryItem = { t: Date.now(), bet, payout, mult: round2(mult), detail };
    g.history.unshift(item);
    if (g.history.length > 40) g.history.length = 40;
    store.touch();
    const ev = { game, bet, payout, mult, profit, detail };
    walletEvents.settled.emit(ev);
    return ev;
  },
  canRefill() {
    return store.data.balance < WALLET.refillThreshold;
  },
  refill() {
    if (!this.canRefill()) return false;
    store.data.stats.refills++;
    setBalance(store.data.balance + WALLET.refillAmount);
    return true;
  },
};
