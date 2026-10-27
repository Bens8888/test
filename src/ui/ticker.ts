/** Fake "live" activity: bots with made-up usernames placing play-money bets. */
import { BOT_NAMES } from '../config';
import { GAMES } from '../games/registry';
import { walletEvents } from '../engine/wallet';
import { Emitter, fmt, fmtMult, pick, rand } from '../engine/util';

export interface LiveBet {
  user: string;
  game: string; // id
  bet: number;
  mult: number;
  payout: number;
  you?: boolean;
  t: number;
}

export const liveFeed = new Emitter<LiveBet>();
export const recent: LiveBet[] = [];

function botBet(): LiveBet {
  const game = pick(GAMES).id;
  const bet = Math.round(Math.exp(rand(Math.log(1), Math.log(2500))) * 100) / 100;
  let mult = 0;
  const r = Math.random();
  if (r > 0.52) {
    // heavy tail: mostly 1.1–5x, sometimes huge
    mult = Math.min(5000, 0.99 / Math.max(0.0004, Math.random()));
    mult = Math.max(1.05, Math.round(mult * 100) / 100);
  }
  return { user: pick(BOT_NAMES), game, bet, mult, payout: Math.round(bet * mult * 100) / 100, t: Date.now() };
}

function push(b: LiveBet) {
  recent.unshift(b);
  if (recent.length > 30) recent.length = 30;
  liveFeed.emit(b);
}

let started = false;
export function startBots() {
  if (started) return;
  started = true;
  for (let i = 0; i < 12; i++) push({ ...botBet(), t: Date.now() - i * 4000 });
  const loop = () => {
    push(botBet());
    setTimeout(loop, rand(500, 1700));
  };
  setTimeout(loop, 800);
  walletEvents.settled.on((e) => push({ user: 'You', game: e.game, bet: e.bet, mult: e.mult, payout: e.payout, you: true, t: Date.now() }));
}

export function gameName(id: string) {
  return GAMES.find((g) => g.id === id)?.name ?? id;
}

export function describeWin(b: LiveBet) {
  return `${b.user} won ${fmt(b.payout)} on ${gameName(b.game)} (${fmtMult(b.mult)})`;
}
