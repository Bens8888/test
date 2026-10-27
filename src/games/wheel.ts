/** WHEEL — spin a neon prize wheel; choose risk and number of segments. */
import '../styles/games/wheel.css';
import type { GameMeta } from './registry';
import { WHEEL } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { wheelIndex } from '../engine/outcomes';
import { wheelSegments, type WheelRisk } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { easeOutQuart, fitCanvas, fmt, h, rafLoop, rand, round2 } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const TAU = Math.PI * 2;
const ZERO_COLOR = '#2a2745';
const PALETTE = ['#22e4ff', '#2dff8a', '#ffc94d', '#ff8a3d', '#ff3d9a', '#9b5cff', '#7affd4', '#ffffff'];

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Pick a risk level and how many segments the wheel has.',
      'Spin — the segment under the pointer is your multiplier. 0× segments lose the bet.',
      'Low risk: frequent small wins. High risk: one jackpot segment, everything else is 0×.',
      'Every wheel averages 0.99× per spin (1% house edge).',
    ],
    stageClass: 'wheel-stage',
  });

  let risk: WheelRisk = 'medium';
  let count = 30;
  let segs = wheelSegments(risk, count);
  let colors = new Map<number, string>();
  let angle = 0; // wheel rotation (rad)
  let spin: null | { from: number; to: number; t: number; T: number; resolve: () => void } = null;
  let lastIdx = -1;
  let flick = 0; // pointer deflection 0..1
  let litIdx = -1; // winning segment glow
  let litT = 0;
  let wheelImg: HTMLCanvasElement | null = null;

  // ---------------------------------------------------------------- DOM
  const canvas = h('canvas.stage-canvas') as HTMLCanvasElement;
  const ctx = canvas.getContext('2d')!;
  const hubMult = h('div.wheel-hub-mult', '—');
  const hub = h('div.wheel-hub', hubMult);
  const pointer = h('div.wheel-pointer');
  const wrap = h('div.wheel-wrap', canvas, pointer, hub);
  const legend = h('div.wheel-legend');
  shell.stage.append(wrap, legend);
  let size = 0;
  const offResize = fitCanvas(canvas, (w) => {
    size = w;
    wheelImg = null;
  });

  function rebuild() {
    segs = wheelSegments(risk, count);
    const distinct = [...new Set(segs)].sort((a, b) => a - b);
    colors = new Map();
    let k = 0;
    for (const m of distinct) colors.set(m, m === 0 ? ZERO_COLOR : PALETTE[k++ % PALETTE.length]);
    wheelImg = null;
    litIdx = -1;
    legend.replaceChildren(...distinct.map((m) => {
      const n = segs.filter((s) => s === m).length;
      return h('div.wheel-chip', { 'data-mult': String(m), style: { '--c': colors.get(m)! } as any },
        h('div.wc-mult', `${fmt(m)}×`),
        h('div.wc-prob', `${((n / count) * 100).toFixed(n / count < 0.1 ? 1 : 0)}%`),
      );
    }));
    updateProfit?.();
  }

  function renderWheel(px: number) {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const c = document.createElement('canvas');
    c.width = c.height = Math.round(px * dpr);
    const g = c.getContext('2d')!;
    g.scale(dpr, dpr);
    const R = px / 2;
    g.translate(R, R);
    const seg = TAU / count;
    const rOuter = R * 0.86, rInner = R * 0.36;
    for (let i = 0; i < count; i++) {
      const a0 = -Math.PI / 2 + i * seg, a1 = a0 + seg;
      const col = colors.get(segs[i])!;
      const grad = g.createRadialGradient(0, 0, rInner, 0, 0, rOuter);
      if (segs[i] === 0) {
        grad.addColorStop(0, '#1b1930');
        grad.addColorStop(1, i % 2 ? '#2f2c4d' : '#26233f');
      } else {
        grad.addColorStop(0, shade(col, -0.55));
        grad.addColorStop(0.75, col);
        grad.addColorStop(1, shade(col, 0.35));
      }
      g.fillStyle = grad;
      g.beginPath();
      g.arc(0, 0, rOuter, a0, a1);
      g.arc(0, 0, rInner, a1, a0, true);
      g.closePath();
      g.fill();
      g.strokeStyle = 'rgba(10, 6, 30, 0.85)';
      g.lineWidth = Math.max(1, R * 0.008);
      g.stroke();
      // label for non-zero segments when there's room
      if (segs[i] > 0 && count <= 30) {
        g.save();
        g.rotate(a0 + seg / 2 + Math.PI / 2);
        g.fillStyle = 'rgba(10, 5, 30, 0.85)';
        g.font = `800 ${Math.max(8, Math.min(R * 0.06, (seg * rOuter) * 0.36))}px Orbitron, sans-serif`;
        g.textAlign = 'center';
        g.textBaseline = 'middle';
        const m = segs[i];
        g.fillText(Number.isInteger(Math.round(m * 100) / 10) ? fmt(m, m >= 10 ? 0 : 1) : fmt(m, 2), 0, -rOuter * 0.82);
        g.restore();
      }
    }
    // outer rim
    const rim = g.createLinearGradient(-R, -R, R, R);
    rim.addColorStop(0, '#4a3a8f');
    rim.addColorStop(0.5, '#1b1442');
    rim.addColorStop(1, '#3a2a7a');
    g.strokeStyle = rim;
    g.lineWidth = R * 0.1;
    g.beginPath();
    g.arc(0, 0, R * 0.91, 0, TAU);
    g.stroke();
    g.strokeStyle = 'rgba(155, 92, 255, 0.9)';
    g.lineWidth = 2;
    g.beginPath();
    g.arc(0, 0, R * 0.86, 0, TAU);
    g.stroke();
    g.beginPath();
    g.arc(0, 0, R * 0.96, 0, TAU);
    g.stroke();
    // inner ring
    g.fillStyle = '#0d0a26';
    g.beginPath();
    g.arc(0, 0, rInner, 0, TAU);
    g.fill();
    g.strokeStyle = 'rgba(34, 228, 255, 0.6)';
    g.lineWidth = 2;
    g.stroke();
    return c;
  }

  const idxAtPointer = (a: number) => {
    const seg = TAU / count;
    const rel = (((-a) % TAU) + TAU) % TAU;
    return Math.floor(rel / seg) % count;
  };

  const stopLoop = rafLoop((dt, now) => {
    if (spin) {
      spin.t += dt;
      const u = Math.min(1, spin.t / spin.T);
      angle = spin.from + (spin.to - spin.from) * easeOutQuart(u);
      if (u >= 1) {
        const r = spin.resolve;
        spin = null;
        r();
      }
    } else if (litIdx < 0) {
      angle += dt * 0.05; // gentle idle drift
    }
    const idx = idxAtPointer(angle);
    if (idx !== lastIdx) {
      if (lastIdx !== -1 && spin) {
        flick = 1;
        sound.play('tick', 0.9 + Math.random() * 0.3);
      }
      lastIdx = idx;
    }
    flick = Math.max(0, flick - dt * 7);
    pointer.style.transform = `translateX(-50%) rotate(${-flick * 22}deg)`;
    draw(now);
  });

  function draw(now: number) {
    if (!size) return;
    if (!wheelImg) wheelImg = renderWheel(size);
    const R = size / 2;
    ctx.clearRect(0, 0, size, size);
    ctx.save();
    ctx.translate(R, R);
    ctx.rotate(angle);
    ctx.drawImage(wheelImg, -R, -R, size, size);
    // winning segment glow
    if (litIdx >= 0) {
      const seg = TAU / count;
      const a0 = -Math.PI / 2 + litIdx * seg;
      const pulse = 0.55 + 0.45 * Math.sin((now - litT) / 120);
      ctx.fillStyle = `rgba(255, 255, 255, ${0.18 + 0.2 * pulse})`;
      ctx.beginPath();
      ctx.arc(0, 0, R * 0.86, a0, a0 + seg);
      ctx.arc(0, 0, R * 0.36, a0 + seg, a0, true);
      ctx.closePath();
      ctx.fill();
    }
    ctx.restore();
    // rim lights (do not rotate) — chase while spinning, blink when idle
    const lights = 24;
    const spinning = !!spin;
    for (let i = 0; i < lights; i++) {
      const a = (i / lights) * TAU - Math.PI / 2;
      const x = R + Math.cos(a) * R * 0.91, y = R + Math.sin(a) * R * 0.91;
      let on: boolean;
      if (spinning) on = (i + Math.floor(now / 60)) % 4 === 0;
      else if (litIdx >= 0) on = Math.floor(now / 160) % 2 === i % 2;
      else on = (i + Math.floor(now / 500)) % 3 === 0;
      const col = litIdx >= 0 ? colors.get(segs[litIdx]) ?? '#fff' : i % 2 ? '#22e4ff' : '#ff3d9a';
      ctx.fillStyle = on ? col : 'rgba(255,255,255,0.18)';
      ctx.shadowColor = col;
      ctx.shadowBlur = on ? 12 : 0;
      ctx.beginPath();
      ctx.arc(x, y, Math.max(2, R * 0.018), 0, TAU);
      ctx.fill();
    }
    ctx.shadowBlur = 0;
    // glossy highlight
    const gl = ctx.createRadialGradient(R * 0.65, R * 0.45, R * 0.05, R, R, R);
    gl.addColorStop(0, 'rgba(255,255,255,0.12)');
    gl.addColorStop(0.6, 'rgba(255,255,255,0)');
    gl.addColorStop(1, 'rgba(0,0,0,0.2)');
    ctx.fillStyle = gl;
    ctx.beginPath();
    ctx.arc(R, R, R * 0.86, 0, TAU);
    ctx.fill();
  }

  function spinTo(idx: number, fast: boolean) {
    return new Promise<void>((resolve) => {
      const seg = TAU / count;
      // land with segment idx centre (± jitter) under the pointer at the top
      const targetMod = -((idx + 0.5) * seg + rand(-0.36, 0.36) * seg);
      const cur = angle;
      let delta = (((targetMod - cur) % TAU) + TAU) % TAU;
      delta += TAU * (fast ? 3 : 5);
      litIdx = -1;
      spin = { from: cur, to: cur + delta, t: 0, T: fast ? 1.5 : 3.1, resolve };
      sound.play('whoosh');
    });
  }

  // ---------------------------------------------------------------- play
  let busy = false;
  let alive = true;
  async function play(bet: number, fast = false): Promise<{ bet: number; payout: number } | null> {
    if (busy) return null;
    const snapSegs = segs;
    const snapCount = count;
    if (!wallet.placeBet(meta.id, bet)) return null;
    busy = true;
    lockSettings(true);
    legend.querySelectorAll('.wheel-chip').forEach((c) => c.classList.remove('hit'));
    hub.classList.remove('win', 'lose');
    hubMult.textContent = '…';
    try {
      const { floats } = await fair.next(1);
      const idx = wheelIndex(floats[0], snapCount);
      const mult = snapSegs[idx];
      if (alive) await spinTo(idx, fast || store.data.settings.reducedMotion);
      const payout = round2(bet * mult);
      litIdx = idx;
      litT = performance.now();
      hubMult.textContent = `${fmt(mult)}×`;
      hub.classList.add(mult > 0 ? 'win' : 'lose');
      hub.style.setProperty('--c', colors.get(mult) ?? '#fff');
      legend.querySelector(`.wheel-chip[data-mult="${mult}"]`)?.classList.add('hit');
      wallet.settle(meta.id, bet, payout, `${fmt(mult)}× segment`, mult);
      shell.addResult(`${fmt(mult)}×`, payout > bet ? 'win' : payout > 0 ? 'neutral' : 'lose');
      if (alive) {
        if (mult > 0) {
          fx.burstAt(hub, { count: 30, colors: [colors.get(mult)!, '#ffffff', '#ffc94d'] });
          fx.pop(shell.stage, `${fmt(mult)}×`, mult >= 10 ? 'big' : payout >= bet ? 'win' : 'neutral');
        }
        fx.celebrate(shell.stage, payout, bet, mult);
      }
      return { bet, payout };
    } finally {
      busy = false;
      if (!panel.isAuto) lockSettings(false);
    }
  }

  // ---------------------------------------------------------------- panel
  const riskSeg = h('div.seg', (['low', 'medium', 'high'] as WheelRisk[]).map((r) =>
    h(`button${r === risk ? '.active' : ''}`, { type: 'button', onclick: (e: Event) => {
      if (busy) return;
      risk = r;
      riskSeg.querySelectorAll('button').forEach((b) => b.classList.toggle('active', b === e.currentTarget));
      sound.play('click');
      rebuild();
    } }, r[0].toUpperCase() + r.slice(1))));
  const segSel = h('select', WHEEL.segmentOptions.map((n) => h('option', { value: String(n), selected: n === count }, `${n} segments`))) as HTMLSelectElement;
  segSel.addEventListener('change', () => {
    count = parseInt(segSel.value);
    sound.play('click');
    rebuild();
  });
  function lockSettings(on: boolean) {
    riskSeg.querySelectorAll('button').forEach((b) => ((b as HTMLButtonElement).disabled = on));
    segSel.disabled = on;
  }
  let updateProfit: (() => void) | undefined;
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Spin',
    onAction: (bet) => play(bet),
    autoRound: (bet) => play(bet, true),
    autoDelay: 300,
    onAutoChange: (on) => lockSettings(on),
  });
  updateProfit = () => panel.setProfitHint(`Max win: ${fmt(panel.bet * Math.max(...segs))}`);
  panel.onBetChange = updateProfit;
  panel.settings.append(
    h('div.field', h('label.field-label', 'Risk'), riskSeg),
    h('div.field', h('label.field-label', 'Segments'), segSel),
  );
  shell.controls.appendChild(panel.el);
  rebuild();

  return () => {
    alive = false;
    // finish an in-flight spin instantly so the bet always settles
    if (spin) {
      const r = spin.resolve;
      spin = null;
      r();
    }
    stopLoop();
    offResize();
    panel.destroy();
    shell.destroy();
  };
}

/** Lighten (amt > 0) or darken (amt < 0) a hex colour. */
function shade(hex: string, amt: number) {
  const n = parseInt(hex.slice(1), 16);
  let r = (n >> 16) & 255, g = (n >> 8) & 255, b = n & 255;
  const f = (c: number) => Math.round(amt >= 0 ? c + (255 - c) * amt : c * (1 + amt));
  r = f(r); g = f(g); b = f(b);
  return `rgb(${r}, ${g}, ${b})`;
}
