/** Procedural Web Audio sound effects — no audio files needed. */
import { store } from './store';

type SoundName =
  | 'click' | 'chip' | 'win' | 'bigwin' | 'lose' | 'tick' | 'roll' | 'pop' | 'gem' | 'boom'
  | 'card' | 'spin' | 'whoosh' | 'cashout' | 'step' | 'error' | 'land';

let ctx: AudioContext | null = null;
let master: GainNode | null = null;
let noiseBuf: AudioBuffer | null = null;

function ac(): AudioContext | null {
  if (!store.data.settings.sound) return null;
  if (!ctx) {
    const AC = window.AudioContext || (window as any).webkitAudioContext;
    if (!AC) return null;
    ctx = new AC();
    master = ctx.createGain();
    const comp = ctx.createDynamicsCompressor();
    comp.threshold.value = -14;
    master.connect(comp).connect(ctx.destination);
    noiseBuf = ctx.createBuffer(1, ctx.sampleRate * 0.5, ctx.sampleRate);
    const d = noiseBuf.getChannelData(0);
    for (let i = 0; i < d.length; i++) d[i] = Math.random() * 2 - 1;
  }
  if (ctx.state === 'suspended') ctx.resume();
  master!.gain.value = store.data.settings.volume * 0.5;
  return ctx;
}

function tone(freq: number, dur: number, opts: { type?: OscillatorType; vol?: number; delay?: number; slide?: number; attack?: number } = {}) {
  const c = ac();
  if (!c || !master) return;
  const t0 = c.currentTime + (opts.delay ?? 0);
  const o = c.createOscillator();
  const g = c.createGain();
  o.type = opts.type ?? 'sine';
  o.frequency.setValueAtTime(freq, t0);
  if (opts.slide) o.frequency.exponentialRampToValueAtTime(Math.max(20, freq * opts.slide), t0 + dur);
  const vol = opts.vol ?? 0.3;
  g.gain.setValueAtTime(0.0001, t0);
  g.gain.exponentialRampToValueAtTime(vol, t0 + (opts.attack ?? 0.005));
  g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
  o.connect(g).connect(master);
  o.start(t0);
  o.stop(t0 + dur + 0.02);
}

function noise(dur: number, opts: { freq?: number; q?: number; vol?: number; delay?: number; type?: BiquadFilterType; sweep?: number } = {}) {
  const c = ac();
  if (!c || !master || !noiseBuf) return;
  const t0 = c.currentTime + (opts.delay ?? 0);
  const src = c.createBufferSource();
  src.buffer = noiseBuf;
  const f = c.createBiquadFilter();
  f.type = opts.type ?? 'bandpass';
  f.frequency.setValueAtTime(opts.freq ?? 2000, t0);
  if (opts.sweep) f.frequency.exponentialRampToValueAtTime(opts.sweep, t0 + dur);
  f.Q.value = opts.q ?? 1;
  const g = c.createGain();
  const vol = opts.vol ?? 0.3;
  g.gain.setValueAtTime(vol, t0);
  g.gain.exponentialRampToValueAtTime(0.0001, t0 + dur);
  src.connect(f).connect(g).connect(master);
  src.start(t0);
  src.stop(t0 + dur + 0.02);
}

const lastPlayed: Partial<Record<SoundName, number>> = {};

export function play(name: SoundName, pitch = 1) {
  if (!store.data.settings.sound) return;
  // throttle rapid-fire sounds (plinko pins, ticks)
  const now = performance.now();
  const minGap = name === 'tick' || name === 'pop' ? 28 : 12;
  if (now - (lastPlayed[name] ?? 0) < minGap) return;
  lastPlayed[name] = now;
  switch (name) {
    case 'click':
      tone(880 * pitch, 0.05, { type: 'triangle', vol: 0.15 });
      break;
    case 'tick':
      tone(1800 * pitch, 0.025, { type: 'square', vol: 0.05 });
      break;
    case 'pop':
      tone(500 * pitch, 0.07, { type: 'sine', vol: 0.18, slide: 2.2 });
      break;
    case 'chip':
      noise(0.05, { freq: 5000, q: 3, vol: 0.25 });
      tone(2600 * pitch, 0.06, { type: 'triangle', vol: 0.12 });
      tone(3400 * pitch, 0.05, { type: 'triangle', vol: 0.08, delay: 0.035 });
      break;
    case 'card':
      noise(0.09, { freq: 3000, q: 0.7, vol: 0.28, type: 'highpass', sweep: 8000 });
      break;
    case 'roll':
      for (let i = 0; i < 6; i++) noise(0.04, { freq: 1200 + Math.random() * 1500, q: 4, vol: 0.2, delay: i * 0.06 + Math.random() * 0.02 });
      break;
    case 'land':
      tone(160 * pitch, 0.12, { type: 'sine', vol: 0.3, slide: 0.6 });
      noise(0.05, { freq: 1500, q: 2, vol: 0.15 });
      break;
    case 'spin':
      noise(0.03, { freq: 2500, q: 6, vol: 0.12 });
      break;
    case 'whoosh':
      noise(0.35, { freq: 400, q: 1.2, vol: 0.25, sweep: 3000 });
      break;
    case 'step':
      tone(420 * pitch, 0.08, { type: 'triangle', vol: 0.2, slide: 1.6 });
      break;
    case 'gem':
      [1318, 1760, 2637].forEach((f, i) => tone(f * pitch, 0.18, { type: 'sine', vol: 0.14, delay: i * 0.045 }));
      break;
    case 'win':
      [523, 659, 784, 1046].forEach((f, i) => tone(f * pitch, 0.22, { type: 'triangle', vol: 0.18, delay: i * 0.07 }));
      break;
    case 'cashout':
      [784, 1046, 1318].forEach((f, i) => tone(f, 0.25, { type: 'triangle', vol: 0.18, delay: i * 0.06 }));
      noise(0.2, { freq: 6000, q: 2, vol: 0.1, delay: 0.1 });
      break;
    case 'bigwin': {
      const notes = [523, 659, 784, 1046, 1318, 1568, 2093];
      notes.forEach((f, i) => tone(f, 0.35, { type: 'square', vol: 0.08, delay: i * 0.08 }));
      notes.forEach((f, i) => tone(f * 1.5, 0.4, { type: 'sine', vol: 0.1, delay: i * 0.08 + 0.04 }));
      for (let i = 0; i < 10; i++) tone(2000 + Math.random() * 3000, 0.1, { vol: 0.05, delay: 0.5 + i * 0.05 });
      break;
    }
    case 'lose':
      tone(330, 0.25, { type: 'sawtooth', vol: 0.1, slide: 0.5 });
      tone(247, 0.35, { type: 'sawtooth', vol: 0.08, delay: 0.12, slide: 0.5 });
      break;
    case 'boom':
      noise(0.7, { freq: 900, q: 0.6, vol: 0.5, type: 'lowpass', sweep: 60 });
      tone(90, 0.5, { type: 'sine', vol: 0.4, slide: 0.3 });
      break;
    case 'error':
      tone(200, 0.12, { type: 'square', vol: 0.1 });
      tone(160, 0.15, { type: 'square', vol: 0.1, delay: 0.1 });
      break;
  }
}

export const sound = { play };

/** Unlock audio on first user gesture (mobile Safari). */
window.addEventListener('pointerdown', () => ac(), { once: true });
