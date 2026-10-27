/** PLINKO — drop balls through a peg pyramid into glowing multiplier buckets. */
import type { GameMeta } from './registry';
import { PLINKO } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { plinkoPath } from '../engine/outcomes';
import { plinkoRtp, plinkoTable, type PlinkoRisk } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fitCanvas, fmt, h, rafLoop, rand, round2 } from '../engine/util';
import { gameShell } from '../ui/gameShell';

// Board geometry is in "units" (1 unit = distance between rows) so it scales cleanly.
const G = 118; // gravity, units/s²
const PIN_R = 0.1;
const BALL_R = 0.23;

interface Segment { x0: number; y0: number; x1: number; y1: number; vy0: number; T: number; pin?: [number, number] }
interface Ball {
  segs: Segment[];
  t: number;
  seg: number;
  bucket: number;
  bet: number;
  mult: number;
  resolve: (r: { bet: number; payout: number }) => void;
  trail: { x: number; y: number }[];
  hue: number;
}

function bucketColor(m: number, maxM: number): [string, string] {
  // low multipliers glow yellow, high ones hot pink / red
  const t = Math.min(1, Math.max(0, Math.log(Math.max(m, 0.2) / 0.2) / Math.log(Math.max(maxM, 1.5) / 0.2)));
  const hue = (55 - t * 85 + 360) % 360;
  return [`hsl(${hue}, 100%, ${60 - t * 6}%)`, `hsl(${hue}, 95%, ${36 - t * 8}%)`];
}

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Pick risk (low / medium / high) and the number of rows (8–16).',
      'Each ball bounces left or right on every peg, then lands in a multiplier bucket.',
      'Edge buckets are rare but pay big. Middle buckets are common and pay less.',
      'You can drop many balls at once. Each ball is a separate bet.',
      'Every table is tuned to ~99% RTP (1% house edge).',
    ],
    stageClass: 'plinko-stage',
  });

  let rows = 16;
  let risk: PlinkoRisk = 'medium';
  const balls: Ball[] = [];
  const pinGlow = new Map<string, number>();
  const bucketHit = new Map<number, number>();
  let table = plinkoTable(rows, risk);

  const canvas = h('canvas.stage-canvas') as HTMLCanvasElement;
  const ctx = canvas.getContext('2d')!;
  const lastHits = h('div.plinko-hits');
  shell.stage.append(canvas, lastHits);
  let W = 0, H = 0;
  const offResize = fitCanvas(canvas, (w, hh) => {
    W = w;
    H = hh;
  });

  // ---- geometry
  const geo = () => {
    const sp = Math.min(W / (rows + 2.2), (H - 8) / (rows + 1.9));
    const top = (H - sp * (rows + 1.9)) / 2 + sp * 1.3;
    return { sp, cx: W / 2, top };
  };
  const pinX = (i: number, j: number) => j - (i + 2) / 2;
  const bucketX = (b: number) => b + 0.5 - (rows + 1) / 2;

  function buildPath(path: number[]): Segment[] {
    const segs: Segment[] = [];
    const contactY = (i: number) => i - (PIN_R + BALL_R) * 0.95;
    let R = 0;
    let x = rand(-0.08, 0.08);
    let y = -1.25;
    for (let i = 0; i < rows; i++) {
      const j = 1 + R;
      const dir = path[i] ? 1 : -1;
      const cx = pinX(i, j) + dir * rand(0.08, 0.16);
      const cy = contactY(i);
      segs.push(makeSeg(x, y, cx, cy, i === 0, [i, j]));
      x = cx;
      y = cy;
      R += path[i];
    }
    // into bucket
    const bx = bucketX(R) + rand(-0.12, 0.12);
    segs.push(makeSeg(x, y, bx, rows - 0.15, false));
    return segs;
  }
  function makeSeg(x0: number, y0: number, x1: number, y1: number, fromRest: boolean, pin?: [number, number]): Segment {
    const dy = y1 - y0;
    let T: number, vy0: number;
    if (fromRest) {
      T = Math.sqrt((2 * dy) / G);
      vy0 = 0;
    } else {
      T = Math.sqrt((2 * dy) / G) * rand(1.22, 1.34);
      vy0 = (dy - 0.5 * G * T * T) / T;
    }
    return { x0, y0, x1, y1, vy0, T, pin };
  }

  // ---- drop
  async function drop(bet: number): Promise<{ bet: number; payout: number } | null> {
    if (!wallet.placeBet(meta.id, bet)) return null;
    const snapRows = rows;
    const snapTable = table;
    const { floats } = await fair.next(snapRows);
    const { path, bucket } = plinkoPath(floats, snapRows);
    if (snapRows !== rows) {
      // board changed while hashing (shouldn't happen: controls lock) — settle instantly
      const payout = round2(bet * snapTable[bucket]);
      wallet.settle(meta.id, bet, payout, `Bucket ${bucket}`, snapTable[bucket]);
      return { bet, payout };
    }
    sound.play('pop', 1.6);
    lockSettings();
    return new Promise((resolve) => {
      balls.push({ segs: buildPath(path), t: 0, seg: 0, bucket, bet, mult: snapTable[bucket], resolve, trail: [], hue: rand(320, 345) });
    });
  }

  function land(b: Ball) {
    const payout = round2(b.bet * b.mult);
    bucketHit.set(b.bucket, performance.now());
    const maxM = table[0];
    const { sp, cx, top } = geo();
    const rect = canvas.getBoundingClientRect();
    const px = rect.left + cx + bucketX(b.bucket) * sp;
    const py = rect.top + top + (rows - 0.4) * sp;
    wallet.settle(meta.id, b.bet, payout, `${fmt(b.mult)}× bucket`, b.mult);
    const [c1] = bucketColor(b.mult, maxM);
    addHit(b.mult, c1);
    if (b.mult >= 1) {
      sound.play(b.mult >= 3 ? 'win' : 'land', 1 + Math.min(1, b.mult / 20));
      fx.burst(px, py, { count: Math.min(60, 10 + b.mult * 3), colors: [c1, '#ffffff', '#ffc94d'], speed: 260 });
      if (b.mult >= 10) fx.celebrate(shell.stage, payout, b.bet, b.mult);
    } else sound.play('land', 0.7);
    shell.addResult(`${b.mult}×`, b.mult >= 1 ? 'win' : 'lose');
    b.resolve({ bet: b.bet, payout });
  }

  function addHit(m: number, color: string) {
    const el = h('div.plinko-hit', { style: { background: color } }, `${m}×`);
    lastHits.prepend(el);
    while (lastHits.children.length > 5) lastHits.lastElementChild?.remove();
  }

  // ---- render loop
  const stopLoop = rafLoop((dt, now) => {
    const speed = panel.isAuto ? 1.35 : 1;
    // advance balls
    for (let k = balls.length - 1; k >= 0; k--) {
      const b = balls[k];
      b.t += dt * speed;
      while (b.seg < b.segs.length && b.t >= b.segs[b.seg].T) {
        b.t -= b.segs[b.seg].T;
        const s = b.segs[b.seg];
        if (s.pin) {
          pinGlow.set(`${s.pin[0]}:${s.pin[1]}`, now);
          sound.play('tick', 0.7 + (s.pin[0] / rows) * 0.8);
        }
        b.seg++;
      }
      if (b.seg >= b.segs.length) {
        balls.splice(k, 1);
        land(b);
        if (!balls.length) unlockSettings();
      }
    }
    draw(now);
  });

  function ballPos(b: Ball) {
    const s = b.segs[Math.min(b.seg, b.segs.length - 1)];
    const t = Math.min(b.t, s.T);
    return { x: s.x0 + (s.x1 - s.x0) * (t / s.T), y: s.y0 + s.vy0 * t + 0.5 * G * t * t };
  }

  function draw(now: number) {
    ctx.clearRect(0, 0, W, H);
    const { sp, cx, top } = geo();
    const X = (u: number) => cx + u * sp;
    const Y = (u: number) => top + u * sp;
    const pinR = Math.max(2, sp * PIN_R);

    // pins
    for (let i = 0; i < rows; i++) {
      for (let j = 0; j < i + 3; j++) {
        const x = X(pinX(i, j)), y = Y(i);
        const g = pinGlow.get(`${i}:${j}`);
        const age = g ? (now - g) / 400 : 1;
        if (age < 1) {
          ctx.fillStyle = `rgba(255, 61, 154, ${0.45 * (1 - age)})`;
          ctx.beginPath();
          ctx.arc(x, y, pinR * (2.6 + age * 2), 0, Math.PI * 2);
          ctx.fill();
        }
        ctx.fillStyle = age < 1 ? '#ffd6ec' : '#e9e8ff';
        ctx.shadowColor = age < 1 ? '#ff3d9a' : 'rgba(180, 170, 255, 0.6)';
        ctx.shadowBlur = age < 1 ? 14 : 6;
        ctx.beginPath();
        ctx.arc(x, y, pinR * (age < 1 ? 1.25 : 1), 0, Math.PI * 2);
        ctx.fill();
      }
    }
    ctx.shadowBlur = 0;

    // buckets
    const maxM = table[0];
    const bw = sp * 0.9, bh = Math.max(18, sp * 0.72);
    const fontSize = Math.max(8, Math.min(14, sp * 0.3));
    ctx.font = `800 ${fontSize}px Orbitron, sans-serif`;
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    for (let b = 0; b <= rows; b++) {
      const m = table[b];
      const hit = bucketHit.get(b);
      const age = hit ? (now - hit) / 500 : 1;
      const bounce = age < 1 ? Math.sin(age * Math.PI) * sp * 0.22 : 0;
      const x = X(bucketX(b)) - bw / 2, y = Y(rows - 0.55) + bounce;
      const [c1, c2] = bucketColor(m, maxM);
      const grad = ctx.createLinearGradient(0, y, 0, y + bh);
      grad.addColorStop(0, c1);
      grad.addColorStop(1, c2);
      ctx.fillStyle = grad;
      ctx.shadowColor = c1;
      ctx.shadowBlur = age < 1 ? 26 : 10;
      roundRect(ctx, x, y, bw, bh, Math.min(6, sp * 0.15));
      ctx.fill();
      ctx.shadowBlur = 0;
      ctx.fillStyle = 'rgba(0,0,0,0.25)';
      roundRect(ctx, x, y + bh - 4, bw, 4, 2);
      ctx.fill();
      ctx.fillStyle = '#1a0820';
      const label = m >= 100 ? String(Math.round(m)) : String(m);
      ctx.fillText(sp < 26 ? label : label + '×', x + bw / 2, y + bh / 2 + 1);
    }

    // balls
    for (const b of balls) {
      const p = ballPos(b);
      const x = X(p.x), y = Y(p.y);
      b.trail.push({ x: p.x, y: p.y });
      if (b.trail.length > 8) b.trail.shift();
      if (!store.data.settings.reducedMotion) {
        b.trail.forEach((tp, i) => {
          ctx.fillStyle = `hsla(${b.hue}, 100%, 65%, ${(i / b.trail.length) * 0.25})`;
          ctx.beginPath();
          ctx.arc(X(tp.x), Y(tp.y), sp * BALL_R * (0.5 + (i / b.trail.length) * 0.5), 0, Math.PI * 2);
          ctx.fill();
        });
      }
      const r = sp * BALL_R;
      const g = ctx.createRadialGradient(x - r * 0.35, y - r * 0.35, r * 0.1, x, y, r);
      g.addColorStop(0, '#fff');
      g.addColorStop(0.35, `hsl(${b.hue}, 100%, 68%)`);
      g.addColorStop(1, `hsl(${b.hue}, 100%, 42%)`);
      ctx.fillStyle = g;
      ctx.shadowColor = `hsl(${b.hue}, 100%, 60%)`;
      ctx.shadowBlur = 16;
      ctx.beginPath();
      ctx.arc(x, y, r, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
    }
  }

  // ---- controls
  const riskSeg = h('div.seg', (['low', 'medium', 'high'] as PlinkoRisk[]).map((r) =>
    h(`button${r === risk ? '.active' : ''}`, { type: 'button', onclick: (e: Event) => {
      risk = r;
      riskSeg.querySelectorAll('button').forEach((b) => b.classList.toggle('active', b === e.currentTarget));
      onBoardChange();
    } }, r[0].toUpperCase() + r.slice(1))));
  const rowsVal = h('span.rows-val', String(rows));
  const rowsInput = h('input', { type: 'range', min: String(PLINKO.minRows), max: String(PLINKO.maxRows), step: '1', value: String(rows) }) as HTMLInputElement;
  rowsInput.addEventListener('input', () => {
    rows = parseInt(rowsInput.value);
    rowsVal.textContent = String(rows);
    sound.play('tick');
    onBoardChange();
  });
  const rtpEl = h('span.muted.small');
  function onBoardChange() {
    table = plinkoTable(rows, risk);
    rtpEl.textContent = `Max ${table[0]}× · RTP ${(plinkoRtp(rows, risk) * 100).toFixed(2)}%`;
    pinGlow.clear();
    bucketHit.clear();
  }
  onBoardChange();
  function lockSettings() {
    riskSeg.querySelectorAll('button').forEach((b) => ((b as HTMLButtonElement).disabled = true));
    rowsInput.disabled = true;
  }
  function unlockSettings() {
    if (panel.isAuto) return;
    riskSeg.querySelectorAll('button').forEach((b) => ((b as HTMLButtonElement).disabled = false));
    rowsInput.disabled = false;
  }

  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Drop ball',
    concurrent: true,
    onAction: (bet) => { drop(bet); },
    autoRound: (bet) => drop(bet),
    autoOverlap: 260,
    onAutoChange: (on) => { if (!on && !balls.length) unlockSettings(); },
  });
  panel.settings.append(
    h('div.field', h('label.field-label', 'Risk'), riskSeg),
    h('div.field', h('label.field-label', h('span', 'Rows'), rowsVal), rowsInput),
    rtpEl,
  );
  shell.controls.appendChild(panel.el);

  return () => {
    stopLoop();
    offResize();
    panel.destroy();
    // resolve any balls still in the air so no chips are lost
    balls.splice(0).forEach((b) => {
      const payout = round2(b.bet * b.mult);
      wallet.settle(meta.id, b.bet, payout, `${fmt(b.mult)}× bucket`, b.mult);
      b.resolve({ bet: b.bet, payout });
    });
    shell.destroy();
  };
}

function roundRect(ctx: CanvasRenderingContext2D, x: number, y: number, w: number, hh: number, r: number) {
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + w, y, x + w, y + hh, r);
  ctx.arcTo(x + w, y + hh, x, y + hh, r);
  ctx.arcTo(x, y + hh, x, y, r);
  ctx.arcTo(x, y, x + w, y, r);
  ctx.closePath();
}
