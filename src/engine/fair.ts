/**
 * "Provably fair" style RNG (for fun — everything is play money).
 *
 *  - A secret server seed is generated locally; only its SHA-256 hash is shown.
 *  - You can set any client seed. Each bet increments the nonce.
 *  - Floats come from HMAC_SHA256(serverSeed, `${clientSeed}:${nonce}:${round}`),
 *    4 bytes per float: f = b0/256 + b1/256² + b2/256³ + b3/256⁴  ∈ [0, 1)
 *  - Rotating the seed reveals the old server seed so past bets can be verified.
 */
import { store } from './store';
import { bytesToHex, Emitter } from './util';
import { hmacSha256Bytes, sha256Bytes } from './sha256';

const enc = new TextEncoder();
const subtle: SubtleCrypto | undefined = (globalThis.crypto as Crypto | undefined)?.subtle;

export async function sha256Hex(text: string): Promise<string> {
  if (subtle) {
    const buf = await subtle.digest('SHA-256', enc.encode(text));
    return bytesToHex(new Uint8Array(buf));
  }
  return bytesToHex(sha256Bytes(enc.encode(text)));
}

const keyCache = new Map<string, Promise<CryptoKey>>();
async function hmac(key: string, msg: string): Promise<Uint8Array> {
  if (subtle) {
    let k = keyCache.get(key);
    if (!k) {
      k = subtle.importKey('raw', enc.encode(key), { name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
      keyCache.set(key, k);
      if (keyCache.size > 8) keyCache.delete(keyCache.keys().next().value!);
    }
    return new Uint8Array(await subtle.sign('HMAC', await k, enc.encode(msg)));
  }
  return hmacSha256Bytes(enc.encode(key), enc.encode(msg));
}

/** Deterministically derive `count` floats in [0,1) from the seed triple. */
export async function generateFloats(serverSeed: string, clientSeed: string, nonce: number, count: number): Promise<number[]> {
  const out: number[] = [];
  let round = 0;
  while (out.length < count) {
    const bytes = await hmac(serverSeed, `${clientSeed}:${nonce}:${round++}`);
    for (let i = 0; i + 4 <= bytes.length && out.length < count; i += 4) {
      out.push(bytes[i] / 256 + bytes[i + 1] / 256 ** 2 + bytes[i + 2] / 256 ** 3 + bytes[i + 3] / 256 ** 4);
    }
  }
  return out;
}

export function randomHex(bytes = 32): string {
  const a = new Uint8Array(bytes);
  crypto.getRandomValues(a);
  return bytesToHex(a);
}

function randomClientSeed() {
  return randomHex(8);
}

function ensure() {
  if (!store.data.fair) {
    store.data.fair = { serverSeed: randomHex(32), clientSeed: randomClientSeed(), nonce: 0 };
    store.touch();
  }
  return store.data.fair;
}

let hashCache: { seed: string; hash: Promise<string> } | null = null;

export interface FloatsResult {
  floats: number[];
  nonce: number;
  clientSeed: string;
  serverSeedHash: string;
}

export const fair = {
  changed: new Emitter<void>(),
  get state() {
    return ensure();
  },
  async serverSeedHash(): Promise<string> {
    const s = ensure();
    if (!hashCache || hashCache.seed !== s.serverSeed) hashCache = { seed: s.serverSeed, hash: sha256Hex(s.serverSeed) };
    return hashCache.hash;
  },
  /** Reserve the next nonce and derive floats for one bet. */
  async next(count: number): Promise<FloatsResult> {
    const s = ensure();
    const nonce = s.nonce++;
    const { serverSeed, clientSeed } = s;
    store.touch();
    this.changed.emit();
    const [floats, serverSeedHash] = await Promise.all([generateFloats(serverSeed, clientSeed, nonce, count), this.serverSeedHash()]);
    return { floats, nonce, clientSeed, serverSeedHash };
  },
  setClientSeed(seed: string) {
    const s = ensure();
    seed = seed.trim().slice(0, 64) || randomClientSeed();
    if (seed === s.clientSeed) return;
    this.rotate(seed);
  },
  /** Reveal the current server seed and start a fresh pair. */
  rotate(newClientSeed?: string) {
    const s = ensure();
    s.prev = { serverSeed: s.serverSeed, clientSeed: s.clientSeed, nonce: s.nonce };
    s.serverSeed = randomHex(32);
    s.clientSeed = newClientSeed ?? s.clientSeed;
    s.nonce = 0;
    store.touch();
    this.changed.emit();
  },
};
