/** ROAD RUSH — hop Bolt the robo-bunny across neon traffic lanes; cash out before a crash. */
import '../styles/games/chicken.css';
import type { GameMeta } from './registry';
import { CHICKEN } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { chickenHitLane } from '../engine/outcomes';
import { chickenMultiplier, type ChickenDifficulty } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { clamp, easeInOutCubic, fitCanvas, fmt, fmtMult, h, rafLoop, rand, round2, sleep } from '../engine/util';
import { gameShell } from '../ui/gameShell';

class Abort extends Error {}

interface Round {
  bet: number;
  diff: ChickenDifficulty;
  lanes: number;
  hitLane: number;
  lane: number; // lane Bolt stands in (-1 = start sidewalk)
  over: boolean;
  payout: number;
  pendingCrash: boolean;
  ready: boolean;
}
interface Car { lane: number; y: number; speed: number; dir: 1 | -1; len: number; color: string; truck: boolean; fade: number; killer?: boolean }
interface Hop { from: number; to: number; t: number; dur: number; resolve: () => void }

const CAR_COLORS = ['#ff3d9a', '#22e4ff', '#ffc94d', '#9b5cff', '#2dff8a', '#ff8a3d'];

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Meet Bolt, a tiny neon robo-bunny who needs to cross a very busy road.',
      'Place a bet, then press Hop (or tap the road / press →) to jump one lane forward.',
      'Every lane you survive raises the multiplier. A hover-car can smash into any lane — if it hits Bolt the bet is lost.',
      'Cash out any time after the first lane. Reach the finish line to collect the maximum.',
      'Easy: 1/25 crash chance per lane · Medium: 3/25 · Hard: 5/25 · Daredevil: 10/25. RTP 98%.',
    ],
    stageClass: 'road-stage',
  });

  let diff: ChickenDifficulty = 'medium';
  let round: Round | null = null;
  let destroyed = false;
  let hop: Hop | null = null;
  const wait = async (ms: number) => {
    await sleep(ms);
    if (destroyed) throw new Abort();
  };
  const lanesCount = () => (round ? round.lanes : CHICKEN.difficulty[diff].lanes);
  const diffNow = () => (round ? round.diff : diff);
  const fast = () => panel.isAuto || store.data.settings.reducedMotion;

  // ---------------------------------------------------------------- DOM
  const canvas = h('canvas.stage-canvas') as HTMLCanvasElement;
  const ctx = canvas.getContext('2d')!;
  const hudLane = h('div.rr-hud-val', '0 / 0');
  const hudMult = h('div.rr-hud-val.big', '1.00×');
  const hudNext = h('div.rr-hud-val.next', '—');
  const hud = h('div.rr-hud',
    h('div.rr-hud-item', h('div.rr-hud-label', 'Lane'), hudLane),
    h('div.rr-hud-item.main', h('div.rr-hud-label', 'Multiplier'), hudMult),
    h('div.rr-hud-item', h('div.rr-hud-label', 'Next'), hudNext),
  );
  const banner = h('div.rr-banner');
  const hint = h('div.rr-hint', 'Place a bet to start');
  shell.stage.append(canvas, hud, banner, hint);
  let W = 0, H = 0;
  const offResize = fitCanvas(canvas, (w, hh) => { W = w; H = hh; });
  canvas.addEventListener('click', () => {
    if (!panel.isAuto) doHop().catch(onAbort);
  });

  // ---------------------------------------------------------------- world
  let cars: Car[] = [];
  const spawnT = new Map<number, number>();
  let camX = 0;
  let boltLane = -1; // visual lane (can be lanes = finish)
  let boltState: 'idle' | 'hop' | 'dead' | 'win' = 'idle';
  let deadT = 0;
  let winT = 0;
  let blink = 0;

  const geo = () => {
    const LW = clamp(Math.min(H * 0.29, W / 3.8), 66, 124);
    const startW = LW * 1.35;
    const roadY = H * 0.5;
    return { LW, startW, boltY: roadY + LW * 0.15, finishX: startW + LW * lanesCount() };
  };
  const laneX = (i: number) => {
    const { LW, startW, finishX } = geo();
    if (i < 0) return startW * 0.5;
    if (i >= lanesCount()) return finishX + LW * 0.8;
    return startW + LW * (i + 0.5);
  };
  const laneDir = (i: number): 1 | -1 => (i % 2 === 0 ? 1 : -1);
  const safeLimit = () => (round ? round.lane : -1); // lanes ≤ this are barricaded

  function boltPos() {
    const { LW, boltY } = geo();
    if (hop) {
      const t = easeInOutCubic(Math.min(1, hop.t / hop.dur));
      const x = laneX(hop.from) + (laneX(hop.to) - laneX(hop.from)) * t;
      return { x, y: boltY - Math.sin(Math.PI * Math.min(1, hop.t / hop.dur)) * LW * 0.42, squash: 1 + Math.sin(Math.PI * t) * 0.12 };
    }
    let y = boltY;
    if (boltState === 'win') y -= Math.abs(Math.sin(winT * 7)) * LW * 0.25;
    return { x: laneX(boltLane), y, squash: 1 };
  }

  // ---------------------------------------------------------------- loop
  const stopLoop = rafLoop((dt) => {
    const { LW, boltY, finishX } = geo();
    blink += dt;
    if (boltState === 'dead') deadT += dt;
    if (boltState === 'win') winT += dt;
    // hop progress
    if (hop) {
      hop.t += dt;
      if (hop.t >= hop.dur) {
        const hp = hop;
        hop = null;
        boltLane = hp.to;
        hp.resolve();
      }
    }
    // camera
    const bp = boltPos();
    const worldW = finishX + LW * 1.8;
    const target = clamp(bp.x - W * 0.34, 0, Math.max(0, worldW - W));
    camX += (target - camX) * Math.min(1, dt * 5);
    // spawn traffic in visible lanes that aren't barricaded
    const first = Math.max(0, Math.floor((camX - geo().startW) / LW) - 1);
    const last = Math.min(lanesCount() - 1, Math.ceil((camX + W - geo().startW) / LW) + 1);
    for (let i = first; i <= last; i++) {
      if (i <= safeLimit() || (hop && i === hop.to)) continue;
      const t = (spawnT.get(i) ?? rand(0, 1.5)) - dt;
      if (t <= 0) {
        const truck = Math.random() < 0.25;
        const len = LW * (truck ? 1.55 : 0.95);
        const dir = laneDir(i);
        cars.push({ lane: i, dir, len, truck, y: dir === 1 ? -len : H + len, speed: H * rand(0.9, 1.7) * (truck ? 0.8 : 1), color: CAR_COLORS[Math.floor(Math.random() * CAR_COLORS.length)], fade: 1 });
        spawnT.set(i, rand(0.9, 2.6));
      } else spawnT.set(i, t);
    }
    // move cars
    for (const c of cars) {
      c.y += c.dir * c.speed * dt;
      const barricaded = c.lane <= safeLimit() || (hop && c.lane === hop.to && !c.killer);
      // cars approaching a barricaded lane's bunny fade away before the barrier
      if (barricaded && !c.killer) {
        const ahead = c.dir === 1 ? c.y + c.len / 2 < boltY - LW * 0.6 : c.y - c.len / 2 > boltY + LW * 0.6;
        if (ahead || c.fade < 1) c.fade -= dt * 4;
      }
    }
    cars = cars.filter((c) => c.fade > 0 && c.y > -c.len * 2 && c.y < H + c.len * 2);
    draw();
  });

  // ---------------------------------------------------------------- draw
  function draw() {
    if (!W) return;
    const { LW, startW, boltY, finishX } = geo();
    const n = lanesCount();
    ctx.clearRect(0, 0, W, H);
    ctx.save();
    ctx.translate(-camX, 0);
    // start sidewalk
    const sw = ctx.createLinearGradient(0, 0, startW, 0);
    sw.addColorStop(0, '#231a4d');
    sw.addColorStop(1, '#2d2160');
    ctx.fillStyle = sw;
    ctx.fillRect(camX - 2, 0, startW - camX + 2, H);
    ctx.strokeStyle = 'rgba(155, 92, 255, 0.25)';
    ctx.lineWidth = 1;
    for (let y = (blink * 0) % 40; y < H; y += 40) { ctx.beginPath(); ctx.moveTo(camX, y); ctx.lineTo(startW, y); ctx.stroke(); }
    ctx.fillStyle = '#b28bff';
    ctx.shadowColor = '#9b5cff';
    ctx.shadowBlur = 12;
    ctx.fillRect(startW - 5, 0, 5, H);
    ctx.shadowBlur = 0;
    // road
    const road = ctx.createLinearGradient(0, 0, 0, H);
    road.addColorStop(0, '#0e0a24');
    road.addColorStop(0.5, '#17123a');
    road.addColorStop(1, '#0e0a24');
    ctx.fillStyle = road;
    ctx.fillRect(startW, 0, finishX - startW, H);
    const first = Math.max(0, Math.floor((camX - startW) / LW) - 1);
    const last = Math.min(n - 1, Math.ceil((camX + W - startW) / LW) + 1);
    for (let i = first; i <= last; i++) {
      const x0 = startW + LW * i;
      const cleared = round && i <= round.lane;
      if (cleared) {
        ctx.fillStyle = 'rgba(45, 255, 138, 0.06)';
        ctx.fillRect(x0, 0, LW, H);
      }
      // dashed divider
      if (i > 0) {
        ctx.strokeStyle = 'rgba(34, 228, 255, 0.35)';
        ctx.lineWidth = 2;
        ctx.setLineDash([LW * 0.28, LW * 0.22]);
        ctx.lineDashOffset = -blink * 30 * laneDir(i);
        ctx.beginPath(); ctx.moveTo(x0, 0); ctx.lineTo(x0, H); ctx.stroke();
        ctx.setLineDash([]);
      }
      // manhole / multiplier coin
      const cx = x0 + LW / 2, cy = boltY + LW * 0.36;
      const m = chickenMultiplier(diffNow(), i + 1);
      const next = round && !round.over && i === round.lane + 1;
      ctx.save();
      ctx.translate(cx, cy);
      ctx.scale(1, 0.55);
      const rr = LW * 0.36;
      const cg = ctx.createRadialGradient(-rr * 0.3, -rr * 0.3, rr * 0.1, 0, 0, rr);
      if (cleared) { cg.addColorStop(0, '#8dffbf'); cg.addColorStop(1, '#0a7a45'); }
      else if (next) { cg.addColorStop(0, '#fff3b0'); cg.addColorStop(1, '#c98a00'); }
      else { cg.addColorStop(0, '#5a4c9a'); cg.addColorStop(1, '#241c4e'); }
      ctx.fillStyle = cg;
      ctx.shadowColor = cleared ? '#2dff8a' : next ? '#ffc94d' : 'transparent';
      ctx.shadowBlur = cleared || next ? 18 : 0;
      ctx.beginPath(); ctx.arc(0, 0, rr, 0, Math.PI * 2); ctx.fill();
      ctx.shadowBlur = 0;
      ctx.strokeStyle = 'rgba(255,255,255,0.35)';
      ctx.lineWidth = 2;
      ctx.beginPath(); ctx.arc(0, 0, rr * 0.78, 0, Math.PI * 2); ctx.stroke();
      ctx.restore();
      ctx.font = `800 ${Math.max(10, LW * 0.13)}px Orbitron, sans-serif`;
      ctx.textAlign = 'center';
      ctx.textBaseline = 'middle';
      ctx.fillStyle = cleared || next ? '#1a0e00' : '#d8d4ff';
      ctx.fillText(fmtMult(m), cx, cy + 1);
      // lane number
      ctx.fillStyle = 'rgba(200, 200, 255, 0.25)';
      ctx.font = `700 ${Math.max(9, LW * 0.11)}px Orbitron, sans-serif`;
      ctx.fillText(String(i + 1), cx, H - LW * 0.18);
      // barrier for barricaded lanes
      if (round && i <= round.lane && !(round.over && round.payout <= 0 && i === round.lane)) {
        const dir = laneDir(i);
        const by = dir === 1 ? boltY - LW * 0.95 : boltY + LW * 0.75;
        drawBarrier(x0 + LW * 0.1, by, LW * 0.8, LW * 0.16);
      }
    }
    // finish zone
    const fz = finishX;
    const cell = LW * 0.2;
    for (let y = 0; y < H; y += cell) for (let k = 0; k < 2; k++) {
      ctx.fillStyle = (Math.floor(y / cell) + k) % 2 ? 'rgba(255,255,255,0.75)' : 'rgba(20,16,50,0.9)';
      ctx.fillRect(fz + k * cell, y, cell, cell);
    }
    const fg = ctx.createLinearGradient(fz, 0, fz + LW * 1.8, 0);
    fg.addColorStop(0, 'rgba(45, 255, 138, 0.25)');
    fg.addColorStop(1, 'rgba(45, 255, 138, 0.02)');
    ctx.fillStyle = fg;
    ctx.fillRect(fz + cell * 2, 0, LW * 1.8, H);
    ctx.save();
    ctx.translate(fz + cell * 2 + LW * 0.75, boltY - LW * 1.2);
    ctx.fillStyle = '#2dff8a';
    ctx.shadowColor = '#2dff8a';
    ctx.shadowBlur = 16;
    ctx.font = `800 ${Math.max(12, LW * 0.17)}px Orbitron, sans-serif`;
    ctx.textAlign = 'center';
    ctx.fillText('FINISH', 0, 0);
    ctx.font = `700 ${Math.max(10, LW * 0.12)}px Orbitron, sans-serif`;
    ctx.fillText(fmtMult(chickenMultiplier(diffNow(), n)), 0, LW * 0.24);
    ctx.restore();
    ctx.shadowBlur = 0;
    // cars
    for (const c of cars) drawCar(c, LW);
    // bolt
    const bp = boltPos();
    drawBolt(bp.x, bp.y, LW * 0.62, bp.squash);
    ctx.restore();
  }

  function drawBarrier(x: number, y: number, w: number, hh: number) {
    ctx.save();
    ctx.shadowColor = '#ffc94d';
    ctx.shadowBlur = 12;
    ctx.fillStyle = '#ffc94d';
    ctx.fillRect(x, y, w, hh);
    ctx.shadowBlur = 0;
    ctx.beginPath();
    ctx.rect(x, y, w, hh);
    ctx.clip();
    ctx.fillStyle = '#1a1030';
    for (let k = -2; k < 12; k++) {
      ctx.beginPath();
      const sx = x + k * hh * 1.4;
      ctx.moveTo(sx, y + hh); ctx.lineTo(sx + hh * 0.7, y + hh); ctx.lineTo(sx + hh * 1.4, y); ctx.lineTo(sx + hh * 0.7, y);
      ctx.fill();
    }
    ctx.restore();
  }

  function drawCar(c: Car, LW: number) {
    const { startW } = geo();
    const cx = startW + LW * (c.lane + 0.5);
    const w = LW * (c.truck ? 0.62 : 0.54);
    const x = cx - w / 2;
    const y = c.y - c.len / 2;
    ctx.save();
    ctx.globalAlpha = Math.max(0, Math.min(1, c.fade));
    // headlight beams
    const front = c.dir === 1 ? y + c.len : y;
    const beam = ctx.createLinearGradient(0, front, 0, front + c.dir * LW * 1.1);
    beam.addColorStop(0, 'rgba(255, 250, 210, 0.35)');
    beam.addColorStop(1, 'rgba(255, 250, 210, 0)');
    ctx.fillStyle = beam;
    ctx.beginPath();
    ctx.moveTo(x + w * 0.1, front);
    ctx.lineTo(x + w * 0.9, front);
    ctx.lineTo(x + w * 1.25, front + c.dir * LW * 1.1);
    ctx.lineTo(x - w * 0.25, front + c.dir * LW * 1.1);
    ctx.fill();
    // hover glow
    ctx.fillStyle = c.truck ? 'rgba(34, 228, 255, 0.25)' : 'rgba(0,0,0,0.35)';
    ctx.beginPath(); ctx.ellipse(cx, c.y, w * 0.7, c.len * 0.55, 0, 0, Math.PI * 2); ctx.fill();
    // body
    const g = ctx.createLinearGradient(x, 0, x + w, 0);
    g.addColorStop(0, shade(c.color, -0.35));
    g.addColorStop(0.5, c.color);
    g.addColorStop(1, shade(c.color, -0.35));
    ctx.fillStyle = g;
    ctx.shadowColor = c.color;
    ctx.shadowBlur = 16;
    rr(x, y, w, c.len, w * 0.3);
    ctx.fill();
    ctx.shadowBlur = 0;
    if (c.truck) {
      // container stripes
      ctx.fillStyle = 'rgba(0,0,0,0.25)';
      const cy0 = c.dir === 1 ? y + c.len * 0.04 : y + c.len * 0.36;
      for (let k = 0; k < 4; k++) ctx.fillRect(x + w * 0.12, cy0 + k * c.len * 0.15, w * 0.76, c.len * 0.06);
    }
    // windshield
    ctx.fillStyle = 'rgba(10, 8, 30, 0.85)';
    const ws = c.dir === 1 ? y + c.len * (c.truck ? 0.72 : 0.58) : y + c.len * (c.truck ? 0.1 : 0.16);
    rr(x + w * 0.14, ws, w * 0.72, c.len * (c.truck ? 0.16 : 0.24), w * 0.15);
    ctx.fill();
    // lights
    const hl = c.dir === 1 ? y + c.len - 5 : y + 2;
    const tl = c.dir === 1 ? y + 2 : y + c.len - 5;
    ctx.fillStyle = '#fffbe0';
    ctx.shadowColor = '#fffbe0';
    ctx.shadowBlur = 10;
    ctx.fillRect(x + w * 0.1, hl, w * 0.22, 3);
    ctx.fillRect(x + w * 0.68, hl, w * 0.22, 3);
    ctx.fillStyle = '#ff2050';
    ctx.shadowColor = '#ff2050';
    ctx.fillRect(x + w * 0.1, tl, w * 0.2, 3);
    ctx.fillRect(x + w * 0.7, tl, w * 0.2, 3);
    ctx.restore();
  }

  function drawBolt(x: number, y: number, s: number, squash: number) {
    const dead = boltState === 'dead';
    ctx.save();
    // shadow
    ctx.fillStyle = 'rgba(0,0,0,0.45)';
    const { boltY } = geo();
    ctx.beginPath(); ctx.ellipse(x, boltY + s * 0.5, s * 0.45, s * 0.12, 0, 0, Math.PI * 2); ctx.fill();
    ctx.translate(x, y);
    if (dead) {
      ctx.rotate(Math.min(1, deadT * 4) * 1.3);
      ctx.scale(1, 1 - Math.min(0.45, deadT * 2));
    } else ctx.scale(1 / squash, squash);
    const metal = (x0: number, y0: number, r: number) => {
      const g = ctx.createRadialGradient(x0 - r * 0.35, y0 - r * 0.4, r * 0.1, x0, y0, r * 1.2);
      if (dead) { g.addColorStop(0, '#bdbdd0'); g.addColorStop(1, '#3d3b55'); }
      else { g.addColorStop(0, '#ffffff'); g.addColorStop(0.35, '#9ff3ff'); g.addColorStop(1, '#2a6fd6'); }
      return g;
    };
    const glow = dead ? '#666' : '#ff3d9a';
    // ears
    for (const side of [-1, 1]) {
      ctx.save();
      ctx.translate(side * s * 0.17, -s * 0.42);
      ctx.rotate(side * (0.22 + Math.sin(blink * 3) * 0.04));
      ctx.fillStyle = metal(0, -s * 0.3, s * 0.4);
      rr(-s * 0.09, -s * 0.62, s * 0.18, s * 0.66, s * 0.09);
      ctx.fill();
      ctx.fillStyle = glow;
      ctx.shadowColor = glow;
      ctx.shadowBlur = dead ? 0 : 12;
      rr(-s * 0.035, -s * 0.54, s * 0.07, s * 0.44, s * 0.035);
      ctx.fill();
      ctx.beginPath(); ctx.arc(0, -s * 0.6, s * 0.05, 0, Math.PI * 2); ctx.fill();
      ctx.restore();
    }
    // feet
    ctx.fillStyle = dead ? '#555' : '#1f4fb0';
    ctx.beginPath(); ctx.ellipse(-s * 0.17, s * 0.47, s * 0.13, s * 0.07, 0, 0, Math.PI * 2); ctx.fill();
    ctx.beginPath(); ctx.ellipse(s * 0.17, s * 0.47, s * 0.13, s * 0.07, 0, 0, Math.PI * 2); ctx.fill();
    // body
    ctx.fillStyle = metal(0, s * 0.22, s * 0.4);
    ctx.beginPath(); ctx.ellipse(0, s * 0.22, s * 0.32, s * 0.27, 0, 0, Math.PI * 2); ctx.fill();
    // chest light
    ctx.fillStyle = dead ? '#444' : '#2dff8a';
    ctx.shadowColor = '#2dff8a';
    ctx.shadowBlur = dead ? 0 : 10;
    ctx.beginPath(); ctx.arc(0, s * 0.24, s * 0.06, 0, Math.PI * 2); ctx.fill();
    ctx.shadowBlur = 0;
    // head
    ctx.fillStyle = metal(0, -s * 0.18, s * 0.36);
    ctx.beginPath(); ctx.arc(0, -s * 0.18, s * 0.31, 0, Math.PI * 2); ctx.fill();
    // visor
    ctx.fillStyle = '#0b0820';
    rr(-s * 0.23, -s * 0.28, s * 0.46, s * 0.17, s * 0.085);
    ctx.fill();
    // eyes
    if (dead) {
      ctx.strokeStyle = '#ff4d6d';
      ctx.lineWidth = Math.max(1.5, s * 0.03);
      for (const ex of [-0.1, 0.1]) {
        ctx.beginPath();
        ctx.moveTo((ex - 0.04) * s, -s * 0.24); ctx.lineTo((ex + 0.04) * s, -s * 0.15);
        ctx.moveTo((ex + 0.04) * s, -s * 0.24); ctx.lineTo((ex - 0.04) * s, -s * 0.15);
        ctx.stroke();
      }
    } else {
      const blinking = blink % 3.2 > 3.05;
      ctx.fillStyle = '#22e4ff';
      ctx.shadowColor = '#22e4ff';
      ctx.shadowBlur = 10;
      for (const ex of [-0.1, 0.1]) {
        if (boltState === 'win') {
          ctx.strokeStyle = '#22e4ff';
          ctx.lineWidth = Math.max(1.5, s * 0.03);
          ctx.beginPath();
          ctx.arc(ex * s, -s * 0.17, s * 0.04, Math.PI, 0);
          ctx.stroke();
        } else rr((ex - 0.035) * s, -s * (blinking ? 0.2 : 0.245), s * 0.07, s * (blinking ? 0.015 : 0.09), s * 0.02);
        ctx.fill();
      }
      ctx.shadowBlur = 0;
      // cheeks
      ctx.fillStyle = 'rgba(255, 61, 154, 0.7)';
      ctx.beginPath(); ctx.arc(-s * 0.2, -s * 0.06, s * 0.04, 0, Math.PI * 2); ctx.fill();
      ctx.beginPath(); ctx.arc(s * 0.2, -s * 0.06, s * 0.04, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
    // sparks when wrecked
    if (dead && deadT < 2.5 && Math.random() < 0.4) {
      ctx.save();
      ctx.strokeStyle = Math.random() < 0.5 ? '#ffc94d' : '#22e4ff';
      ctx.lineWidth = 2;
      const a = Math.random() * Math.PI * 2, l = s * rand(0.2, 0.5);
      ctx.beginPath();
      ctx.moveTo(x, y);
      ctx.lineTo(x + Math.cos(a) * l, y + Math.sin(a) * l);
      ctx.stroke();
      ctx.restore();
    }
  }

  function rr(x: number, y: number, w: number, hh: number, r: number) {
    r = Math.min(r, w / 2, hh / 2);
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + hh, r);
    ctx.arcTo(x + w, y + hh, x, y + hh, r);
    ctx.arcTo(x, y + hh, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  // ---------------------------------------------------------------- HUD / UI
  function syncUI() {
    const n = lanesCount();
    const crossed = round ? round.lane + 1 : 0;
    const lost = !!round && round.over && round.payout <= 0;
    hudLane.textContent = `${crossed} / ${n}`;
    hudMult.textContent = lost ? '0.00×' : fmtMult(crossed > 0 ? chickenMultiplier(diffNow(), crossed) : 1);
    hudMult.classList.toggle('lost', lost);
    hudNext.textContent = round && !round.over && crossed < n ? fmtMult(chickenMultiplier(diffNow(), crossed + 1)) : '—';
    const live = !!round && !round.over;
    hint.textContent = live ? (crossed === 0 ? 'Tap the road or press Hop to jump!' : '') : 'Place a bet to start';
    hint.classList.toggle('hide', live && crossed > 0);
    hopBtn.disabled = !live || !!hop || panel.isAuto || !round?.ready;
    if (live) {
      const payout = round2(round!.bet * chickenMultiplier(round!.diff, crossed));
      panel.setLabel(crossed > 0 ? `Cash out ${fmt(payout)}` : 'Hop to start');
      panel.setVariant(crossed > 0 ? 'cashout' : 'play');
      panel.setEnabled((crossed > 0 && !hop) || panel.isAuto);
    } else {
      panel.setLabel('Bet');
      panel.setVariant('play');
      panel.setEnabled(true);
    }
  }

  function showBanner(main: string, sub: string, kind: 'win' | 'lose') {
    banner.className = `rr-banner show ${kind}`;
    banner.replaceChildren(h('div.rr-banner-main', main), h('div.rr-banner-sub', sub));
    clearTimeout(bannerTimer);
    bannerTimer = setTimeout(() => banner.classList.remove('show'), 2200);
  }
  let bannerTimer: ReturnType<typeof setTimeout> | undefined;

  // ---------------------------------------------------------------- flow
  function animateHop(to: number) {
    return new Promise<void>((resolve) => {
      boltState = 'hop';
      hop = { from: boltLane, to, t: 0, dur: fast() ? 0.2 : 0.34, resolve };
      sound.play('step', 1 + Math.max(0, to) * 0.03);
    });
  }

  async function start(bet: number) {
    if (round && !round.over) return false;
    const { hit, lanes } = CHICKEN.difficulty[diff];
    if (!wallet.placeBet(meta.id, bet)) return false;
    const r: Round = { bet, diff, lanes, hitLane: lanes, lane: -1, over: false, payout: 0, pendingCrash: false, ready: false };
    round = r;
    boltLane = -1;
    boltState = 'idle';
    deadT = winT = 0;
    banner.classList.remove('show');
    panel.lock(true);
    syncUI();
    const { floats } = await fair.next(lanes);
    if (destroyed) throw new Abort();
    r.hitLane = chickenHitLane(floats, hit, lanes);
    r.ready = true;
    sound.play('whoosh');
    syncUI();
    return true;
  }

  async function doHop() {
    const r = round;
    if (!r || r.over || hop || !r.ready) return;
    const to = r.lane + 1;
    if (to >= r.lanes) return;
    const crash = to === r.hitLane;
    r.pendingCrash = crash;
    if (crash) {
      // a speeding hover-car timed to meet Bolt on landing
      const { LW, boltY } = geo();
      const dir = laneDir(to);
      const dur = fast() ? 0.2 : 0.34;
      const speed = H * 2.8;
      const len = LW * 1.05;
      cars.push({ lane: to, dir, len, truck: false, killer: true, color: '#ff3d6e', fade: 1, speed, y: boltY - dir * (speed * dur + len * 0.25) });
    }
    const p = animateHop(to);
    syncUI();
    await p;
    if (destroyed) throw new Abort();
    if (crash) {
      r.over = true;
      r.pendingCrash = false;
      r.payout = 0;
      boltState = 'dead';
      sound.play('boom');
      const rect = canvas.getBoundingClientRect();
      const bp = boltPos();
      fx.burst(rect.left + bp.x - camX, rect.top + bp.y, { count: 60, colors: ['#ff3d6e', '#ffc94d', '#22e4ff', '#ffffff'], speed: 480 });
      fx.shake(shell.stage, 1.6);
      fx.flash(shell.stage, 'lose');
      wallet.settle(meta.id, r.bet, 0, `Hit on lane ${to + 1}/${r.lanes} (${r.diff})`, 0);
      shell.addResult('0.00×', 'lose');
      showBanner('SMASH!', `Lane ${to + 1} had traffic`, 'lose');
      endRound();
      return;
    }
    r.lane = to;
    boltState = 'idle';
    const rect = canvas.getBoundingClientRect();
    const bp = boltPos();
    fx.burst(rect.left + bp.x - camX, rect.top + bp.y + geo().LW * 0.36, { count: 12, colors: ['#2dff8a', '#ffc94d', '#fff'], speed: 160, ring: false });
    sound.play('pop', 1 + to * 0.05);
    syncUI();
    if (r.lane >= r.lanes - 1) {
      await wait(fast() ? 100 : 250);
      await animateHop(r.lanes);
      await cashout(true);
    }
  }

  async function cashout(finished = false) {
    const r = round;
    if (!r || r.over || r.lane < 0 || hop) return;
    r.over = true;
    const mult = chickenMultiplier(r.diff, r.lane + 1);
    const payout = round2(r.bet * mult);
    r.payout = payout;
    boltState = 'win';
    winT = 0;
    sound.play('cashout');
    wallet.settle(meta.id, r.bet, payout, `${finished ? 'Finished' : 'Cashed out'} lane ${r.lane + 1}/${r.lanes} (${r.diff})`, mult);
    shell.addResult(fmtMult(mult), 'win');
    fx.pop(shell.stage, `+${fmt(payout - r.bet)}`, mult >= 10 || finished ? 'big' : 'win');
    fx.celebrate(shell.stage, payout, r.bet, mult);
    if (finished && mult < 10) fx.confetti(160);
    showBanner(finished ? 'MADE IT!' : fmtMult(mult), `You won ${fmt(payout)}`, 'win');
    endRound();
    setTimeout(() => {
      if (boltState === 'win' && round === r) boltState = 'idle';
    }, fast() ? 300 : 900);
  }

  function endRound() {
    if (!panel.isAuto) panel.lock(false);
    syncUI();
  }
  function onAbort(e: unknown) {
    if (!(e instanceof Abort)) console.error(e);
  }

  // ---------------------------------------------------------------- controls
  const diffSelect = h('select', (Object.keys(CHICKEN.difficulty) as ChickenDifficulty[]).map((d) => {
    const { hit, lanes } = CHICKEN.difficulty[d];
    return h('option', { value: d, selected: d === diff }, `${d[0].toUpperCase() + d.slice(1)} · ${lanes} lanes · ${Math.round(hit * 100)}% risk`);
  })) as HTMLSelectElement;
  diffSelect.addEventListener('change', () => {
    diff = diffSelect.value as ChickenDifficulty;
    sound.play('click');
    round = null;
    boltLane = -1;
    boltState = 'idle';
    cars = [];
    spawnT.clear();
    syncUI();
  });
  const hopBtn = h('button.btn-secondary.rr-hop', { type: 'button', onclick: () => doHop().catch(onAbort) }, 'Hop ›') as HTMLButtonElement;
  hopBtn.dataset.keepEnabled = '1';
  const autoLaneInput = h('input', { type: 'number', min: '1', step: '1', value: '3' }) as HTMLInputElement;
  const onKey = (e: KeyboardEvent) => {
    if (e.key !== 'ArrowRight' || panel.isAuto) return;
    const t = e.target as HTMLElement;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) return;
    e.preventDefault();
    doHop().catch(onAbort);
  };
  window.addEventListener('keydown', onKey);

  const panel: BetPanel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: async (bet) => {
      if (round && !round.over) await cashout().catch(onAbort);
      else await start(bet).catch(onAbort);
    },
    autoExtra: h('div.field', h('label.field-label', 'Cash out at lane'), autoLaneInput),
    autoRound: async (bet) => {
      try {
        const ok = await start(bet);
        if (!ok || !round) return null;
        const r = round;
        const target = Math.max(1, Math.min(r.lanes, Math.floor(parseFloat(autoLaneInput.value) || 3)));
        while (!r.over && r.lane + 1 < target) {
          await wait(store.data.settings.reducedMotion ? 50 : 140);
          await doHop();
        }
        if (!r.over) await cashout();
        return { bet: r.bet, payout: r.payout };
      } catch (e) {
        if (e instanceof Abort) return null;
        throw e;
      }
    },
    autoDelay: 600,
    onAutoChange: (on) => {
      diffSelect.disabled = on;
      syncUI();
    },
  });
  panel.settings.append(h('div.field', h('label.field-label', 'Difficulty'), diffSelect), hopBtn);
  shell.controls.appendChild(panel.el);
  syncUI();

  return () => {
    destroyed = true;
    window.removeEventListener('keydown', onKey);
    const r = round;
    if (r && !r.over) {
      r.over = true;
      if (r.pendingCrash) wallet.settle(meta.id, r.bet, 0, `Hit on lane ${r.lane + 2} (left game)`, 0);
      else if (r.lane >= 0) {
        const mult = chickenMultiplier(r.diff, r.lane + 1);
        wallet.settle(meta.id, r.bet, round2(r.bet * mult), `Auto cash-out lane ${r.lane + 1} (left game)`, mult);
      } else wallet.refund(meta.id, r.bet);
    }
    stopLoop();
    offResize();
    panel.destroy();
    shell.destroy();
  };
}

/** Darken (-) or lighten (+) a hex colour. */
function shade(hex: string, amt: number) {
  const n = parseInt(hex.slice(1), 16);
  const f = (c: number) => Math.round(clamp(amt < 0 ? c * (1 + amt) : c + (255 - c) * amt, 0, 255));
  const r = f((n >> 16) & 255), g = f((n >> 8) & 255), b = f(n & 255);
  return `rgb(${r}, ${g}, ${b})`;
}
