/** Juice: particles, confetti, screen shake, count-ups, flying chips & multiplier pops. */
import { store } from './store';
import { sound } from './sound';
import { easeOutCubic, fmt, fmtMult, h, rand } from './util';

interface Particle {
  x: number; y: number; vx: number; vy: number; life: number; max: number;
  size: number; color: string; kind: 'spark' | 'confetti' | 'coin' | 'ring';
  rot: number; vr: number; g: number; drag: number;
}

export interface BurstOpts { count?: number; colors?: string[]; speed?: number; size?: number; ring?: boolean }

const NEON = ['#ff3d9a', '#22e4ff', '#9b5cff', '#2dff8a', '#ffc94d', '#ffffff'];

let canvas: HTMLCanvasElement | null = null;
let cctx: CanvasRenderingContext2D | null = null;
let parts: Particle[] = [];
let running = false;

function intensity(): number {
  const s = store.data.settings;
  const base = s.intensity === 'low' ? 0.4 : s.intensity === 'high' ? 1.6 : 1;
  return s.reducedMotion ? base * 0.3 : base;
}
export const reduced = () => store.data.settings.reducedMotion;

function ensureCanvas() {
  if (canvas) return;
  canvas = h('canvas.fx-layer') as HTMLCanvasElement;
  document.body.appendChild(canvas);
  cctx = canvas.getContext('2d');
  const resize = () => {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    canvas!.width = innerWidth * dpr;
    canvas!.height = innerHeight * dpr;
    cctx!.setTransform(dpr, 0, 0, dpr, 0, 0);
  };
  addEventListener('resize', resize);
  resize();
}

function loop() {
  if (!cctx || !canvas) return;
  let last = performance.now();
  running = true;
  const step = (now: number) => {
    const dt = Math.min(0.04, (now - last) / 1000);
    last = now;
    cctx!.clearRect(0, 0, innerWidth, innerHeight);
    parts = parts.filter((p) => (p.life -= dt) > 0);
    for (const p of parts) {
      p.vx *= 1 - p.drag * dt;
      p.vy = p.vy * (1 - p.drag * dt) + p.g * dt;
      p.x += p.vx * dt;
      p.y += p.vy * dt;
      p.rot += p.vr * dt;
      const a = Math.min(1, p.life / (p.max * 0.4));
      cctx!.globalAlpha = a;
      cctx!.save();
      cctx!.translate(p.x, p.y);
      cctx!.rotate(p.rot);
      if (p.kind === 'confetti') {
        cctx!.fillStyle = p.color;
        cctx!.fillRect(-p.size / 2, -p.size / 4, p.size, p.size / 2 * Math.abs(Math.cos(p.rot * 3)) + 1);
      } else if (p.kind === 'coin') {
        const sx = Math.abs(Math.cos(p.rot * 2));
        cctx!.scale(sx + 0.15, 1);
        const g = cctx!.createRadialGradient(-p.size * 0.3, -p.size * 0.3, 1, 0, 0, p.size);
        g.addColorStop(0, '#fff6c2');
        g.addColorStop(0.5, '#ffc94d');
        g.addColorStop(1, '#b8740a');
        cctx!.fillStyle = g;
        cctx!.beginPath();
        cctx!.arc(0, 0, p.size, 0, Math.PI * 2);
        cctx!.fill();
      } else if (p.kind === 'ring') {
        const t = 1 - p.life / p.max;
        cctx!.strokeStyle = p.color;
        cctx!.lineWidth = 3 * (1 - t) + 0.5;
        cctx!.beginPath();
        cctx!.arc(0, 0, p.size * (0.2 + easeOutCubic(t) * 1.2), 0, Math.PI * 2);
        cctx!.stroke();
      } else {
        cctx!.fillStyle = p.color;
        cctx!.shadowColor = p.color;
        cctx!.shadowBlur = 10;
        cctx!.beginPath();
        cctx!.arc(0, 0, p.size * (0.4 + 0.6 * a), 0, Math.PI * 2);
        cctx!.fill();
      }
      cctx!.restore();
    }
    cctx!.globalAlpha = 1;
    if (parts.length) requestAnimationFrame(step);
    else running = false;
  };
  requestAnimationFrame(step);
}

