/** Payout math for every game, derived from config.ts. Pure functions (unit tested). */
import { CHICKEN, HOUSE_EDGE, KENO, PLINKO, TOWER, WHEEL } from '../config';
import { floor2 } from './util';

export type PlinkoRisk = keyof typeof PLINKO.risk;
export type KenoRisk = keyof typeof KENO.shapes;
export type WheelRisk = keyof typeof WHEEL.patterns;
export type TowerDifficulty = keyof typeof TOWER.difficulty;
export type ChickenDifficulty = keyof typeof CHICKEN.difficulty;

export const rtp = (game: keyof typeof HOUSE_EDGE) => 1 - HOUSE_EDGE[game];

// ---------------------------------------------------------------- helpers
export function binomialProbs(n: number): number[] {
  const out: number[] = [];
  let c = 1;
  for (let k = 0; k <= n; k++) {
    out.push(c / 2 ** n);
    c = (c * (n - k)) / (k + 1);
  }
  return out;
}

export function choose(n: number, k: number): number {
  if (k < 0 || k > n) return 0;
  let r = 1;
  for (let i = 1; i <= k; i++) r = (r * (n - k + i)) / i;
  return r;
}

/** Round to "friendly" casino numbers: 1000, 130, 26, 5.6, 0.45 */
export function niceNumber(v: number): number {
  if (v >= 100) {
    const m = 10 ** (Math.floor(Math.log10(v)) - 1);
    return Math.round(v / m) * m;
  }
  if (v >= 10) return Math.round(v);
  if (v >= 1) return Math.round(v * 10) / 10;
  return Math.round(v * 100) / 100;
}

// ---------------------------------------------------------------- plinko
const plinkoCache = new Map<string, number[]>();
export function plinkoTable(rows: number, risk: PlinkoRisk): number[] {
  const key = rows + risk;
  const hit = plinkoCache.get(key);
  if (hit) return hit;
  const { center, k0, kPerRow } = PLINKO.risk[risk];
  const p = binomialProbs(rows);
  const k = k0 + kPerRow * (rows - 8);
  const g = (d: number) => (Math.exp(k * d) - 1) / (Math.exp(k) - 1);
  const ds = p.map((_, i) => Math.abs(i - rows / 2) / (rows / 2));
  const target = rtp('plinko');
  const build = (s: number) => ds.map((d) => niceNumber(center + s * g(d)));
  const rtpOf = (t: number[]) => t.reduce((a, v, i) => a + v * p[i], 0);
  // binary search the largest scale whose rounded table stays at/below target RTP
  let lo = 0, hi = 1e6;
  for (let it = 0; it < 80; it++) {
    const mid = (lo + hi) / 2;
    if (rtpOf(build(mid)) > target) hi = mid;
    else lo = mid;
  }
  const table = build(lo);
  plinkoCache.set(key, table);
  return table;
}

export function plinkoRtp(rows: number, risk: PlinkoRisk) {
  const p = binomialProbs(rows);
  return plinkoTable(rows, risk).reduce((a, v, i) => a + v * p[i], 0);
}

// ---------------------------------------------------------------- keno
export function kenoHitProbs(picks: number): number[] {
  const { numbers, drawn } = KENO;
  const total = choose(numbers, drawn);
  const out: number[] = [];
  for (let h = 0; h <= picks; h++) out.push((choose(picks, h) * choose(numbers - picks, drawn - h)) / total);
  return out;
}

const kenoCache = new Map<string, number[]>();
export function kenoTable(risk: KenoRisk, picks: number): number[] {
  const key = risk + picks;
  const hit = kenoCache.get(key);
  if (hit) return hit;
  const shape = KENO.shapes[risk][picks] ?? [];
  const p = kenoHitProbs(picks);
  const raw = p.map((_, h) => shape[h] ?? 0);
  const ev = raw.reduce((a, v, h) => a + v * p[h], 0);
  const scale = ev > 0 ? rtp('keno') / ev : 0;
  const table = raw.map((v) => (v > 0 ? floor2(v * scale) : 0));
  kenoCache.set(key, table);
  return table;
}

// ---------------------------------------------------------------- wheel
export function wheelSegments(risk: WheelRisk, count: number): number[] {
  const r = rtp('wheel');
  const pattern = WHEEL.patterns[risk] as number[] | 'jackpot';
  if (pattern === 'jackpot') {
    const segs = new Array(count).fill(0);
    segs[0] = floor2(r * count);
    return segs;
  }
  const raw = Array.from({ length: count }, (_, i) => pattern[i % pattern.length]);
  const sum = raw.reduce((a, b) => a + b, 0);
  const scale = (r * count) / sum;
  return raw.map((v) => (v > 0 ? floor2(v * scale) : 0));
}

// ---------------------------------------------------------------- mines
export function minesMultiplier(mines: number, revealed: number, tiles = 25): number {
  if (revealed <= 0) return 1;
  let m = 1;
  for (let i = 0; i < revealed; i++) m *= (tiles - i) / (tiles - mines - i);
  return floor2(m * rtp('mines'));
}

// ---------------------------------------------------------------- tower
export function towerMultiplier(diff: TowerDifficulty, row: number): number {
  if (row <= 0) return 1;
  const { tiles, bombs } = TOWER.difficulty[diff];
  return floor2(rtp('tower') * (tiles / (tiles - bombs)) ** row);
}

// ---------------------------------------------------------------- chicken / road
export function chickenMultiplier(diff: ChickenDifficulty, lane: number): number {
  if (lane <= 0) return 1;
  const { hit } = CHICKEN.difficulty[diff];
  return floor2(rtp('chicken') / (1 - hit) ** lane);
}

// ---------------------------------------------------------------- hi-lo
/** rank: 1 (Ace) .. 13 (King) */
export function hiloChances(rank: number): { hi: number; lo: number; hiLabel: string; loLabel: string } {
  if (rank === 1) return { hi: 12 / 13, lo: 1 / 13, hiLabel: 'Higher', loLabel: 'Same' };
  if (rank === 13) return { hi: 1 / 13, lo: 12 / 13, hiLabel: 'Same', loLabel: 'Lower' };
  return { hi: (14 - rank) / 13, lo: rank / 13, hiLabel: 'Higher or Same', loLabel: 'Lower or Same' };
}
export function hiloStepMultiplier(p: number): number {
  return floor2(rtp('hilo') / p);
}
/** Did guessing `dir` on `from` win when the next card is `to`? */
export function hiloWins(from: number, to: number, dir: 'hi' | 'lo'): boolean {
  if (from === 1) return dir === 'hi' ? to > 1 : to === 1;
  if (from === 13) return dir === 'hi' ? to === 13 : to < 13;
  return dir === 'hi' ? to >= from : to <= from;
}

// ---------------------------------------------------------------- dice / limbo
export function diceMultiplier(chance: number): number {
  return floor2((100 * rtp('dice')) / chance) || 0;
}
export function limboChance(target: number): number {
  return (rtp('limbo') / target) * 100;
}
