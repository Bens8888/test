/** CRAPS — full table: pass/don't pass, come/don't come, odds, field and place bets. */
import '../styles/games/craps.css';
import type { GameMeta } from './registry';
import { CRAPS } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { dieFromFloat } from '../engine/outcomes';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fmt, fmtShort, h, rand, round2, sleep } from '../engine/util';
import { gameShell } from '../ui/gameShell';

// ============================================================== pure rules
export const POINT_NUMS = [4, 5, 6, 8, 9, 10];
export type Bets = Record<string, number>;

/** Pass / come odds pay true odds (win per 1 staked). */
export const trueOdds = (n: number) => (n === 4 || n === 10 ? 2 : n === 5 || n === 9 ? 1.5 : 1.2);
/** Don't pass / don't come odds lay the true odds. */
export const layOdds = (n: number) => 1 / trueOdds(n);

export interface BetResult {
  key: string;
  stake: number;
  /** chips returned to the balance */
  ret: number;
  /** win/lose/push take the bet down; stay = place bet paid and left up; move = come bet travels to a number */
  outcome: 'win' | 'lose' | 'push' | 'stay' | 'move';
  to?: string;
}

export interface RollOutcome {
  bets: Bets;
  point: number | null;
  results: BetResult[];
  message: string;
  kind: 'natural' | 'craps' | 'point-set' | 'point-made' | 'seven-out' | 'roll';
  /** stake of bets taken down this roll (win/lose/push) */
  settledStake: number;
  /** stake of place bets that won and stay up */
  stayStake: number;
  returned: number;
}

const NAMES: Record<number, string> = { 2: 'Snake eyes', 3: 'Ace-deuce', 4: 'Four', 5: 'Five', 6: 'Six', 7: 'Seven', 8: 'Eight', 9: 'Nine', 10: 'Ten', 11: 'Yo-leven', 12: 'Boxcars' };

/** Resolve one roll against the bets on the table. Pure — unit tested. */
export function resolveRoll(bets: Bets, point: number | null, d1: number, d2: number): RollOutcome {
  const b: Bets = { ...bets };
  const results: BetResult[] = [];
  const total = d1 + d2;
  const comeOut = point === null;
  const take = (key: string, outcome: 'win' | 'lose' | 'push', winMult = 0) => {
    const s = b[key];
    if (!s) return;
    const ret = outcome === 'win' ? round2(s * (1 + winMult)) : outcome === 'push' ? s : 0;
    results.push({ key, stake: s, ret, outcome });
    delete b[key];
  };

  // one-roll field bet
  if (b.field) CRAPS.field[total] ? take('field', 'win', CRAPS.field[total]) : take('field', 'lose');

  // place bets — working only while a point is on
  if (!comeOut) {
    for (const n of POINT_NUMS) {
      const k = 'place' + n;
      const s = b[k];
      if (!s) continue;
      if (total === n) {
        const [w, st] = CRAPS.place[n];
        results.push({ key: k, stake: s, ret: round2((s * w) / st), outcome: 'stay' });
      } else if (total === 7) take(k, 'lose');
    }
  }

  // come / don't come bets already on numbers (come odds are off on the come-out roll)
  for (const n of POINT_NUMS) {
    if (total === n) {
      take('come' + n, 'win', 1);
      take('comeOdds' + n, comeOut ? 'push' : 'win', trueOdds(n));
      take('dc' + n, 'lose');
      take('dcOdds' + n, 'lose');
    } else if (total === 7) {
      take('come' + n, 'lose');
      take('comeOdds' + n, comeOut ? 'push' : 'lose');
      take('dc' + n, 'win', 1);
      take('dcOdds' + n, 'win', layOdds(n));
    }
  }

  // pending come / don't come bets
  if (b.come) {
    if (total === 7 || total === 11) take('come', 'win', 1);
    else if (total === 2 || total === 3 || total === 12) take('come', 'lose');
    else {
      results.push({ key: 'come', stake: b.come, ret: 0, outcome: 'move', to: 'come' + total });
      b['come' + total] = b.come;
      delete b.come;
    }
  }
  if (b.dontCome) {
    if (total === 2 || total === 3) take('dontCome', 'win', 1);
    else if (total === 12) take('dontCome', 'push');
    else if (total === 7 || total === 11) take('dontCome', 'lose');
    else {
      results.push({ key: 'dontCome', stake: b.dontCome, ret: 0, outcome: 'move', to: 'dc' + total });
      b['dc' + total] = b.dontCome;
      delete b.dontCome;
    }
  }

  // line bets
  let newPoint = point;
  let kind: RollOutcome['kind'] = 'roll';
  let message = `${NAMES[total]}${d1 === d2 && total >= 4 && total <= 10 ? ' — the hard way' : ''}`;
  if (comeOut) {
    if (total === 7 || total === 11) {
      take('pass', 'win', 1);
      take('dontPass', 'lose');
      kind = 'natural';
      message = total === 7 ? 'Natural 7! Pass wins' : 'Yo-leven! Pass wins';
    } else if (total === 2 || total === 3) {
      take('pass', 'lose');
      take('dontPass', 'win', 1);
      kind = 'craps';
      message = `${NAMES[total]} — craps! Don't Pass wins`;
    } else if (total === 12) {
      take('pass', 'lose');
      take('dontPass', 'push');
      kind = 'craps';
      message = "Boxcars — craps! Don't Pass pushes";
    } else {
      newPoint = total;
      kind = 'point-set';
      message = `Point is ${total}`;
    }
  } else if (total === point) {
    take('pass', 'win', 1);
    take('passOdds', 'win', trueOdds(total));
    take('dontPass', 'lose');
    take('dontOdds', 'lose');
    newPoint = null;
    kind = 'point-made';
    message = `Winner! Point ${total} made`;
  } else if (total === 7) {
    take('pass', 'lose');
    take('passOdds', 'lose');
    take('dontPass', 'win', 1);
    take('dontOdds', 'win', layOdds(point!));
    newPoint = null;
    kind = 'seven-out';
    message = 'Seven out!';
  }

  let settledStake = 0, stayStake = 0, returned = 0;
  for (const r of results) {
    if (r.outcome === 'win' || r.outcome === 'lose' || r.outcome === 'push') settledStake += r.stake;
    if (r.outcome === 'stay') stayStake += r.stake;
    returned += r.ret;
  }
  return { bets: b, point: newPoint, results, message, kind, settledStake: round2(settledStake), stayStake: round2(stayStake), returned: round2(returned) };
}

