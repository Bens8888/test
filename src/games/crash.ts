/** CRASH — the multiplier climbs until it crashes. Cash out before it does. */
import '../styles/games/crash.css';
import type { GameMeta } from './registry';
import { BOT_NAMES, CRASH } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { crashPoint } from '../engine/outcomes';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fitCanvas, floor2, fmt, fmtMult, fmtShort, h, rafLoop, rand, round2 } from '../engine/util';
import { gameShell } from '../ui/gameShell';

type Phase = 'betting' | 'flying' | 'crashed';
type Result = { bet: number; payout: number } | null;

interface PlayerBet {
  amount: number;
  target: number | null;
  status: 'queued' | 'placed' | 'active' | 'cashed' | 'lost';
  payout: number;
  cashMult: number;
  resolve?: (r: Result) => void;
}
interface Bot { name: string; bet: number; target: number; status: 'waiting' | 'cashed' | 'lost'; row: HTMLElement; out: HTMLElement }

const CRASHED_PAUSE = 3;

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Each round a rocket takes off and the multiplier climbs from 1.00×.',
      'At a random point it crashes. Cash out before the crash to win bet × multiplier.',
      'Set an auto cash-out target to cash out automatically when the multiplier reaches it.',
      'Bets are placed during the countdown. Bet mid-flight to join the next round.',
      'P(reaching x) ≈ 99% ÷ x — house edge 1%. Everyone else here is a bot with a made-up name.',
    ],
    stageClass: 'crash-stage',
  });

  // ---------------------------------------------------------------- state
  let phase: Phase = 'betting';
  let phaseT = 0;
  let crashAt = 1;
  let roundReady = false;
  let mult = 1;
  let player: PlayerBet | null = null;
  let queued: PlayerBet | null = null;
  let bots: Bot[] = [];
  let cashMark: { t: number; m: number } | null = null;
  let botMarks: { t: number; m: number }[] = [];
  let explosion: { x: number; y: number; t: number } | null = null;
  let destroyed = false;

  // ---------------------------------------------------------------- DOM
  const canvas = h('canvas.stage-canvas') as HTMLCanvasElement;
  const ctx = canvas.getContext('2d')!;
  const multEl = h('div.crash-mult', '1.00×');
  const labelEl = h('div.crash-label');
  const progress = h('div.crash-progress', h('i'));
  const overlay = h('div.crash-overlay', labelEl, multEl, progress);
  const chart = h('div.crash-chart', canvas, overlay);
  const playersHead = h('div.crash-players-head', h('span', 'Players'), h('span.muted.small'));
  const playersList = h('div.crash-players-list');
  const players = h('div.crash-players', playersHead, playersList);
  shell.stage.append(chart, players);

  let W = 0, H = 0;
  const offResize = fitCanvas(canvas, (w, hh) => { W = w; H = hh; });

  const stars = Array.from({ length: 90 }, () => ({ x: Math.random(), y: Math.random(), z: rand(0.2, 1) }));

  // ---------------------------------------------------------------- bots
  function spawnBots() {
    const names = [...BOT_NAMES].sort(() => Math.random() - 0.5).slice(0, CRASH.botCount);
    bots = names.map((name) => {
      const bet = Math.round(Math.exp(rand(Math.log(1), Math.log(2500))) * 100) / 100;
      const target = Math.max(1.05, round2(1 / (1 - Math.random() * 0.9) * rand(0.95, 1.4)));
      const out = h('span.cp-out', '—');
      const row = h('div.cp-row', h('span.cp-name', name), h('span.cp-bet', fmtShort(bet)), out);
      return { name, bet, target, status: 'waiting' as const, row, out };
    });
    botMarks = [];
    renderPlayers();
  }
  const youRow = h('div.cp-row.you', h('span.cp-name', 'You'), h('span.cp-bet'), h('span.cp-out', '—'));
  function renderPlayers() {
    const rows: HTMLElement[] = [];
    if (player && player.status !== 'queued') {
      (youRow.children[1] as HTMLElement).textContent = fmtShort(player.amount);
      const out = youRow.children[2] as HTMLElement;
      youRow.classList.toggle('cashed', player.status === 'cashed');
      youRow.classList.toggle('lost', player.status === 'lost');
      out.textContent = player.status === 'cashed' ? fmtMult(player.cashMult) : player.status === 'lost' ? '✕' : '—';
      rows.push(youRow);
    }
    rows.push(...bots.map((b) => b.row));
    playersList.replaceChildren(...rows);
    const total = bots.reduce((a, b) => a + b.bet, 0) + (player && player.status !== 'queued' ? player.amount : 0);
    (playersHead.children[1] as HTMLElement).textContent = `${bots.length + (player && player.status !== 'queued' ? 1 : 0)} · ${fmtShort(round2(total))} chips`;
  }
  function botCash(b: Bot) {
    b.status = 'cashed';
    b.row.classList.add('cashed');
    b.out.textContent = `${fmtMult(b.target)} +${fmtShort(round2(b.bet * b.target - b.bet))}`;
    botMarks.push({ t: Math.log(b.target) / CRASH.growth, m: b.target });
  }

  // ---------------------------------------------------------------- round lifecycle
  function startBetting() {
    phase = 'betting';
    phaseT = 0;
    roundReady = false;
    mult = 1;
    cashMark = null;
    explosion = null;
    chart.classList.remove('crashed', 'flying');
    chart.classList.add('betting');
    fair.next(1).then((r) => {
      crashAt = crashPoint(r.floats[0]);
      roundReady = true;
    });
    if (queued) {
      const q = queued;
      queued = null;
      if (wallet.placeBet(meta.id, q.amount)) {
        q.status = 'placed';
        player = q;
        sound.play('chip');
      } else {
        q.resolve?.(null);
      }
    }
    spawnBots();
    updateButton(true);
  }

  function startFlight() {
    phase = 'flying';
    phaseT = 0;
    mult = 1;
    chart.classList.remove('betting');
    chart.classList.add('flying');
    if (player?.status === 'placed') player.status = 'active';
    sound.play('whoosh');
    renderPlayers();
    updateButton(true);
    if (crashAt <= 1) crash();
  }

  function cashout(at: number) {
    const p = player;
    if (!p || p.status !== 'active') return;
    p.status = 'cashed';
    p.cashMult = at;
    p.payout = round2(p.amount * at);
    cashMark = { t: Math.log(at) / CRASH.growth, m: at };
    wallet.settle(meta.id, p.amount, p.payout, `Cashed out @ ${fmtMult(at)}`, at);
    sound.play('cashout');
    fx.pop(chart, `+${fmt(p.payout - p.amount)}`, at >= 10 ? 'big' : 'win');
    fx.celebrate(shell.stage, p.payout, p.amount, at);
    renderPlayers();
    updateButton(true);
    // the bet is settled — let Autobet move on (its next bet joins the next round)
    const r = p.resolve;
    p.resolve = undefined;
    r?.({ bet: p.amount, payout: p.payout });
  }

  function crash() {
    phase = 'crashed';
    phaseT = 0;
    mult = crashAt;
    chart.classList.remove('flying');
    chart.classList.add('crashed');
    const pos = rocketPos();
    explosion = { x: pos.x, y: pos.y, t: 0 };
    sound.play('boom');
    fx.shake(shell.stage, crashAt > 3 ? 1.4 : 0.9);
    fx.flash(chart, 'lose');
    const rect = canvas.getBoundingClientRect();
    fx.burst(rect.left + pos.x, rect.top + pos.y, { count: 46, colors: ['#ff3d6e', '#ff8a3d', '#ffc94d', '#fff'], speed: 460 });
    for (const b of bots) if (b.status === 'waiting') { b.status = 'lost'; b.row.classList.add('lost'); b.out.textContent = '✕'; }
    if (player?.status === 'active') {
      player.status = 'lost';
      wallet.settle(meta.id, player.amount, 0, `Crashed @ ${fmtMult(crashAt)}`, 0);
      sound.play('lose');
    }
    shell.addResult(fmtMult(crashAt), crashAt >= 2 ? 'win' : 'lose');
    renderPlayers();
    // round over: resolve the player's bet
    if (player && (player.status === 'cashed' || player.status === 'lost')) {
      const p = player;
      player = null;
      p.resolve?.({ bet: p.amount, payout: p.payout });
    }
    updateButton(true);
  }

  // ---------------------------------------------------------------- main loop
  const stopLoop = rafLoop((dt) => {
    phaseT += dt;
    if (phase === 'betting') {
      if (phaseT >= CRASH.bettingSeconds && roundReady) startFlight();
    } else if (phase === 'flying') {
      const m = Math.exp(CRASH.growth * phaseT);
      // auto cash-outs happen at exactly the target if it's below the crash point
      if (player?.status === 'active' && player.target && player.target <= crashAt && m >= player.target) cashout(player.target);
      for (const b of bots) if (b.status === 'waiting' && b.target <= crashAt && m >= b.target) botCash(b);
      if (m >= crashAt) crash();
      else {
        if (Math.floor(m * 10) !== Math.floor(mult * 10) && m < 20) sound.play('tick', 0.6 + Math.min(1.4, m / 6));
        mult = m;
      }
    } else if (phaseT >= CRASHED_PAUSE) startBetting();
    if (explosion) explosion.t += dt;
    renderOverlay();
    updateButton();
    draw(dt);
  });

  // ---------------------------------------------------------------- drawing
  const PAD = { l: 48, r: 18, t: 20, b: 30 };
  function axes() {
    const t = phase === 'betting' ? 0 : phase === 'crashed' ? Math.log(crashAt) / CRASH.growth : phaseT;
    const xMax = Math.max(8, t * 1.18 + 0.5);
    const yMax = Math.max(2, (phase === 'betting' ? 1 : mult) * 1.25);
    return { xMax, yMax, t };
  }
  const px = (tau: number, xMax: number) => PAD.l + (tau / xMax) * (W - PAD.l - PAD.r);
  const py = (m: number, yMax: number) => H - PAD.b - ((m - 1) / (yMax - 1)) * (H - PAD.t - PAD.b);
  function rocketPos() {
    const { xMax, yMax, t } = axes();
    return { x: px(t, xMax), y: py(Math.exp(CRASH.growth * t), yMax) };
  }
  function niceStep(range: number, maxTicks: number) {
    const steps = [0.1, 0.2, 0.5, 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 50000, 100000];
    return steps.find((s) => range / s <= maxTicks) ?? 1e6;
  }

  function draw(dt: number) {
    if (!W || !H) return;
    ctx.clearRect(0, 0, W, H);
    const reduced = store.data.settings.reducedMotion;
    // stars
    const speed = phase === 'flying' ? 0.04 + Math.log(mult) * 0.06 : 0.01;
    ctx.fillStyle = '#fff';
    for (const s of stars) {
      if (!reduced) {
        s.x -= speed * s.z * dt * 3;
        s.y += speed * s.z * dt * 1.5;
        if (s.x < 0) s.x += 1;
        if (s.y > 1) s.y -= 1;
      }
      ctx.globalAlpha = 0.25 + s.z * 0.5;
      const len = phase === 'flying' && !reduced ? Math.min(18, speed * 120 * s.z) : 0;
      if (len > 2) {
        ctx.strokeStyle = '#cfe9ff';
        ctx.lineWidth = s.z;
        ctx.beginPath();
        ctx.moveTo(s.x * W, s.y * H);
        ctx.lineTo(s.x * W + len, s.y * H - len * 0.5);
        ctx.stroke();
      } else {
        ctx.beginPath();
        ctx.arc(s.x * W, s.y * H, s.z * 1.3, 0, Math.PI * 2);
        ctx.fill();
      }
    }
    ctx.globalAlpha = 1;

    const { xMax, yMax, t } = axes();
    // grid & labels
    ctx.font = '600 11px Orbitron, sans-serif';
    ctx.textBaseline = 'middle';
    ctx.lineWidth = 1;
    const yStep = niceStep(yMax - 1, 5);
    ctx.textAlign = 'right';
    for (let v = Math.ceil(1 / yStep) * yStep; v <= yMax; v += yStep) {
      const y = py(v, yMax);
      if (y < PAD.t - 2) continue;
      ctx.strokeStyle = 'rgba(155, 140, 255, 0.12)';
      ctx.beginPath(); ctx.moveTo(PAD.l, y); ctx.lineTo(W - PAD.r, y); ctx.stroke();
      ctx.fillStyle = 'rgba(200, 200, 240, 0.6)';
      ctx.fillText(`${+v.toFixed(1)}×`, PAD.l - 8, y);
    }
    const xStep = niceStep(xMax, 6);
    ctx.textAlign = 'center';
    for (let s = xStep; s <= xMax; s += xStep) {
      const x = px(s, xMax);
      ctx.strokeStyle = 'rgba(155, 140, 255, 0.07)';
      ctx.beginPath(); ctx.moveTo(x, PAD.t); ctx.lineTo(x, H - PAD.b); ctx.stroke();
      ctx.fillStyle = 'rgba(200, 200, 240, 0.5)';
      ctx.fillText(`${s}s`, x, H - PAD.b + 16);
    }
    ctx.strokeStyle = 'rgba(200, 200, 240, 0.3)';
    ctx.beginPath(); ctx.moveTo(PAD.l, PAD.t); ctx.lineTo(PAD.l, H - PAD.b); ctx.lineTo(W - PAD.r, H - PAD.b); ctx.stroke();

    // curve
    const crashed = phase === 'crashed';
    if (phase !== 'betting' && t > 0) {
      const N = 90;
      ctx.beginPath();
      for (let i = 0; i <= N; i++) {
        const tau = (t * i) / N;
        const x = px(tau, xMax), y = py(Math.exp(CRASH.growth * tau), yMax);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      const end = { x: px(t, xMax), y: py(Math.exp(CRASH.growth * t), yMax) };
      // area fill
      ctx.save();
      ctx.lineTo(end.x, H - PAD.b);
      ctx.lineTo(PAD.l, H - PAD.b);
      ctx.closePath();
      const fill = ctx.createLinearGradient(0, end.y, 0, H - PAD.b);
      fill.addColorStop(0, crashed ? 'rgba(255, 61, 110, 0.35)' : 'rgba(255, 61, 154, 0.38)');
      fill.addColorStop(1, 'rgba(155, 92, 255, 0.02)');
      ctx.fillStyle = fill;
      ctx.fill();
      ctx.restore();
      // stroke
      ctx.beginPath();
      for (let i = 0; i <= N; i++) {
        const tau = (t * i) / N;
        const x = px(tau, xMax), y = py(Math.exp(CRASH.growth * tau), yMax);
        if (i === 0) ctx.moveTo(x, y); else ctx.lineTo(x, y);
      }
      const grad = ctx.createLinearGradient(PAD.l, 0, end.x, 0);
      if (crashed) { grad.addColorStop(0, '#ff3d6e'); grad.addColorStop(1, '#ff8a3d'); }
      else { grad.addColorStop(0, '#9b5cff'); grad.addColorStop(0.6, '#ff3d9a'); grad.addColorStop(1, '#ffc94d'); }
      ctx.strokeStyle = grad;
      ctx.lineWidth = 4;
      ctx.lineCap = 'round';
      ctx.shadowColor = crashed ? '#ff3d6e' : '#ff3d9a';
      ctx.shadowBlur = 18;
      ctx.stroke();
      ctx.shadowBlur = 0;

      // bot cash-out dots
      for (const bm of botMarks) {
        if (bm.t > t) continue;
        ctx.fillStyle = 'rgba(45, 255, 138, 0.85)';
        ctx.beginPath(); ctx.arc(px(bm.t, xMax), py(bm.m, yMax), 3, 0, Math.PI * 2); ctx.fill();
      }
      // player cash-out flag
      if (cashMark) {
        const cx = px(cashMark.t, xMax), cy = py(cashMark.m, yMax);
        ctx.fillStyle = '#2dff8a';
        ctx.shadowColor = '#2dff8a'; ctx.shadowBlur = 14;
        ctx.beginPath(); ctx.arc(cx, cy, 6, 0, Math.PI * 2); ctx.fill();
        ctx.shadowBlur = 0;
        ctx.font = '800 11px Orbitron, sans-serif';
        ctx.textAlign = 'center';
        const label = `YOU ${fmtMult(cashMark.m)}`;
        const tw = ctx.measureText(label).width + 14;
        ctx.fillStyle = 'rgba(10, 40, 25, 0.9)';
        ctx.strokeStyle = '#2dff8a';
        roundRect(ctx, cx - tw / 2, cy - 34, tw, 20, 6); ctx.fill(); ctx.stroke();
        ctx.fillStyle = '#2dff8a';
        ctx.fillText(label, cx, cy - 24);
      }

      if (!crashed) {
        // rocket oriented along the curve
        const t2 = Math.max(0, t - 0.25);
        const prev = { x: px(t2, xMax), y: py(Math.exp(CRASH.growth * t2), yMax) };
        const ang = Math.atan2(end.y - prev.y, end.x - prev.x);
        drawRocket(end.x, end.y, t > 0.05 ? ang : -0.5);
      }
    } else if (phase === 'betting') {
      drawRocket(PAD.l + 18, H - PAD.b - 14, -Math.PI / 4, true);
    }

    // explosion shockwave
    if (explosion && explosion.t < 0.9) {
      const k = explosion.t / 0.9;
      const r = 12 + k * 90;
      const g = ctx.createRadialGradient(explosion.x, explosion.y, 0, explosion.x, explosion.y, r);
      g.addColorStop(0, `rgba(255, 240, 200, ${0.9 * (1 - k)})`);
      g.addColorStop(0.4, `rgba(255, 138, 61, ${0.6 * (1 - k)})`);
      g.addColorStop(1, 'rgba(255, 61, 110, 0)');
      ctx.fillStyle = g;
      ctx.beginPath(); ctx.arc(explosion.x, explosion.y, r, 0, Math.PI * 2); ctx.fill();
      ctx.strokeStyle = `rgba(255, 201, 77, ${1 - k})`;
      ctx.lineWidth = 3 * (1 - k);
      ctx.beginPath(); ctx.arc(explosion.x, explosion.y, r * 1.2, 0, Math.PI * 2); ctx.stroke();
    }
  }

  function drawRocket(x: number, y: number, ang: number, idle = false) {
    const s = Math.max(0.75, Math.min(1.2, W / 700));
    ctx.save();
    ctx.translate(x, y);
    ctx.rotate(ang);
    ctx.scale(s, s);
    // flame
    const flick = idle ? 0.35 : 0.8 + Math.random() * 0.5;
    const fg = ctx.createLinearGradient(-14, 0, -14 - 30 * flick, 0);
    fg.addColorStop(0, '#fff3b0'); fg.addColorStop(0.4, '#ff8a3d'); fg.addColorStop(1, 'rgba(255, 61, 154, 0)');
    ctx.fillStyle = fg;
    ctx.beginPath(); ctx.moveTo(-12, -6); ctx.quadraticCurveTo(-14 - 34 * flick, 0, -12, 6); ctx.closePath(); ctx.fill();
    // fins
    ctx.fillStyle = '#ff3d9a';
    ctx.beginPath(); ctx.moveTo(-8, -6); ctx.lineTo(-16, -14); ctx.lineTo(-2, -7); ctx.closePath(); ctx.fill();
    ctx.beginPath(); ctx.moveTo(-8, 6); ctx.lineTo(-16, 14); ctx.lineTo(-2, 7); ctx.closePath(); ctx.fill();
    // body
    const bg = ctx.createLinearGradient(0, -8, 0, 8);
    bg.addColorStop(0, '#ffffff'); bg.addColorStop(1, '#a9abd6');
    ctx.fillStyle = bg;
    ctx.shadowColor = 'rgba(255, 255, 255, 0.7)'; ctx.shadowBlur = 12;
    ctx.beginPath(); ctx.moveTo(18, 0); ctx.quadraticCurveTo(8, -9, -12, -7); ctx.lineTo(-12, 7); ctx.quadraticCurveTo(8, 9, 18, 0); ctx.fill();
    ctx.shadowBlur = 0;
    // window
    ctx.fillStyle = '#22e4ff';
    ctx.strokeStyle = '#5b3dff'; ctx.lineWidth = 2;
    ctx.beginPath(); ctx.arc(4, 0, 3.4, 0, Math.PI * 2); ctx.fill(); ctx.stroke();
    ctx.restore();
  }

  function renderOverlay() {
    if (phase === 'betting') {
      const left = Math.max(0, CRASH.bettingSeconds - phaseT);
      labelEl.textContent = 'Next round in';
      multEl.textContent = `${left.toFixed(1)}s`;
      (progress.firstElementChild as HTMLElement).style.width = `${(left / CRASH.bettingSeconds) * 100}%`;
    } else if (phase === 'flying') {
      labelEl.textContent = player?.status === 'active' ? `Your bet ${fmt(player.amount)} → ${fmt(floor2(player.amount * mult))}` : '';
      multEl.textContent = fmtMult(floor2(mult));
      const heat = Math.min(1, Math.log(mult) / Math.log(20));
      multEl.style.setProperty('--heat', String(heat));
    } else {
      labelEl.textContent = 'CRASHED';
      multEl.textContent = fmtMult(crashAt);
    }
  }

  // ---------------------------------------------------------------- betting actions
  const parseTarget = () => {
    const v = parseFloat(targetInput.value);
    return v >= 1.01 ? round2(v) : null;
  };

  function placeOrQueue(amount: number, target: number | null, resolve?: (r: Result) => void): boolean {
    const b: PlayerBet = { amount, target, status: 'queued', payout: 0, cashMult: 0, resolve };
    if (phase === 'betting' && !player) {
      if (!wallet.placeBet(meta.id, amount)) return false;
      b.status = 'placed';
      player = b;
      sound.play('chip');
      renderPlayers();
    } else {
      if (amount <= 0 || !wallet.canAfford(amount)) {
        wallet.placeBet(meta.id, amount); // triggers the "insufficient" feedback
        return false;
      }
      queued = b;
      if (!panel.isAuto) toast('Bet queued for the next round', 'info');
    }
    updateButton(true);
    return true;
  }

  function onAction(bet: number) {
    if (player?.status === 'active') {
      cashout(floor2(mult));
      return;
    }
    if (player?.status === 'placed' && phase === 'betting') {
      wallet.refund(meta.id, player.amount);
      player.resolve?.(null);
      player = null;
      sound.play('click');
      renderPlayers();
      updateButton(true);
      return;
    }
    if (queued) {
      queued.resolve?.(null);
      queued = null;
      updateButton(true);
      return;
    }
    if (player) return; // cashed this round — wait for the next one
    placeOrQueue(bet, parseTarget());
  }

  function autoRound(bet: number): Promise<Result> {
    return new Promise((resolve) => {
      const target = parseTarget();
      if (!target) {
        toast('Set an auto cash-out target (≥ 1.01×) for Autobet', 'error');
        resolve(null);
        return;
      }
      if (destroyed || queued || (player && player.status !== 'cashed' && player.status !== 'lost')) {
        resolve(null);
        return;
      }
      if (player) {
        // still finishing this round — queue for the next one
        queued = { amount: bet, target, status: 'queued', payout: 0, cashMult: 0, resolve };
        return;
      }
      if (!placeOrQueue(bet, target, resolve)) resolve(null);
    });
  }

  // ---------------------------------------------------------------- panel
  const targetInput = h('input', { type: 'number', step: '0.01', min: '1.01', inputMode: 'decimal', value: '2.00', placeholder: 'off' }) as HTMLInputElement;
  targetInput.addEventListener('input', () => {
    if (player && (player.status === 'placed' || player.status === 'active')) player.target = parseTarget();
    if (queued) queued.target = parseTarget();
    updateHint();
  });
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: (bet) => onAction(bet),
    autoRound,
    autoDelay: 50,
    onAutoChange: () => updateButton(true),
  });
  panel.settings.append(
    h('div.field', h('label.field-label', h('span', 'Auto cash out'), h('span.muted.small', 'empty = off')), h('div.suffix-input', targetInput, h('span', '×'))),
  );
  const updateHint = () => {
    const t = parseTarget();
    panel.setProfitHint(t ? `Win at ${fmtMult(t)}: +${fmt(Math.max(0, panel.bet * t - panel.bet))}` : '');
  };
  panel.onBetChange = updateHint;
  shell.controls.appendChild(panel.el);
  updateHint();

  let lastLabel = '';
  function updateButton(force = false) {
    if (panel.isAuto) {
      lastLabel = '';
      return;
    }
    let label: string, variant: 'play' | 'cashout' | 'danger' = 'play', enabled = true;
    if (player?.status === 'active') {
      label = `Cash out @ ${fmtMult(floor2(mult))}`;
      variant = 'cashout';
    } else if (player?.status === 'placed') {
      label = 'Cancel bet';
      variant = 'danger';
    } else if (queued) {
      label = 'Cancel (next round)';
      variant = 'danger';
    } else if (player) {
      label = `Cashed ${fmtMult(player.cashMult)}`;
      // in the Auto tab the button starts Autobet, which simply joins the next round
      const autoBox = panel.el.querySelector('.auto-box') as HTMLElement | null;
      enabled = !!autoBox && !autoBox.hidden;
    } else label = phase === 'betting' ? 'Bet' : 'Bet (next round)';
    const key = `${label}|${variant}|${enabled}`;
    if (!force && key === lastLabel) return;
    lastLabel = key;
    panel.setLabel(label);
    panel.setVariant(variant);
    panel.setEnabled(enabled);
  }

  startBetting();

  return () => {
    destroyed = true;
    stopLoop();
    offResize();
    // make sure no chips are ever lost when leaving
    if (player?.status === 'placed') {
      wallet.refund(meta.id, player.amount);
      player.resolve?.(null);
    } else if (player?.status === 'active') {
      cashout(floor2(mult));
      player?.resolve?.({ bet: player.amount, payout: player.payout });
    } else if (player) player.resolve?.({ bet: player.amount, payout: player.payout });
    queued?.resolve?.(null);
    player = null;
    queued = null;
    panel.destroy();
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
