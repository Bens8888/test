/**
 * Float → game outcome mappings. Shared by the games and the fairness verifier so a
 * revealed seed always reproduces exactly what you saw.
 */
import { CRASH, HOUSE_EDGE } from '../config';

export const ROULETTE_ORDER = [0, 32, 15, 19, 4, 21, 2, 25, 17, 34, 6, 27, 13, 36, 11, 30, 8, 23, 10, 5, 24, 16, 33, 1, 20, 14, 31, 9, 22, 18, 29, 7, 28, 12, 35, 3, 26];
export const RED_NUMBERS = new Set([1, 3, 5, 7, 9, 12, 14, 16, 18, 19, 21, 23, 25, 27, 30, 32, 34, 36]);

/** 0.00 – 100.00 */
export const diceRoll = (f: number) => Math.floor(f * 10001) / 100;

/** Crash-style multiplier with house edge; ≥ 1.00. P(result ≥ x) ≈ (1-edge)/x */
export function crashPoint(f: number, edge: number = HOUSE_EDGE.crash): number {
  const raw = (1 - edge) / (1 - f);
  return Math.min(CRASH.maxMultiplier, Math.max(1, Math.floor(raw * 100) / 100));
}
export const limboResult = (f: number) => crashPoint(f, HOUSE_EDGE.limbo);

/** 0 = left, 1 = right for each row; bucket = number of rights */
export function plinkoPath(floats: number[], rows: number): { path: number[]; bucket: number } {
  const path = floats.slice(0, rows).map((f) => (f < 0.5 ? 0 : 1));
  return { path, bucket: path.reduce<number>((a, b) => a + b, 0) };
}

export const rouletteNumber = (f: number) => Math.floor(f * 37);
export const wheelIndex = (f: number, segments: number) => Math.floor(f * segments);

/** Draw `count` unique values from [lo, lo+size) without replacement. */
export function drawUnique(floats: number[], size: number, count: number, lo = 0): number[] {
  const pool = Array.from({ length: size }, (_, i) => i + lo);
  const out: number[] = [];
  for (let i = 0; i < count; i++) out.push(pool.splice(Math.floor(floats[i] * pool.length), 1)[0]);
  return out;
}

/** 10 numbers between 1 and 40 */
export const kenoDraw = (floats: number[]) => drawUnique(floats, 40, 10, 1);

/** Mine tile indices 0..24 */
export const minesPositions = (floats: number[], mines: number) => drawUnique(floats, 25, mines, 0);

/** For each tower row the set of bomb columns */
export function towerBombs(floats: number[], rows: number, tiles: number, bombs: number): number[][] {
  const out: number[][] = [];
  for (let r = 0; r < rows; r++) out.push(drawUnique(floats.slice(r * bombs, r * bombs + bombs), tiles, bombs, 0));
  return out;
}

/** Index of the lane where the runner gets hit (lanes = never hit) */
export function chickenHitLane(floats: number[], hit: number, lanes: number): number {
  for (let i = 0; i < lanes; i++) if (floats[i] < hit) return i;
  return lanes;
}

export interface Card {
  rank: number; // 1..13 (A..K)
  suit: number; // 0..3 ♠ ♥ ♦ ♣
}
export const SUITS = ['♠', '♥', '♦', '♣'];
export const RANK_LABELS = ['', 'A', '2', '3', '4', '5', '6', '7', '8', '9', '10', 'J', 'Q', 'K'];
/** Infinite-deck card from a float */
export function cardFromFloat(f: number): Card {
  const idx = Math.floor(f * 52);
  return { rank: (idx % 13) + 1, suit: Math.floor(idx / 13) };
}

export const dieFromFloat = (f: number) => Math.floor(f * 6) + 1;