/** Max odds allowed behind a flat bet. */
export function oddsCap(flat: number, n: number) {
  return round2(flat * (CRAPS.maxOdds[n] ?? 3));
}

// ============================================================== UI
const CHIP_COLORS: Record<number, string> = { 1: '#cfd3e6', 5: '#e53950', 10: '#2f7cf6', 25: '#16a35a', 100: '#24213a', 500: '#8e44ff', 1000: '#f5b300' };
const chipColor = (amt: number) => {
  const d = CRAPS.chips.filter((c) => c <= amt);
  return CHIP_COLORS[d.length ? d[d.length - 1] : 1] ?? '#cfd3e6';
};
const PIPS: Record<number, number[]> = { 1: [5], 2: [1, 9], 3: [1, 5, 9], 4: [1, 3, 7, 9], 5: [1, 3, 5, 7, 9], 6: [1, 3, 4, 6, 7, 9] };
/** cube rotation that brings face v to the front */
const FACE_ROT: Record<number, [number, number]> = { 1: [0, 0], 2: [90, 0], 3: [0, -90], 4: [0, 90], 5: [-90, 0], 6: [0, 180] };

interface Slot { id: string; el: HTMLElement; chips: HTMLElement; keys: string[]; target: () => string | null; why: string }

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Come-out roll: Pass Line wins on 7 or 11, loses on 2, 3, 12. Don\'t Pass is the opposite, but 12 is a push.',
      'Any 4, 5, 6, 8, 9 or 10 sets the point (the ON puck). Roll the point again before a 7 and Pass wins; a 7 first is "seven out".',
      'Odds: once a point is on, back your Pass / Come bet with odds paying true odds — 2:1 on 4/10, 3:2 on 5/9, 6:5 on 6/8 (up to 3-4-5×). Lay odds on Don\'t bets pay the inverse.',
      'Come / Don\'t Come work like Pass / Don\'t Pass but start on any roll while a point is on; the chip travels to its number. Click a come chip on a number to add odds. Come odds are off on the come-out.',
      'Field (one roll): 3, 4, 9, 10, 11 pay 1:1, 2 pays 2:1, 12 pays 3:1. Place bets: 4/10 pay 9:5, 5/9 pay 7:5, 6/8 pay 7:6 — they stay up when they win and are off on the come-out.',
      'Chips leave your balance when placed. Right-click (or long-press) a bet to take it down. Line and come bets can\'t be removed once they have a point.',
    ],
    stageClass: 'craps-stage',
  });

  // ---------------------------------------------------------------- state
  let bets: Bets = {};
  let point: number | null = null;
  let chip = CRAPS.chips[2];
  let rolling = false;
  let destroyed = false;
  let pendingApply: (() => void) | null = null;
  const undoStack: { key: string; amount: number }[] = [];
  const slots: Slot[] = [];
  const tableTotal = () => round2(Object.values(bets).reduce((a, b) => a + b, 0));

  // ---------------------------------------------------------------- dice tray
  const makeDie = () => {
    const cube = h('div.cr-cube', [1, 2, 3, 4, 5, 6].map((v) =>
      h(`div.cr-face.f${v}`, Array.from({ length: 9 }, (_, i) => h(PIPS[v].includes(i + 1) ? 'span.pip' : 'span')))));
    const wrap = h('div.cr-die', h('div.cr-die-shadow'), cube);
    return { wrap, cube, rot: '' };
  };
  const dice = [makeDie(), makeDie()];
  const msg = h('div.cr-msg', 'Place your bets — come-out roll');
  const totalBadge = h('div.cr-total');
  const tray = h('div.cr-tray', h('div.cr-wall'), msg, totalBadge, dice[0].wrap, dice[1].wrap);

  const tilt = (i: number) => `rotateX(-15deg) rotateY(${i ? -17 : 17}deg)`;
  function placeDiceIdle() {
    const W = tray.clientWidth, H = tray.clientHeight, ds = dice[0].wrap.offsetWidth || 56;
    dice.forEach((d, i) => {
      d.wrap.style.transform = `translate(${W / 2 + (i ? 10 : -ds - 10)}px, ${H / 2 - ds / 2 + 10}px)`;
      if (!d.rot) d.rot = `${tilt(i)} rotateX(${FACE_ROT[i ? 3 : 4][0]}deg) rotateY(${FACE_ROT[i ? 3 : 4][1]}deg)`;
      d.cube.style.transform = d.rot;
    });
  }

  async function animateDice(v: [number, number], fast: boolean) {
    const W = tray.clientWidth, H = tray.clientHeight, ds = dice[0].wrap.offsetWidth || 56;
    const dur = store.data.settings.reducedMotion ? 250 : fast ? 750 : 1250;
    sound.play('roll');
    const anims: Promise<unknown>[] = [];
    dice.forEach((d, i) => {
      const fx0 = W / 2 + (i ? rand(6, 18) : -ds - rand(6, 18));
      const fy0 = H / 2 - ds / 2 + rand(-14, 18);
      const path = [
        { transform: `translate(${-ds - 20 - i * 30}px, ${H * 0.62 + i * 12}px)` },
        { transform: `translate(${W - ds - 14 - i * 6}px, ${H * (0.28 + i * 0.22)}px)`, offset: 0.5 },
        { transform: `translate(${W * 0.72 - i * 20}px, ${H * 0.45}px)`, offset: 0.72 },
        { transform: `translate(${fx0}px, ${fy0}px)` },
      ];
      const [rx, ry] = FACE_ROT[v[i]];
      const end = `${tilt(i)} rotateX(${rx}deg) rotateY(${ry}deg)`;
      const start = `${tilt(i)} rotateX(${rx - 720 - Math.round(rand(0, 3)) * 90}deg) rotateY(${ry + 540 + Math.round(rand(0, 3)) * 90}deg)`;
      const a1 = d.wrap.animate(path, { duration: dur, easing: 'cubic-bezier(.25,.75,.35,1)', fill: 'forwards' });
      const a2 = d.cube.animate([{ transform: start }, { transform: end }], { duration: dur, easing: 'cubic-bezier(.2,.65,.3,1)', fill: 'forwards' });
      d.rot = end;
      anims.push(a1.finished.then(() => { d.wrap.style.transform = path[path.length - 1].transform; a1.cancel(); }).catch(() => {}));
      anims.push(a2.finished.then(() => { d.cube.style.transform = end; a2.cancel(); }).catch(() => {}));
    });
    setTimeout(() => sound.play('land'), dur * 0.5);
    await Promise.all(anims);
    sound.play('land', 1.3);
  }

  // ---------------------------------------------------------------- table
  const slot = (id: string, cls: string, keys: string[], target: () => string | null, why: string, content: (HTMLElement | string | null)[], title = '') => {
    const chips = h('div.cr-chips');
    const el = h(`div.cr-spot.${cls}`, { title }, ...content, chips);
    const s: Slot = { id, el, chips, keys, target, why };
    slots.push(s);
    el.dataset.slot = id;
    el.addEventListener('click', (e) => { e.stopPropagation(); clickSlot(s); });
    el.addEventListener('contextmenu', (e) => { e.preventDefault(); e.stopPropagation(); removeFromSlot(s); });
    let lp: any = 0;
    el.addEventListener('pointerdown', (e) => {
      if (e.pointerType !== 'touch') return;
      lp = setTimeout(() => { lp = -1; removeFromSlot(s); }, 550);
    });
    const cancel = () => { if (lp > 0) clearTimeout(lp); };
    el.addEventListener('pointerup', cancel);
    el.addEventListener('pointerleave', cancel);
    el.addEventListener('click', (e) => { if (lp === -1) { e.stopImmediatePropagation(); lp = 0; } }, true);
    return el;
  };

  const numBoxes = new Map<number, HTMLElement>();
  const numLabel = (n: number) => (n === 6 ? 'SIX' : n === 9 ? 'NINE' : String(n));
  const numCols = POINT_NUMS.map((n) => {
    const [w, st] = CRAPS.place[n];
    const box = h('div.cr-numbox',
      slot('dc' + n, 'cr-dc', ['dc' + n, 'dcOdds' + n], () => (bets['dc' + n] ? 'dcOdds' + n : null), "Don't Come bets land here — click to lay odds on them", [h('span.cr-sub', 'DC')], `Don't Come on ${n}`),
      slot('place' + n, 'cr-place', ['place' + n], () => 'place' + n, '', [h('span.cr-numtxt', numLabel(n)), h('span.cr-pays', `${w}:${st}`)], `Place ${n} — pays ${w}:${st}`),
      slot('come' + n, 'cr-come', ['come' + n, 'comeOdds' + n], () => (bets['come' + n] ? 'comeOdds' + n : null), 'Come bets land here — click to add odds', [h('span.cr-sub', 'COME')], `Come on ${n}`),
    );
    numBoxes.set(n, box);
    return box;
  });
  const dontComeBar = slot('dontCome', 'cr-dontcome', ['dontCome'], () => (point ? 'dontCome' : null), "Don't Come opens once a point is set", [h('span.cr-label', "DON'T COME"), h('span.cr-small', 'BAR ⚀⚅')], "Don't Come — 1:1, 12 pushes");
  const come = slot('come', 'cr-comebox', ['come'], () => (point ? 'come' : null), 'Come bets open once a point is set', [h('span.cr-big', 'COME')], 'Come — 1:1');
  const field = slot('field', 'cr-field', ['field'], () => 'field', '', [
    h('span.cr-label', 'FIELD'),
    h('div.cr-field-nums', h('span.circ', '2', h('small', '2×')), h('span', '3'), h('span', '4'), h('span', '9'), h('span', '10'), h('span', '11'), h('span.circ', '12', h('small', '3×'))),
  ], 'Field — one roll');
  const dontPass = slot('dontPass', 'cr-dontpass', ['dontPass'], () => (point === null ? 'dontPass' : null), "Don't Pass bets are made on the come-out roll", [h('span.cr-label', "DON'T PASS BAR"), h('span.cr-small', '⚅⚅ push')], "Don't Pass — 1:1");
  const dontOdds = slot('dontOdds', 'cr-odds.dont', ['dontOdds'], () => (point && bets.dontPass ? 'dontOdds' : null), "Lay odds need a Don't Pass bet and a point", [h('span.cr-label', 'LAY ODDS')], "Don't Pass odds");
  const pass = slot('pass', 'cr-pass', ['pass'], () => (point === null ? 'pass' : null), 'Pass Line bets are made on the come-out roll', [h('span.cr-big', 'PASS LINE')], 'Pass Line — 1:1');
  const passOdds = slot('passOdds', 'cr-odds', ['passOdds'], () => (point && bets.pass ? 'passOdds' : null), 'Odds need a Pass Line bet and a point', [h('span.cr-label', 'PASS ODDS')], 'Pass odds — true odds');

  const puck = h('div.cr-puck.off', 'OFF');
  const table = h('div.cr-table', dontComeBar, ...numCols, come, field, dontPass, dontOdds, pass, passOdds, puck);
  const tableWrap = h('div.cr-felt', table);

  function positionPuck() {
    const target = point ? numBoxes.get(point)! : dontComeBar;
    puck.style.left = `${target.offsetLeft + target.offsetWidth / 2}px`;
    puck.style.top = `${target.offsetTop + (point ? 2 : target.offsetHeight - 2)}px`;
    puck.textContent = point ? 'ON' : 'OFF';
    puck.classList.toggle('off', !point);
  }
  const ro = new ResizeObserver(() => { positionPuck(); if (!rolling) placeDiceIdle(); });
  ro.observe(table);
  ro.observe(tray);

  // ---------------------------------------------------------------- chips toolbar
  const chipBar = h('div.rt-chipbar.cr-chipbar', CRAPS.chips.map((v) => {
    const b = h(`button.rt-chip${v === chip ? '.active' : ''}`, { type: 'button', style: { '--chip': CHIP_COLORS[v] } as any, title: `${v} chips` }, fmtShort(v));
    b.onclick = () => {
      chip = v;
      chipBar.querySelectorAll('.rt-chip').forEach((x) => x.classList.toggle('active', x === b));
      chipLabel.textContent = fmt(chip, 0);
      sound.play('chip');
    };
    return b;
  }));
  const undoBtn = h('button.mini', { type: 'button', title: 'Undo last chip', onclick: () => undo() }, '↶ Undo');
  const clearBtn = h('button.mini', { type: 'button', title: 'Take down all removable bets', onclick: () => clearAll() }, '✕ Clear');
  shell.stage.append(tray, tableWrap, h('div.rt-toolbar', chipBar, h('div.rt-actions', undoBtn, clearBtn)));

  const removable = (key: string) => {
    if (key === 'pass' || key === 'dontPass') return point === null;
    if (/^(come|dc)\d+$/.test(key)) return false;
    return true;
  };

  function render() {
    for (const s of slots) {
      const amounts = s.keys.map((k) => [k, bets[k] ?? 0] as const).filter(([, v]) => v > 0);
      const existing = new Map<string, HTMLElement>();
      s.chips.querySelectorAll<HTMLElement>('.cr-placed').forEach((c) => existing.set(c.dataset.key!, c));
      existing.forEach((c, k) => { if (!amounts.some(([kk]) => kk === k)) c.remove(); });
      for (const [k, v] of amounts) {
        let c = existing.get(k);
        if (!c) {
          c = h('div.cr-placed', { 'data-key': k });
          if (/Odds/.test(k)) c.classList.add('odds');
          s.chips.appendChild(c);
        }
        c.textContent = fmtShort(v);
        c.style.setProperty('--chip', chipColor(v));
      }
      s.el.classList.toggle('closed', !s.target() && !amounts.length);
      s.el.classList.toggle('has', amounts.length > 0);
    }
    const onTable = tableTotal();
    totalEl.textContent = fmt(onTable);
    pointEl.textContent = point ? `ON · ${point}` : 'OFF (come-out)';
    pointEl.classList.toggle('on', !!point);
    undoBtn.toggleAttribute('disabled', !undoStack.length || rolling);
    clearBtn.toggleAttribute('disabled', !Object.keys(bets).some(removable) || rolling);
    positionPuck();
  }

  function addChip(key: string, amount: number, fromEl?: Element | null, slotEl?: HTMLElement): boolean {
    // odds caps
    const capFor = (k: string): number | null => {
      if (k === 'passOdds') return oddsCap(bets.pass ?? 0, point!);
      if (k === 'dontOdds') return oddsCap(bets.dontPass ?? 0, point!);
      const m = k.match(/^(comeOdds|dcOdds)(\d+)$/);
      if (m) return oddsCap(bets[(m[1] === 'comeOdds' ? 'come' : 'dc') + m[2]] ?? 0, +m[2]);
      return null;
    };
    const cap = capFor(key);
    const cur = bets[key] ?? 0;
    if (cap !== null && cur + amount > cap + 1e-9) {
      amount = round2(cap - cur);
      if (amount <= 0) {
        toast(`Max odds reached (${fmt(cap, 0)})`, 'info');
        return false;
      }
    }
    if (!wallet.placeBet(meta.id, amount)) return false;
    bets[key] = round2(cur + amount);
    undoStack.push({ key, amount });
    if (fromEl && slotEl) fx.flyChip(fromEl, slotEl, CHIP_COLORS[chip] ?? chipColor(amount));
    sound.play('chip');
    render();
    const c = slotEl?.querySelector<HTMLElement>(`.cr-placed[data-key="${key}"]`);
    if (c) {
      c.classList.remove('bump');
      void c.offsetWidth;
      c.classList.add('bump');
    }
    return true;
  }

  function clickSlot(s: Slot) {
    if (rolling || panel.isAuto) return;
    const key = s.target();
    if (!key) {
      toast(s.why, 'info');
      sound.play('error');
      return;
    }
    addChip(key, chip, chipBar.querySelector('.rt-chip.active'), s.el);
  }

  function takeDown(key: string, amount: number) {
    const cur = bets[key] ?? 0;
    amount = Math.min(cur, amount);
    if (amount <= 0) return;
    wallet.refund(meta.id, amount);
    const left = round2(cur - amount);
    if (left > 0) bets[key] = left;
    else delete bets[key];
    // odds can't outlive their flat bet
    if (key === 'pass' && !bets.pass && bets.passOdds) takeDown('passOdds', bets.passOdds);
    if (key === 'dontPass' && !bets.dontPass && bets.dontOdds) takeDown('dontOdds', bets.dontOdds);
  }

  function removeFromSlot(s: Slot) {
    if (rolling || panel.isAuto) return;
    const key = [...s.keys].reverse().find((k) => bets[k] && removable(k));
    if (!key) {
      if (s.keys.some((k) => bets[k])) toast('That bet is working — it can\'t be taken down now', 'info');
      return;
    }
    takeDown(key, bets[key]);
    for (let i = undoStack.length - 1; i >= 0; i--) if (!bets[undoStack[i].key]) undoStack.splice(i, 1);
    sound.play('click');
    render();
  }

  function undo() {
    if (rolling) return;
    const last = undoStack.pop();
    if (!last) return;
    if (removable(last.key)) takeDown(last.key, last.amount);
    sound.play('click');
    render();
  }

  function clearAll() {
    if (rolling) return;
    Object.keys(bets).filter(removable).forEach((k) => bets[k] && takeDown(k, bets[k]));
    undoStack.length = 0;
    sound.play('whoosh');
    render();
  }

  // ---------------------------------------------------------------- roll
  const balanceEl = () => document.querySelector('.balance-pill');
  const chipElFor = (key: string) => table.querySelector<HTMLElement>(`.cr-placed[data-key="${key}"]`);

  async function roll(fast = false): Promise<{ bet: number; payout: number } | null> {
    if (rolling) return null;
    if (!Object.keys(bets).length) {
      toast('Place a bet first — try the Pass Line', 'info');
      return null;
    }
    rolling = true;
    tableWrap.classList.add('locked');
    render();
    try {
      const { floats } = await fair.next(2);
      if (destroyed) return null;
      const d: [number, number] = [dieFromFloat(floats[0]), dieFromFloat(floats[1])];
      const total = d[0] + d[1];
      const out = resolveRoll(bets, point, d[0], d[1]);
      let applied = false;
      // money is applied exactly once — either after the animation or immediately on page leave
      const applyMoney = () => {
        if (applied) return;
        applied = true;
        pendingApply = null;
        bets = out.bets;
        point = out.point;
        if (out.settledStake > 0 || out.returned > 0) {
          const base = out.settledStake + out.stayStake;
          wallet.settle(meta.id, out.settledStake, out.returned, `${d[0]}+${d[1]}=${total} · ${out.message}`, base > 0 ? out.returned / base : 0);
        }
      };
      pendingApply = applyMoney;
      totalBadge.classList.remove('show');
      msg.classList.remove('show');
      await animateDice(d, fast);
      if (destroyed) return null;
      totalBadge.textContent = String(total);
      totalBadge.className = `cr-total show ${total === 7 ? 'seven' : ''}`;
      msg.textContent = out.message;
      msg.className = `cr-msg show ${out.kind}`;
      shell.addResult(String(total), out.kind === 'seven-out' ? 'red' : out.kind === 'point-made' || out.kind === 'natural' ? 'win' : out.kind === 'point-set' ? 'green' : out.kind === 'craps' ? 'lose' : 'neutral');

      // chip feedback
      const bal = balanceEl();
      for (const r of out.results) {
        const c = chipElFor(r.key);
        if (!c) continue;
        if (r.outcome === 'win' || r.outcome === 'stay') {
          c.classList.add('won');
          if (bal) fx.flyChip(c, bal, '#f5b300', fmtShort(r.ret));
        } else if (r.outcome === 'push') {
          c.classList.add('push');
          if (bal) fx.flyChip(c, bal, chipColor(r.stake));
        } else if (r.outcome === 'lose') c.classList.add('lost');
        else if (r.outcome === 'move' && r.to) {
          const dest = table.querySelector(`[data-slot="${r.to}"]`);
          if (dest) fx.flyChip(c, dest, chipColor(r.stake), fmtShort(r.stake));
          c.classList.add('moving');
        }
      }
      if (out.kind === 'point-set' || out.kind === 'point-made' || out.kind === 'seven-out') sound.play(out.kind === 'point-set' ? 'pop' : 'whoosh');
      applyMoney();
      const base = out.settledStake + out.stayStake;
      if (base > 0) {
        const mult = out.returned / base;
        if (out.returned > 0) fx.pop(shell.stage, `+${fmt(out.returned)}`, mult >= 10 ? 'big' : 'win');
        fx.celebrate(shell.stage, out.returned, out.settledStake, mult);
      }
      await sleep(store.data.settings.reducedMotion ? 150 : fast ? 450 : 750);
      undoStack.length = 0;
      return { bet: out.settledStake, payout: out.returned };
    } finally {
      rolling = false;
      tableWrap.classList.remove('locked');
      if (!destroyed) render();
    }
  }

  // ---------------------------------------------------------------- panel
  const totalEl = h('span.rt-total', '0.00');
  const pointEl = h('span.cr-point-status');
  const chipLabel = h('span', fmt(chip, 0));
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Roll',
    hideAmount: true,
    onAction: () => roll(false),
    autoRound: async () => {
      if (!bets.pass && point === null) {
        if (!addChip('pass', chip, null, pass)) return null;
      } else if (!Object.keys(bets).length && point !== null) {
        if (!addChip('come', chip, null, come)) return null;
      }
      return roll(true);
    },
    autoDelay: 450,
  });
  panel.settings.append(
    h('div.info-box',
      h('div.stat-row', h('span.muted', 'On the table'), h('span', h('span.chip-icon.inline'), totalEl)),
      h('div.stat-row', h('span.muted', 'Point'), pointEl),
      h('div.stat-row', h('span.muted', 'Selected chip'), chipLabel),
      h('p.muted.small', 'Click the felt to bet. Right-click / long-press to take a bet down. Auto rolls with a Pass Line bet of the selected chip.'),
    ),
  );
  shell.controls.appendChild(panel.el);
  render();
  requestAnimationFrame(() => { placeDiceIdle(); positionPuck(); });

  return () => {
    destroyed = true;
    pendingApply?.();
    // give back every chip still on the felt
    const left = tableTotal();
    if (left > 0) wallet.refund(meta.id, left);
    bets = {};
    // cleanup can run from beforeunload (after the store's own save) — persist the refund now
    store.save();
    ro.disconnect();
    panel.destroy();
    shell.destroy();
  };
}