function add(p: Partial<Particle> & { x: number; y: number }) {
  ensureCanvas();
  if (parts.length > 900) return;
  parts.push({
    vx: 0, vy: 0, life: 1, max: 1, size: 4, color: '#fff', kind: 'spark', rot: 0, vr: 0, g: 600, drag: 0.6,
    ...p,
  } as Particle);
}
function kick() {
  if (!running) loop();
}

/** Coordinates of the centre of an element (viewport space). */
export function centerOf(el: Element) {
  const r = el.getBoundingClientRect();
  return { x: r.left + r.width / 2, y: r.top + r.height / 2 };
}

export const fx: {
  burst(x: number, y: number, opts?: BurstOpts): void;
  burstAt(el: Element, opts?: BurstOpts): void;
  confetti(count?: number): void;
  coinShower(count?: number): void;
  shake(el?: HTMLElement, strength?: number): void;
  flash(el: HTMLElement, kind?: 'win' | 'lose' | 'big'): void;
  pop(el: HTMLElement, text: string, kind?: 'win' | 'lose' | 'neutral' | 'big'): void;
  countUp(el: HTMLElement, from: number, to: number, opts?: { duration?: number; format?: (v: number) => string }): void;
  flyChip(from: Element, to: Element, color?: string, label?: string): void;
  celebrate(stage: HTMLElement, payout: number, bet: number, mult: number): 'lose' | 'win' | 'big' | 'huge';
  bigWinBanner(payout: number, mult: number): void;
} = {
  /** Spark burst at viewport point. */
  burst(x: number, y: number, opts: BurstOpts = {}) {
    const n = Math.round((opts.count ?? 24) * intensity());
    const colors = opts.colors ?? NEON;
    for (let i = 0; i < n; i++) {
      const a = Math.random() * Math.PI * 2;
      const sp = rand(0.3, 1) * (opts.speed ?? 380);
      const life = rand(0.4, 0.9);
      add({ x, y, vx: Math.cos(a) * sp, vy: Math.sin(a) * sp, life, max: life, size: rand(1.5, opts.size ?? 3.5), color: colors[i % colors.length], g: 500, drag: 2 });
    }
    if (opts.ring !== false && !reduced()) add({ x, y, kind: 'ring', life: 0.6, max: 0.6, size: 70, color: colors[0], g: 0 });
    kick();
  },
  burstAt(el: Element, opts?: BurstOpts) {
    const c = centerOf(el);
    this.burst(c.x, c.y, opts);
  },
  confetti(count = 140) {
    const n = Math.round(count * intensity());
    for (let i = 0; i < n; i++) {
      const fromLeft = i % 2 === 0;
      const life = rand(2, 3.4);
      add({
        x: fromLeft ? -10 : innerWidth + 10,
        y: innerHeight * rand(0.45, 0.8),
        vx: (fromLeft ? 1 : -1) * rand(250, 750),
        vy: -rand(500, 1050),
        life, max: life, size: rand(7, 13), color: NEON[i % NEON.length], kind: 'confetti',
        rot: rand(0, 6), vr: rand(-12, 12), g: 900, drag: 1.4,
      });
    }
    kick();
  },
  coinShower(count = 50) {
    const n = Math.round(count * intensity());
    for (let i = 0; i < n; i++) {
      const life = rand(1.6, 2.6);
      add({ x: rand(0, innerWidth), y: -rand(10, 300), vx: rand(-60, 60), vy: rand(100, 300), life, max: life, size: rand(6, 11), kind: 'coin', rot: rand(0, 6), vr: rand(-8, 8), g: 700, drag: 0.3 });
    }
    kick();
  },
  shake(el: HTMLElement = document.body, strength = 1) {
    if (reduced()) return;
    const s = Math.min(2.5, strength * intensity());
    el.style.setProperty('--shake', `${6 * s}px`);
    el.classList.remove('shake');
    void el.offsetWidth;
    el.classList.add('shake');
    setTimeout(() => el.classList.remove('shake'), 520);
  },
  /** Brief coloured glow over an element. */
  flash(el: HTMLElement, kind: 'win' | 'lose' | 'big' = 'win') {
    el.classList.remove('flash-win', 'flash-lose', 'flash-big');
    void el.offsetWidth;
    el.classList.add(`flash-${kind}`);
    setTimeout(() => el.classList.remove(`flash-${kind}`), 900);
  },
  /** Floating "+1,234.00" / "12.00×" pop that rises and fades. */
  pop(el: HTMLElement, text: string, kind: 'win' | 'lose' | 'neutral' | 'big' = 'win') {
    const host = el;
    if (getComputedStyle(host).position === 'static') host.style.position = 'relative';
    const p = h(`div.mult-pop.${kind}`, text);
    host.appendChild(p);
    setTimeout(() => p.remove(), 1500);
  },
  /** Animate a number in an element. */
  countUp(el: HTMLElement, from: number, to: number, opts: { duration?: number; format?: (v: number) => string } = {}) {
    const format = opts.format ?? ((v: number) => fmt(v));
    const dur = reduced() ? 0 : (opts.duration ?? 600);
    const prev = (el as any)._countRaf;
    if (prev) cancelAnimationFrame(prev);
    if (dur <= 0 || from === to) {
      el.textContent = format(to);
      return;
    }
    const t0 = performance.now();
    const step = (now: number) => {
      const t = Math.min(1, (now - t0) / dur);
      el.textContent = format(from + (to - from) * easeOutCubic(t));
      if (t < 1) (el as any)._countRaf = requestAnimationFrame(step);
      else (el as any)._countRaf = 0;
    };
    (el as any)._countRaf = requestAnimationFrame(step);
  },
  /** Fly a chip graphic from one element to another. */
  flyChip(from: Element, to: Element, color = '#ffc94d', label = '') {
    if (reduced()) return;
    const a = centerOf(from), b = centerOf(to);
    const chip = h('div.fly-chip', { style: { background: color } }, label);
    document.body.appendChild(chip);
    const midX = (a.x + b.x) / 2, midY = Math.min(a.y, b.y) - 80;
    const t0 = performance.now(), dur = 420;
    const step = (now: number) => {
      const t = Math.min(1, (now - t0) / dur);
      const e = easeOutCubic(t);
      const x = (1 - e) ** 2 * a.x + 2 * (1 - e) * e * midX + e * e * b.x;
      const y = (1 - e) ** 2 * a.y + 2 * (1 - e) * e * midY + e * e * b.y;
      chip.style.transform = `translate(${x - 14}px, ${y - 14}px) rotate(${t * 360}deg) scale(${1 - t * 0.2})`;
      if (t < 1) requestAnimationFrame(step);
      else chip.remove();
    };
    requestAnimationFrame(step);
  },
  /**
   * Standard celebration for a settled round, scaled by multiplier.
   * Returns the tier so games can add their own flourish.
   */
  celebrate(stage: HTMLElement, payout: number, bet: number, mult: number): 'lose' | 'win' | 'big' | 'huge' {
    const profit = payout - bet;
    if (payout <= 0 || profit < 0) {
      sound.play('lose');
      fx.flash(stage, 'lose');
      return 'lose';
    }
    if (mult >= 10 || (profit >= 5000 && mult >= 3)) {
      sound.play('bigwin');
      fx.flash(stage, 'big');
      fx.shake(document.querySelector('.app-main') as HTMLElement, mult >= 50 ? 2 : 1.2);
      fx.confetti(mult >= 50 ? 220 : 130);
      if (mult >= 50) fx.coinShower(70);
      fx.bigWinBanner(payout, mult);
      return mult >= 50 ? 'huge' : 'big';
    }
    sound.play('win');
    fx.flash(stage, 'win');
    fx.burstAt(stage, { count: 30, colors: ['#2dff8a', '#22e4ff', '#ffffff'] });
    return 'win';
  },
  bigWinBanner(payout: number, mult: number) {
    const amt = h('div.bw-amount', '0.00');
    const el = h('div.bigwin-banner', h('div.bw-title', mult >= 50 ? 'MEGA WIN' : 'BIG WIN'), amt, h('div.bw-mult', fmtMult(mult)));
    document.body.appendChild(el);
    fx.countUp(amt, 0, payout, { duration: 1200 });
    setTimeout(() => el.classList.add('out'), 2200);
    setTimeout(() => el.remove(), 2800);
    el.addEventListener('click', () => el.remove());
  },
};

/** Lightweight toast notifications. */
export function toast(msg: string, kind: 'info' | 'error' | 'success' = 'info') {
  let host = document.querySelector('.toasts');
  if (!host) {
    host = h('div.toasts');
    document.body.appendChild(host);
  }
  const t = h(`div.toast.${kind}`, msg);
  host.appendChild(t);
  setTimeout(() => t.classList.add('out'), 2600);
  setTimeout(() => t.remove(), 3000);
}
