/** ROULETTE — European single-zero wheel with a full betting layout. */
import type { GameMeta } from './registry';
import { ROULETTE } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { RED_NUMBERS, ROULETTE_ORDER, rouletteNumber } from '../engine/outcomes';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { easeInOutCubic, easeOutCubic, fitCanvas, fmt, fmtShort, h, rafLoop, round2 } from '../engine/util';
import { gameShell } from '../ui/gameShell';

type BetType = keyof typeof ROULETTE.payouts;
interface Spot { key: string; type: BetType; numbers: number[]; el: HTMLElement; label: string }

const TAU = Math.PI * 2;
const SLOT = TAU / 37;
const CHIP_COLORS: Record<number, string> = { 1: '#cfd3e6', 5: '#e53950', 10: '#2f7cf6', 25: '#16a35a', 100: '#24213a', 500: '#8e44ff', 1000: '#f5b300', 5000: '#ff5ab4' };
const chipColor = (amt: number) => {
  const denoms = ROULETTE.chips.filter((c) => c <= amt);
  return CHIP_COLORS[denoms.length ? denoms[denoms.length - 1] : 1] ?? '#cfd3e6';
};
const colorOf = (n: number) => (n === 0 ? 'green' : RED_NUMBERS.has(n) ? 'red' : 'black');

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'European roulette: 37 pockets (0–36), single zero. House edge 2.70%.',
      'Pick a chip and click the table to bet. Right-click (or Undo) to remove.',
      'Inside bets: Straight 35:1 · Split 17:1 · Street 11:1 · Corner 8:1 · Six line 5:1. Click the lines between numbers for splits, the corners for corners, the bottom edge for streets.',
      'Outside bets: Dozens & Columns 2:1 · Red/Black, Odd/Even, 1–18/19–36 pay 1:1. Zero loses all outside bets.',
      'Rebet repeats your last spin; 2× doubles everything on the table.',
    ],
    stageClass: 'roulette-stage',
  });

  // ---------------------------------------------------------------- state
  const bets = new Map<string, number>();
  let lastBets: [string, number][] = [];
  const undoStack: { key: string; amount: number }[] = [];
  let chip = ROULETTE.chips[2];
  let spinning = false;
  const spots = new Map<string, Spot>();
  const numCells = new Map<number, HTMLElement>();

  // ---------------------------------------------------------------- wheel
  const canvas = h('canvas.stage-canvas') as HTMLCanvasElement;
  const ctx = canvas.getContext('2d')!;
  const resultBadge = h('div.rw-result', h('span', '?'));
  const wheelWrap = h('div.rw-wheel-wrap', h('div.rw-wheel-tilt', canvas), resultBadge);
  const hotNums = h('div.rw-hot');
  const statsBox = h('div.rw-side',
    h('div.rw-side-title', 'Last results'),
    hotNums,
    h('div.rw-legend', h('span.dot.red'), 'Red', h('span.dot.black'), 'Black', h('span.dot.green'), 'Zero'),
  );
  const top = h('div.rw-top', wheelWrap, statsBox);

  let W = 0;
  let wheelImg: HTMLCanvasElement | null = null;
  const offResize = fitCanvas(canvas, (w) => {
    W = w;
    wheelImg = null;
  });

  // animation state
  let wheelAngle = 0;
  const idleSpeed = 0.35;
  let spin: null | { t: number; T: number; Tland: number; extra: number; wheel0: number; pocket: number; delta: number; r0: number; resolve: () => void } = null;
  let ballAngle = -Math.PI / 2; // world
  let ballR = 0.42; // fraction of radius
  let ballAttached: number | null = null; // pocket index the ball sits in
  let ballVisible = false;

  const R_TRACK = 0.86, R_POCKET = 0.625;
  const pocketAngle = (num: number) => ROULETTE_ORDER.indexOf(num) * SLOT;

  function renderWheel(size: number) {
    const dpr = Math.min(devicePixelRatio || 1, 2);
    const c = document.createElement('canvas');
    c.width = c.height = Math.round(size * dpr);
    const g = c.getContext('2d')!;
    g.scale(dpr, dpr);
    const R = size / 2;
    g.translate(R, R);
    // outer wooden rim
    let grad = g.createRadialGradient(0, 0, R * 0.8, 0, 0, R);
    grad.addColorStop(0, '#3a1d0c');
    grad.addColorStop(0.5, '#6b3a17');
    grad.addColorStop(0.85, '#4a240d');
    grad.addColorStop(1, '#1e0d04');
    g.fillStyle = grad;
    g.beginPath(); g.arc(0, 0, R, 0, TAU); g.fill();
    // metallic ring
    g.strokeStyle = '#d9b45a'; g.lineWidth = R * 0.02;
    g.beginPath(); g.arc(0, 0, R * 0.93, 0, TAU); g.stroke();
    // ball track
    grad = g.createRadialGradient(0, 0, R * 0.74, 0, 0, R * 0.92);
    grad.addColorStop(0, '#1a1030'); grad.addColorStop(0.5, '#2a1d4a'); grad.addColorStop(1, '#120a22');
    g.fillStyle = grad;
    g.beginPath(); g.arc(0, 0, R * 0.92, 0, TAU); g.fill();
    // number ring + pockets
    for (let i = 0; i < 37; i++) {
      const n = ROULETTE_ORDER[i];
      const a0 = i * SLOT - SLOT / 2 - Math.PI / 2, a1 = a0 + SLOT;
      const col = n === 0 ? '#0fa860' : RED_NUMBERS.has(n) ? '#d61f45' : '#15122a';
      // number band
      g.fillStyle = col;
      g.beginPath(); g.arc(0, 0, R * 0.76, a0, a1); g.arc(0, 0, R * 0.66, a1, a0, true); g.closePath(); g.fill();
      // pocket (darker)
      g.fillStyle = n === 0 ? '#0a7a45' : RED_NUMBERS.has(n) ? '#9e1531' : '#0c0a1a';
      g.beginPath(); g.arc(0, 0, R * 0.66, a0, a1); g.arc(0, 0, R * 0.56, a1, a0, true); g.closePath(); g.fill();
      // number text
      g.save();
      g.rotate(i * SLOT);
      g.fillStyle = '#fff';
      g.font = `700 ${R * 0.075}px Orbitron, sans-serif`;
      g.textAlign = 'center'; g.textBaseline = 'middle';
      g.fillText(String(n), 0, -R * 0.71);
      g.restore();
      // frets
      g.strokeStyle = '#e8c56a'; g.lineWidth = Math.max(1, R * 0.008);
      g.beginPath(); g.moveTo(Math.cos(a0) * R * 0.56, Math.sin(a0) * R * 0.56); g.lineTo(Math.cos(a0) * R * 0.76, Math.sin(a0) * R * 0.76); g.stroke();
    }
    g.strokeStyle = '#e8c56a'; g.lineWidth = Math.max(1, R * 0.012);
    [0.76, 0.66, 0.56].forEach((r) => { g.beginPath(); g.arc(0, 0, R * r, 0, TAU); g.stroke(); });
    // cone
    grad = g.createRadialGradient(-R * 0.12, -R * 0.12, R * 0.05, 0, 0, R * 0.56);
    grad.addColorStop(0, '#8a5a2b'); grad.addColorStop(0.5, '#5a3414'); grad.addColorStop(1, '#2a1607');
    g.fillStyle = grad;
    g.beginPath(); g.arc(0, 0, R * 0.555, 0, TAU); g.fill();
    // decorative inlay
    g.strokeStyle = 'rgba(232, 197, 106, 0.4)'; g.lineWidth = 1.5;
    g.beginPath(); g.arc(0, 0, R * 0.4, 0, TAU); g.stroke();
    // turret
    for (let k = 0; k < 4; k++) {
      g.save(); g.rotate((k * Math.PI) / 2);
      const tg = g.createLinearGradient(0, 0, 0, -R * 0.34);
      tg.addColorStop(0, '#fff3c4'); tg.addColorStop(1, '#b8862a');
      g.fillStyle = tg;
      g.beginPath(); g.moveTo(-R * 0.025, 0); g.lineTo(-R * 0.012, -R * 0.3); g.lineTo(R * 0.012, -R * 0.3); g.lineTo(R * 0.025, 0); g.fill();
      g.beginPath(); g.arc(0, -R * 0.32, R * 0.035, 0, TAU); g.fill();
      g.restore();
    }
    grad = g.createRadialGradient(-R * 0.03, -R * 0.03, 1, 0, 0, R * 0.09);
    grad.addColorStop(0, '#fff8d9'); grad.addColorStop(1, '#a87a22');
    g.fillStyle = grad;
    g.beginPath(); g.arc(0, 0, R * 0.08, 0, TAU); g.fill();
    return c;
  }

  const stopLoop = rafLoop((dt) => {
    // wheel motion
    if (spin) {
      spin.t += dt;
      const u = Math.min(1, spin.t / spin.T);
      wheelAngle = spin.wheel0 + idleSpeed * spin.t + spin.extra * easeOutCubic(u);
      const ul = Math.min(1, spin.t / spin.Tland);
      const rel = spin.pocket + spin.delta * (1 - easeOutCubic(ul));
      ballAngle = wheelAngle + rel;
      // radius profile: launch → track → spiral → bounces → pocket
      if (ul < 0.06) ballR = spin.r0 + (R_TRACK - spin.r0) * easeOutCubic(ul / 0.06);
      else if (ul < 0.55) ballR = R_TRACK;
      else if (ul < 0.86) {
        const k = (ul - 0.55) / 0.31;
        ballR = R_TRACK + (R_POCKET + 0.06 - R_TRACK) * easeInOutCubic(k) + Math.sin(k * Math.PI * 5) * 0.025 * (1 - k);
      } else {
        const k = (ul - 0.86) / 0.14;
        ballR = R_POCKET + 0.06 * (1 - k) + Math.abs(Math.sin(k * Math.PI * 3)) * 0.04 * (1 - k);
      }
      if (ul >= 0.55 && ul < 0.86 && Math.random() < dt * 14) sound.play('spin');
      if (spin.t >= spin.Tland && ballAttached === null) {
        ballAttached = ROULETTE_ORDER.indexOf(currentResult);
        sound.play('land');
      }
      if (u >= 1) {
        const r = spin.resolve;
        spin = null;
        r();
      }
    } else {
      wheelAngle += idleSpeed * dt;
      if (ballAttached !== null) {
        ballAngle = wheelAngle + ballAttached * SLOT;
        ballR = R_POCKET;
      }
    }
    draw();
  });

  let currentResult = 0;

  function draw() {
    const size = W;
    if (!size) return;
    ctx.clearRect(0, 0, size, size);
    if (!wheelImg) wheelImg = renderWheel(size);
    const R = size / 2;
    ctx.save();
    ctx.translate(R, R);
    // drop shadow
    ctx.fillStyle = 'rgba(0,0,0,0.5)';
    ctx.beginPath(); ctx.arc(0, R * 0.02, R * 0.99, 0, TAU); ctx.fill();
    ctx.rotate(wheelAngle);
    ctx.drawImage(wheelImg, -R, -R, size, size);
    ctx.restore();
    // static lighting overlay (gives the 3D look)
    const light = ctx.createRadialGradient(R * 0.7, R * 0.55, R * 0.05, R, R, R);
    light.addColorStop(0, 'rgba(255,255,255,0.18)');
    light.addColorStop(0.5, 'rgba(255,255,255,0.03)');
    light.addColorStop(1, 'rgba(0,0,0,0.35)');
    ctx.fillStyle = light;
    ctx.beginPath(); ctx.arc(R, R, R, 0, TAU); ctx.fill();
    // diamond deflectors on the track
    ctx.save(); ctx.translate(R, R);
    for (let k = 0; k < 8; k++) {
      const a = (k / 8) * TAU + Math.PI / 8;
      ctx.save(); ctx.rotate(a); ctx.translate(0, -R * 0.8);
      ctx.fillStyle = '#e8c56a';
      ctx.beginPath(); ctx.moveTo(0, -R * 0.03); ctx.lineTo(R * 0.016, 0); ctx.lineTo(0, R * 0.03); ctx.lineTo(-R * 0.016, 0); ctx.fill();
      ctx.restore();
    }
    ctx.restore();
    // ball
    if (ballVisible) {
      const a = ballAngle - Math.PI / 2;
      const bx = R + Math.cos(a) * ballR * R, by = R + Math.sin(a) * ballR * R;
      const br = R * 0.038;
      ctx.fillStyle = 'rgba(0,0,0,0.45)';
      ctx.beginPath(); ctx.ellipse(bx + br * 0.4, by + br * 0.6, br, br * 0.7, 0, 0, TAU); ctx.fill();
      const bg = ctx.createRadialGradient(bx - br * 0.4, by - br * 0.4, br * 0.1, bx, by, br);
      bg.addColorStop(0, '#ffffff'); bg.addColorStop(0.6, '#e6e6f0'); bg.addColorStop(1, '#8d8da8');
      ctx.fillStyle = bg;
      ctx.shadowColor = 'rgba(255,255,255,0.8)'; ctx.shadowBlur = 8;
      ctx.beginPath(); ctx.arc(bx, by, br, 0, TAU); ctx.fill();
      ctx.shadowBlur = 0;
    }
  }

  function spinTo(result: number, fast: boolean) {
    return new Promise<void>((resolve) => {
      const T = fast ? 3.2 : 5.6;
      const Tland = T * 0.8;
      currentResult = result;
      const pocket = pocketAngle(result);
      const cur = ballVisible ? ballAngle : wheelAngle + Math.PI;
      // relative offset so the ball starts where it currently is, then orbits opposite to the wheel
      let delta = cur - wheelAngle - pocket;
      delta = ((delta % TAU) + TAU) % TAU;
      delta -= TAU * (fast ? 4 : 6);
      spin = { t: 0, T, Tland, extra: TAU * (fast ? 1.2 : 2), wheel0: wheelAngle, pocket, delta, r0: ballVisible ? ballR : R_TRACK, resolve };
      ballAttached = null;
      ballVisible = true;
      sound.play('whoosh');
    });
  }

  // ---------------------------------------------------------------- table
  const numsGrid = h('div.rt-nums');
  const hotLayer = h('div.rt-hot-layer');
  const numFor = (r: number, c: number) => 3 * c + (3 - r);
  const addSpot = (key: string, type: BetType, numbers: number[], el: HTMLElement, label: string) => {
    const sp: Spot = { key, type, numbers, el, label };
    spots.set(key, sp);
    el.dataset.spot = key;
    el.addEventListener('click', (e) => { e.stopPropagation(); place(key); });
    el.addEventListener('contextmenu', (e) => { e.preventDefault(); e.stopPropagation(); removeSpot(key); });
    el.addEventListener('pointerenter', () => highlight(numbers, true));
    el.addEventListener('pointerleave', () => highlight(numbers, false));
    return sp;
  };
  const zero = h('div.rt-cell.rt-zero.green', h('span', '0'));
  numCells.set(0, zero);
  addSpot('straight:0', 'straight', [0], zero, '0');
  for (let r = 0; r < 3; r++) {
    for (let c = 0; c < 12; c++) {
      const n = numFor(r, c);
      const cell = h(`div.rt-cell.rt-num.${colorOf(n)}`, { style: { gridRow: String(r + 1), gridColumn: String(c + 1) } }, h('span', String(n)));
      numsGrid.appendChild(cell);
      numCells.set(n, cell);
      addSpot(`straight:${n}`, 'straight', [n], cell, String(n));
    }
  }
  // hotspots (percent positions over the 12×3 number grid)
  const hot = (key: string, type: BetType, nums: number[], x: number, y: number, w: number, hh: number, cls: string) => {
    const el = h(`div.rt-hot.${cls}`, { style: { left: `${x}%`, top: `${y}%`, width: `${w}%`, height: `${hh}%` } });
    hotLayer.appendChild(el);
    addSpot(key, type, nums.sort((a, b) => a - b), el, nums.join('/'));
  };
  const CW = 100 / 12, RH = 100 / 3;
  for (let r = 0; r < 3; r++) {
    for (let c = 0; c < 12; c++) {
      const n = numFor(r, c);
      // split with right neighbour
      if (c < 11) hot(`split:${n}-${n + 3}`, 'split', [n, n + 3], (c + 1) * CW - CW * 0.16, r * RH + RH * 0.2, CW * 0.32, RH * 0.6, 'v');
      // split with the number above (row r-1)
      if (r > 0) hot(`split:${n}-${n + 1}`, 'split', [n, n + 1], c * CW + CW * 0.2, r * RH - RH * 0.14, CW * 0.6, RH * 0.28, 'h');
      // corner (top-right corner of this cell)
      if (r > 0 && c < 11) hot(`corner:${n}`, 'corner', [n, n + 1, n + 3, n + 4], (c + 1) * CW - CW * 0.16, r * RH - RH * 0.14, CW * 0.32, RH * 0.28, 'x');
    }
  }
  for (let c = 0; c < 12; c++) {
    const b = 3 * c + 1;
    hot(`street:${b}`, 'street', [b, b + 1, b + 2], c * CW + CW * 0.2, 100 - RH * 0.14, CW * 0.6, RH * 0.28, 'h');
    if (c < 11) hot(`sixline:${b}`, 'sixline', [b, b + 1, b + 2, b + 3, b + 4, b + 5], (c + 1) * CW - CW * 0.16, 100 - RH * 0.14, CW * 0.32, RH * 0.28, 'x');
  }
  // zero splits & trios
  for (let r = 0; r < 3; r++) {
    const n = numFor(r, 0);
    hot(`split:0-${n}`, 'split', [0, n], -CW * 0.14, r * RH + RH * 0.2, CW * 0.28, RH * 0.6, 'v');
  }
  hot('street:0-1-2', 'street', [0, 1, 2], -CW * 0.14, 2 * RH - RH * 0.14, CW * 0.28, RH * 0.28, 'x');
  hot('street:0-2-3', 'street', [0, 2, 3], -CW * 0.14, RH - RH * 0.14, CW * 0.28, RH * 0.28, 'x');
  hot('corner:0-1-2-3', 'corner', [0, 1, 2, 3], -CW * 0.14, 100 - RH * 0.14, CW * 0.28, RH * 0.28, 'x');
  numsGrid.appendChild(hotLayer);

  const range = (a: number, b: number) => Array.from({ length: b - a + 1 }, (_, i) => a + i);
  const cols = h('div.rt-cols', [0, 1, 2].map((r) => {
    const el = h('div.rt-cell.rt-out', '2:1');
    const colNums = range(0, 11).map((c) => numFor(r, c));
    addSpot(`column:${3 - r}`, 'column', colNums, el, `Column ${3 - r}`);
    return el;
  }));
  const dozens = h('div.rt-dozens', [1, 2, 3].map((d) => {
    const el = h('div.rt-cell.rt-out', `${d === 1 ? '1st' : d === 2 ? '2nd' : '3rd'} 12`);
    addSpot(`dozen:${d}`, 'dozen', range((d - 1) * 12 + 1, d * 12), el, `Dozen ${d}`);
    return el;
  }));
  const outsideDefs: [string, string, number[], string][] = [
    ['low', '1–18', range(1, 18), ''],
    ['even', 'EVEN', range(1, 36).filter((n) => n % 2 === 0), ''],
    ['red', '', range(1, 36).filter((n) => RED_NUMBERS.has(n)), 'red-diamond'],
    ['black', '', range(1, 36).filter((n) => !RED_NUMBERS.has(n)), 'black-diamond'],
    ['odd', 'ODD', range(1, 36).filter((n) => n % 2 === 1), ''],
    ['high', '19–36', range(19, 36), ''],
  ];
  const outside = h('div.rt-outside', outsideDefs.map(([key, label, nums, cls]) => {
    const el = h(`div.rt-cell.rt-out${cls ? '.' + cls : ''}`, cls ? h('span.diamond') : label);
    addSpot(key, 'even', nums, el, key[0].toUpperCase() + key.slice(1));
    return el;
  }));
  const table = h('div.rt-table', zero, numsGrid, cols, dozens, outside);
  const tableWrap = h('div.rt-table-wrap', table);

  function highlight(nums: number[], on: boolean) {
    nums.forEach((n) => numCells.get(n)?.classList.toggle('hl', on));
  }

  // ---------------------------------------------------------------- chips & toolbar
  const chipBar = h('div.rt-chipbar', ROULETTE.chips.map((v) => {
    const b = h(`button.rt-chip${v === chip ? '.active' : ''}`, { type: 'button', style: { '--chip': CHIP_COLORS[v] } as any, title: `${v} chips` }, fmtShort(v));
    b.onclick = () => {
      chip = v;
      chipBar.querySelectorAll('.rt-chip').forEach((x) => x.classList.toggle('active', x === b));
      sound.play('chip');
    };
    return b;
  }));
  const btn = (label: string, fn: () => void, title: string) => h('button.mini', { type: 'button', title, onclick: () => { if (!spinning) fn(); } }, label);
  const undoBtn = btn('↶ Undo', undo, 'Undo last chip');
  const clearBtn = btn('✕ Clear', clearAll, 'Clear table');
  const rebetBtn = btn('⟳ Rebet', rebet, 'Repeat last spin');
  const doubleBtn = btn('2×', doubleAll, 'Double all bets');
  const toolbar = h('div.rt-toolbar', chipBar, h('div.rt-actions', undoBtn, clearBtn, rebetBtn, doubleBtn));
  shell.stage.append(top, tableWrap, toolbar);

  const total = () => round2([...bets.values()].reduce((a, b) => a + b, 0));

  function renderChips() {
    spots.forEach((sp) => {
      const amt = bets.get(sp.key) ?? 0;
      let chipEl = sp.el.querySelector(':scope > .rt-placed') as HTMLElement | null;
      if (!amt) {
        chipEl?.remove();
        return;
      }
      if (!chipEl) {
        chipEl = h('div.rt-placed');
        sp.el.appendChild(chipEl);
      }
      chipEl.textContent = fmtShort(amt);
      chipEl.style.setProperty('--chip', chipColor(amt));
    });
    totalEl.textContent = fmt(total());
    undoBtn.toggleAttribute('disabled', !undoStack.length);
    clearBtn.toggleAttribute('disabled', !bets.size);
    rebetBtn.toggleAttribute('disabled', !lastBets.length || bets.size > 0);
    doubleBtn.toggleAttribute('disabled', !bets.size);
    rebetBtn.classList.toggle('glow', lastBets.length > 0 && bets.size === 0);
  }

  function place(key: string) {
    if (spinning || panel.isAuto) return;
    const t = total();
    if (t + chip > wallet.balance + 1e-9) {
      toast(wallet.balance < 1 ? 'Out of chips — grab a free refill!' : 'Not enough chips for that bet', 'error');
      sound.play('error');
      return;
    }
    if ((bets.get(key) ?? 0) + chip > ROULETTE.maxTableBet) {
      toast('Table limit reached for that spot', 'error');
      return;
    }
    bets.set(key, round2((bets.get(key) ?? 0) + chip));
    undoStack.push({ key, amount: chip });
    const sp = spots.get(key)!;
    const from = chipBar.querySelector('.rt-chip.active');
    if (from) fx.flyChip(from, sp.el, CHIP_COLORS[chip]);
    sound.play('chip');
    renderChips();
    const placed = sp.el.querySelector('.rt-placed');
    placed?.classList.remove('bump');
    void (placed as HTMLElement | null)?.offsetWidth;
    placed?.classList.add('bump');
  }
  function removeSpot(key: string) {
    if (spinning || panel.isAuto || !bets.has(key)) return;
    bets.delete(key);
    for (let i = undoStack.length - 1; i >= 0; i--) if (undoStack[i].key === key) undoStack.splice(i, 1);
    sound.play('click');
    renderChips();
  }
  function undo() {
    const last = undoStack.pop();
    if (!last) return;
    const v = round2((bets.get(last.key) ?? 0) - last.amount);
    if (v <= 0) bets.delete(last.key);
    else bets.set(last.key, v);
    sound.play('click');
    renderChips();
  }
  function clearAll() {
    bets.clear();
    undoStack.length = 0;
    sound.play('whoosh');
    renderChips();
  }
  function rebet() {
    if (!lastBets.length) return;
    const sum = lastBets.reduce((a, [, v]) => a + v, 0);
    if (sum > wallet.balance + 1e-9) {
      toast('Not enough chips to rebet', 'error');
      return;
    }
    bets.clear();
    undoStack.length = 0;
    lastBets.forEach(([k, v]) => {
      bets.set(k, v);
      undoStack.push({ key: k, amount: v });
    });
    sound.play('chip');
    renderChips();
  }
  function doubleAll() {
    const t = total();
    if (t * 2 > wallet.balance + 1e-9) {
      toast('Not enough chips to double', 'error');
      return;
    }
    [...bets.entries()].forEach(([k, v]) => {
      bets.set(k, round2(v * 2));
      undoStack.push({ key: k, amount: v });
    });
    sound.play('chip');
    renderChips();
  }

  // ---------------------------------------------------------------- spin
  const history: number[] = [];
  function renderHot() {
    statsBox.classList.toggle('empty', !history.length);
    hotNums.replaceChildren(...history.slice(0, 12).map((n, i) => h(`span.rw-num.${colorOf(n)}${i === 0 ? '.latest' : ''}`, String(n))));
  }
  renderHot();

  async function doSpin(fast = false): Promise<{ bet: number; payout: number } | null> {
    if (spinning) return null;
    if (!bets.size && fast && lastBets.length) rebet();
    const stake = total();
    if (stake <= 0) {
      toast('Place a bet on the table first', 'info');
      return null;
    }
    if (!wallet.placeBet(meta.id, stake)) return null;
    spinning = true;
    tableWrap.classList.add('locked');
    const snapshot = [...bets.entries()];
    try {
      const { floats } = await fair.next(1);
      const result = rouletteNumber(floats[0]);
      resultBadge.className = 'rw-result spinning';
      resultBadge.replaceChildren(h('span', '…'));
      await spinTo(result, fast || store.data.settings.reducedMotion);
      // payout
      let payout = 0;
      const winners: string[] = [];
      for (const [key, amt] of snapshot) {
        const sp = spots.get(key)!;
        if (sp.numbers.includes(result)) {
          payout += amt * (ROULETTE.payouts[sp.type] + 1);
          winners.push(key);
        }
      }
      payout = round2(payout);
      resultBadge.className = `rw-result ${colorOf(result)}`;
      resultBadge.replaceChildren(h('span', String(result)));
      history.unshift(result);
      renderHot();
      shell.addResult(String(result), colorOf(result) as any);
      const cell = numCells.get(result)!;
      cell.classList.add('winner');
      setTimeout(() => cell.classList.remove('winner'), 2600);
      // chip feedback
      spots.forEach((sp) => {
        const c = sp.el.querySelector(':scope > .rt-placed');
        if (!c) return;
        c.classList.add(winners.includes(sp.key) ? 'won' : 'lost');
      });
      const mult = payout / stake;
      wallet.settle(meta.id, stake, payout, `${result} ${colorOf(result)}`, mult);
      if (payout > 0) fx.pop(shell.stage, `+${fmt(payout)}`, mult >= 10 ? 'big' : 'win');
      fx.celebrate(shell.stage, payout, stake, mult);
      lastBets = snapshot;
      await new Promise((r) => setTimeout(r, fast ? 650 : 1300));
      bets.clear();
      undoStack.length = 0;
      renderChips();
      return { bet: stake, payout };
    } finally {
      spinning = false;
      tableWrap.classList.remove('locked');
    }
  }

  // ---------------------------------------------------------------- panel
  const totalEl = h('span.rt-total', '0.00');
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Spin',
    hideAmount: true,
    onAction: () => doSpin(false),
    autoRound: async () => {
      if (!bets.size) rebet();
      return doSpin(true);
    },
    autoDelay: 500,
  });
  panel.settings.append(
    h('div.info-box',
      h('div.stat-row', h('span.muted', 'Total bet'), h('span', h('span.chip-icon.inline'), totalEl)),
      h('div.stat-row', h('span.muted', 'Selected chip'), h('span.rt-chip-label')),
      h('p.muted.small', 'Click the table to place chips. Right-click a chip to remove it.'),
    ),
  );
  const chipLabel = panel.settings.querySelector('.rt-chip-label') as HTMLElement;
  const syncChipLabel = () => (chipLabel.textContent = fmt(chip, 0));
  chipBar.addEventListener('click', syncChipLabel);
  syncChipLabel();
  shell.controls.appendChild(panel.el);
  renderChips();

  return () => {
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
