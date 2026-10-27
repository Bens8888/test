import { describe, expect, it } from 'vitest';
import { generateFloats, sha256Hex } from '../src/engine/fair';
import { hmacSha256Bytes, sha256Bytes } from '../src/engine/sha256';
import { bytesToHex } from '../src/engine/util';
import {
  binomialProbs, chickenMultiplier, diceMultiplier, hiloChances, hiloStepMultiplier, hiloWins, kenoHitProbs, kenoTable,
  minesMultiplier, plinkoRtp, plinkoTable, towerMultiplier, wheelSegments,
} from '../src/engine/paytables';
import { crashPoint, diceRoll, drawUnique, kenoDraw, plinkoPath, rouletteNumber, ROULETTE_ORDER } from '../src/engine/outcomes';
import { CHICKEN, TOWER, WHEEL } from '../src/config';

const enc = new TextEncoder();

describe('provably fair RNG', () => {
  it('pure-JS SHA-256 matches known vectors', () => {
    expect(bytesToHex(sha256Bytes(enc.encode('')))).toBe('e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855');
    expect(bytesToHex(sha256Bytes(enc.encode('abc')))).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  });
  it('pure-JS HMAC matches WebCrypto', async () => {
    const key = await crypto.subtle.importKey('raw', enc.encode('server'), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
    const ref = new Uint8Array(await crypto.subtle.sign('HMAC', key, enc.encode('client:1:0')));
    expect(bytesToHex(hmacSha256Bytes(enc.encode('server'), enc.encode('client:1:0')))).toBe(bytesToHex(ref));
  });
  it('sha256Hex uses crypto.subtle and agrees with fallback', async () => {
    expect(await sha256Hex('abc')).toBe('ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
  });
  it('floats are deterministic, in [0,1) and differ by nonce', async () => {
    const a = await generateFloats('s', 'c', 0, 20);
    const b = await generateFloats('s', 'c', 0, 20);
    const c = await generateFloats('s', 'c', 1, 20);
    expect(a).toEqual(b);
    expect(a).not.toEqual(c);
    expect(a.every((f) => f >= 0 && f < 1)).toBe(true);
    expect(a.length).toBe(20);
  });
});

describe('outcomes', () => {
  it('dice roll range', () => {
    expect(diceRoll(0)).toBe(0);
    expect(diceRoll(0.999999999)).toBe(100);
  });
  it('roulette covers 0..36 and wheel order is a permutation', () => {
    expect(rouletteNumber(0)).toBe(0);
    expect(rouletteNumber(0.99999)).toBe(36);
    expect([...ROULETTE_ORDER].sort((a, b) => a - b)).toEqual(Array.from({ length: 37 }, (_, i) => i));
  });
  it('crash point has the right distribution', () => {
    expect(crashPoint(0)).toBe(1);
    // P(crash >= 2) should be ~0.495
    let n = 0;
    const N = 200000;
    for (let i = 0; i < N; i++) if (crashPoint((i + 0.5) / N) >= 2) n++;
    expect(n / N).toBeCloseTo(0.495, 2);
  });
  it('draws are unique', () => {
    const k = kenoDraw(Array.from({ length: 10 }, () => Math.random()));
    expect(new Set(k).size).toBe(10);
    expect(k.every((x) => x >= 1 && x <= 40)).toBe(true);
    expect(new Set(drawUnique(Array(24).fill(0.999), 25, 24)).size).toBe(24);
  });
  it('plinko bucket = number of rights', () => {
    expect(plinkoPath([0.1, 0.9, 0.9, 0.2], 4)).toEqual({ path: [0, 1, 1, 0], bucket: 2 });
  });
});

describe('payout tables stay within 1–3% house edge', () => {
  it('plinko', () => {
    for (const risk of ['low', 'medium', 'high'] as const) {
      for (let rows = 8; rows <= 16; rows++) {
        const rtp = plinkoRtp(rows, risk);
        expect(rtp, `${risk} ${rows}`).toBeGreaterThanOrEqual(0.97);
        expect(rtp, `${risk} ${rows}`).toBeLessThanOrEqual(0.99 + 1e-9);
        const t = plinkoTable(rows, risk);
        expect(t.length).toBe(rows + 1);
        expect(t).toEqual([...t].reverse()); // symmetric
      }
    }
    expect(binomialProbs(16).reduce((a, b) => a + b, 0)).toBeCloseTo(1, 10);
  });
  it('keno', () => {
    for (const risk of ['classic', 'low', 'medium', 'high'] as const) {
      for (let picks = 1; picks <= 10; picks++) {
        const p = kenoHitProbs(picks);
        const rtp = kenoTable(risk, picks).reduce((a, m, h) => a + m * p[h], 0);
        expect(rtp, `${risk} ${picks}`).toBeGreaterThanOrEqual(0.97);
        expect(rtp, `${risk} ${picks}`).toBeLessThanOrEqual(0.99 + 1e-9);
      }
    }
  });
  it('wheel', () => {
    for (const risk of ['low', 'medium', 'high'] as const) {
      for (const n of WHEEL.segmentOptions) {
        const segs = wheelSegments(risk, n);
        const rtp = segs.reduce((a, b) => a + b, 0) / n;
        expect(segs.length).toBe(n);
        expect(rtp, `${risk} ${n}`).toBeGreaterThanOrEqual(0.97);
        expect(rtp, `${risk} ${n}`).toBeLessThanOrEqual(0.99 + 1e-9);
      }
    }
  });
  it('dice', () => {
    expect(diceMultiplier(49.5)).toBe(2);
    expect(diceMultiplier(50) * 0.5).toBeCloseTo(0.99, 2);
  });
  it('mines', () => {
    // 1 mine, 1 pick: 25/24 * 0.99
    expect(minesMultiplier(1, 1)).toBe(1.03);
    // EV of cashing out after k picks = P(survive) * mult ≈ 0.99
    const mines = 5, k = 6;
    let p = 1;
    for (let i = 0; i < k; i++) p *= (25 - mines - i) / (25 - i);
    expect(p * minesMultiplier(mines, k)).toBeGreaterThan(0.98);
    expect(p * minesMultiplier(mines, k)).toBeLessThanOrEqual(0.99);
  });
  it('tower & road', () => {
    const { tiles, bombs } = TOWER.difficulty.medium;
    const ps = (tiles - bombs) / tiles;
    expect(ps ** 3 * towerMultiplier('medium', 3)).toBeCloseTo(0.98, 2);
    const { hit } = CHICKEN.difficulty.hard;
    expect((1 - hit) ** 4 * chickenMultiplier('hard', 4)).toBeCloseTo(0.98, 2);
  });
  it('hi-lo', () => {
    for (let r = 1; r <= 13; r++) {
      const c = hiloChances(r);
      let hiWins = 0, loWins = 0;
      for (let to = 1; to <= 13; to++) {
        if (hiloWins(r, to, 'hi')) hiWins++;
        if (hiloWins(r, to, 'lo')) loWins++;
      }
      expect(hiWins / 13).toBeCloseTo(c.hi, 10);
      expect(loWins / 13).toBeCloseTo(c.lo, 10);
      expect(c.hi * hiloStepMultiplier(c.hi)).toBeLessThanOrEqual(0.99 + 1e-9);
    }
  });
});
