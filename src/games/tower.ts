/** TOWER — climb row by row picking safe tiles; cash out before you hit a bomb. */
import '../styles/games/tower.css';
import type { GameMeta } from './registry';
import { TOWER } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { towerBombs } from '../engine/outcomes';
import { towerMultiplier, type TowerDifficulty } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fmt, fmtMult, h, round2, sleep, svg } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const GEM = '<svg viewBox="0 0 64 64"><defs><linearGradient id="twg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#b9ffd9"/><stop offset=".5" stop-color="#2dff8a"/><stop offset="1" stop-color="#0a9f63"/></linearGradient></defs><path d="M20 12h24l12 14-24 28L8 26z" fill="url(#twg)" stroke="#eafff3" stroke-width="2" stroke-linejoin="round"/><path d="M8 26h48M20 12l-4 14 16 28 16-28-4-14M16 26l16-14 16 14" fill="none" stroke="rgba(255,255,255,.55)" stroke-width="1.6" stroke-linejoin="round"/></svg>';
const BOMB = '<svg viewBox="0 0 64 64"><defs><radialGradient id="twb" cx=".35" cy=".35" r=".7"><stop offset="0" stop-color="#ff9aac"/><stop offset=".45" stop-color="#ff3d6e"/><stop offset="1" stop-color="#5a0a22"/></radialGradient></defs><circle cx="30" cy="36" r="18" fill="url(#twb)" stroke="#ffd0da" stroke-width="2"/><path d="M40 22l6-6" stroke="#ffd0da" stroke-width="4" stroke-linecap="round"/><path d="M47 15c3-3 6-2 8 0" stroke="#ffc94d" stroke-width="2.5" fill="none" stroke-linecap="round"/><circle cx="55" cy="14" r="3.5" fill="#ffe14d"/><circle cx="24" cy="30" r="4" fill="rgba(255,255,255,.5)"/></svg>';

class Abort extends Error {}

interface Round {
  bet: number;
  diff: TowerDifficulty;
  bombs: number[][];
  row: number; // rows cleared
  picks: number[];
  over: boolean;
  payout: number;
}

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Choose a difficulty, set your bet and press Bet to start climbing.',
      'Pick one tile on the glowing row. Find a gem and you climb to the next row; hit a bomb and the round is over.',
      'Each row you clear raises the multiplier. Cash out any time after the first row. Reaching the top cashes out automatically.',
      'Easy: 4 tiles / 1 bomb · Medium: 3/1 · Hard: 2/1 · Expert: 3/2 · Master: 4/3. RTP 98%.',
    ],
    stageClass: 'tower-stage',
  });

  let diff: TowerDifficulty = 'medium';
  let round: Round | null = null;
  let busy = false;
  let destroyed = false;
  const wait = async (ms: number) => {
    await sleep(ms);
    if (destroyed) throw new Abort();
  };
  const fast = () => panel.isAuto || store.data.settings.reducedMotion;

  // ---------------------------------------------------------------- DOM
  const tower = h('div.tw-tower');
  const hudMult = h('div.tw-hud-val', '1.00×');
  const hudNext = h('div.tw-hud-val.next', '—');
  const hudProfit = h('div.tw-hud-val', '0.00');
  const hud = h('div.tw-hud',
    h('div.tw-hud-item', h('div.tw-hud-label', 'Current'), hudMult),
    h('div.tw-hud-item', h('div.tw-hud-label', 'Next row'), hudNext),
    h('div.tw-hud-item', h('div.tw-hud-label', 'Profit'), hudProfit),
  );
  const banner = h('div.tw-banner');
  shell.stage.append(hud, h('div.tw-frame', tower, banner));

  let tileEls: HTMLButtonElement[][] = [];
  let rowEls: HTMLElement[] = [];

  function build() {
    const { tiles } = TOWER.difficulty[diff];
    tileEls = [];
    rowEls = [];
    tower.replaceChildren();
    tower.style.setProperty('--tiles', String(tiles));
    for (let r = 0; r < TOWER.rows; r++) {
      const row = h('div.tw-row', { 'data-row': String(r) });
      const mult = h('div.tw-mult', fmtMult(towerMultiplier(diff, r + 1)));
      const tilesWrap = h('div.tw-tiles');
      const els: HTMLButtonElement[] = [];
      for (let t = 0; t < tiles; t++) {
        const b = h('button.tw-tile', { type: 'button', 'aria-label': `Row ${r + 1} tile ${t + 1}` }, h('span.tw-face'), h('span.tw-icon')) as HTMLButtonElement;
        b.addEventListener('click', () => {
          if (panel.isAuto) return;
          pick(r, t).catch(onAbort);
        });
        els.push(b);
        tilesWrap.appendChild(b);
      }
      row.append(mult, tilesWrap);
      tileEls.push(els);
      rowEls.push(row);
      tower.prepend(row); // bottom-up
    }
    syncRows();
  }

  function syncRows() {
    rowEls.forEach((row, r) => {
      row.classList.toggle('active', !!round && !round.over && r === round.row);
      row.classList.toggle('cleared', !!round && r < round.row);
    });
    const cur = round ? towerMultiplier(round.diff, round.row) : 1;
    const nextM = round && round.row < TOWER.rows ? towerMultiplier(round.diff, round.row + 1) : towerMultiplier(diff, 1);
    const lost = !!round && round.over && round.payout <= 0;
    hudMult.textContent = lost ? '0.00×' : fmtMult(round && round.row > 0 ? cur : 1);
    hudNext.textContent = round?.over ? '—' : fmtMult(nextM);
    const profit = !round ? 0 : lost ? -round.bet : round.row > 0 ? round2(round.bet * cur - round.bet) : 0;
    hudProfit.textContent = (profit > 0 ? '+' : '') + fmt(profit);
    hudProfit.classList.toggle('pos', profit > 0);
    hudProfit.classList.toggle('neg', profit < 0);
    if (!panel) return;
    if (round && !round.over) {
      panel.setLabel(round.row > 0 ? `Cash out ${fmt(round2(round.bet * cur))}` : 'Pick a tile');
      panel.setVariant(round.row > 0 ? 'cashout' : 'play');
      panel.setEnabled(round.row > 0 || panel.isAuto);
    } else {
      panel.setLabel('Bet');
      panel.setVariant('play');
      panel.setEnabled(true);
    }
    randomBtn.disabled = !round || round.over || busy || panel.isAuto;
  }

  function clearBoard() {
    tileEls.flat().forEach((b) => {
      b.className = 'tw-tile';
      b.disabled = false;
      (b.querySelector('.tw-icon') as HTMLElement).replaceChildren();
    });
    banner.className = 'tw-banner';
    banner.replaceChildren();
  }

  function revealAll(r: Round) {
    r.bombs.forEach((bombs, row) => {
      tileEls[row].forEach((b, t) => {
        if (b.classList.contains('revealed')) return;
        const isBomb = bombs.includes(t);
        b.classList.add('revealed', 'ghost', isBomb ? 'bomb' : 'gem');
        (b.querySelector('.tw-icon') as HTMLElement).replaceChildren(svg(isBomb ? BOMB : GEM));
      });
    });
    tileEls.flat().forEach((b) => (b.disabled = true));
  }

  function showBanner(text: string, sub: string, kind: 'win' | 'lose') {
    banner.className = `tw-banner show ${kind}`;
    banner.replaceChildren(h('div.tw-banner-main', text), h('div.tw-banner-sub', sub));
    clearTimeout(bannerTimer);
    bannerTimer = setTimeout(() => banner.classList.remove('show'), 2200);
  }
  let bannerTimer: ReturnType<typeof setTimeout> | undefined;

  // ---------------------------------------------------------------- round flow
  async function start(bet: number) {
    if (round && !round.over) return;
    if (!wallet.placeBet(meta.id, bet)) return false;
    const { tiles, bombs } = TOWER.difficulty[diff];
    const r: Round = { bet, diff, bombs: [], row: 0, picks: [], over: false, payout: 0 };
    round = r;
    clearBoard();
    panel.lock(true);
    syncRows();
    try {
      const { floats } = await fair.next(TOWER.rows * bombs);
      if (destroyed) throw new Abort();
      r.bombs = towerBombs(floats, TOWER.rows, tiles, bombs);
    } catch (e) {
      if (e instanceof Abort || destroyed) return false;
      throw e;
    }
    sound.play('whoosh');
    syncRows();
    return true;
  }

  async function pick(row: number, tile: number) {
    const r = round;
    if (!r || r.over || busy || row !== r.row || !r.bombs.length) return;
    busy = true;
    const b = tileEls[row][tile];
    const isBomb = r.bombs[row].includes(tile);
    r.picks.push(tile);
    tileEls[row].forEach((x) => (x.disabled = true));
    b.classList.add('revealed', 'picked', isBomb ? 'bomb' : 'gem');
    (b.querySelector('.tw-icon') as HTMLElement).replaceChildren(svg(isBomb ? BOMB : GEM));
    if (isBomb) {
      r.over = true;
      sound.play('boom');
      fx.burstAt(b, { count: 50, colors: ['#ff3d6e', '#ffc94d', '#ff8a3d', '#ffffff'], speed: 460 });
      fx.shake(shell.stage, 1.4);
      wallet.settle(meta.id, r.bet, 0, `Bomb on row ${row + 1} (${r.diff})`, 0);
      shell.addResult('0.00×', 'lose');
      fx.flash(shell.stage, 'lose');
      busy = false;
      syncRows();
      await wait(fast() ? 150 : 350);
      revealAll(r);
      showBanner('BOOM', `Row ${row + 1} had a bomb`, 'lose');
      endRound();
      return;
    }
    r.row++;
    sound.play('gem', 1 + r.row * 0.06);
    fx.burstAt(b, { count: 18, colors: ['#2dff8a', '#22e4ff', '#ffffff'], speed: 220, ring: false });
    rowEls[row].classList.add('climb');
    busy = false;
    syncRows();
    if (r.row >= TOWER.rows) {
      await wait(fast() ? 120 : 300);
      await cashout();
    }
  }

  async function cashout() {
    const r = round;
    if (!r || r.over || r.row < 1 || busy) return;
    r.over = true;
    const mult = towerMultiplier(r.diff, r.row);
    const payout = round2(r.bet * mult);
    r.payout = payout;
    sound.play('cashout');
    wallet.settle(meta.id, r.bet, payout, `Cashed out row ${r.row}/${TOWER.rows} (${r.diff})`, mult);
    shell.addResult(fmtMult(mult), 'win');
    fx.pop(shell.stage, `+${fmt(payout - r.bet)}`, mult >= 10 ? 'big' : 'win');
    fx.celebrate(shell.stage, payout, r.bet, mult);
    showBanner(fmtMult(mult), `You won ${fmt(payout)}`, 'win');
    syncRows();
    await wait(fast() ? 80 : 250);
    revealAll(r);
    endRound();
  }

  function endRound() {
    if (!panel.isAuto) panel.lock(false);
    syncRows();
  }

  function onAbort(e: unknown) {
    if (!(e instanceof Abort)) console.error(e);
  }

  async function randomPick() {
    if (!round || round.over || busy) return;
    const { tiles } = TOWER.difficulty[round.diff];
    await pick(round.row, Math.floor(Math.random() * tiles));
  }

  // ---------------------------------------------------------------- controls
  const diffSelect = h('select', (Object.keys(TOWER.difficulty) as TowerDifficulty[]).map((d) => {
    const { tiles, bombs } = TOWER.difficulty[d];
    return h('option', { value: d, selected: d === diff }, `${d[0].toUpperCase() + d.slice(1)} · ${tiles} tiles, ${bombs} bomb${bombs > 1 ? 's' : ''}`);
  })) as HTMLSelectElement;
  diffSelect.addEventListener('change', () => {
    diff = diffSelect.value as TowerDifficulty;
    sound.play('click');
    round = null;
    build();
    clearBoard();
  });
  const randomBtn = h('button.btn-secondary.tw-random', { type: 'button', 'data-keep-enabled': '1', onclick: () => randomPick().catch(onAbort) }, '🎲 Random pick') as HTMLButtonElement;
  randomBtn.dataset.keepEnabled = '1';
  const autoRowInput = h('input', { type: 'number', min: '1', max: String(TOWER.rows), step: '1', value: '3' }) as HTMLInputElement;

  const panel: BetPanel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: async (bet) => {
      if (round && !round.over) await cashout().catch(onAbort);
      else await start(bet).catch(onAbort);
    },
    autoExtra: h('div.field', h('label.field-label', `Cash out at row (1–${TOWER.rows})`), autoRowInput),
    autoRound: async (bet) => {
      const target = Math.max(1, Math.min(TOWER.rows, Math.floor(parseFloat(autoRowInput.value) || 3)));
      try {
        const ok = await start(bet);
        if (!ok || !round) return null;
        const r = round;
        while (!r.over && r.row < target) {
          await wait(store.data.settings.reducedMotion ? 60 : 190);
          await randomPick();
        }
        if (!r.over) await cashout();
        return { bet: r.bet, payout: r.payout };
      } catch (e) {
        if (e instanceof Abort) return null;
        throw e;
      }
    },
    autoDelay: 500,
    onAutoChange: (on) => {
      diffSelect.disabled = on;
      syncRows();
    },
  });
  panel.settings.append(h('div.field', h('label.field-label', 'Difficulty'), diffSelect), randomBtn);
  panel.onBetChange = () => syncRows();
  shell.controls.appendChild(panel.el);
  build();

  return () => {
    destroyed = true;
    const r = round;
    if (r && !r.over) {
      r.over = true;
      if (r.row >= 1) {
        const mult = towerMultiplier(r.diff, r.row);
        wallet.settle(meta.id, r.bet, round2(r.bet * mult), `Auto cash-out row ${r.row} (left game)`, mult);
      } else wallet.refund(meta.id, r.bet);
    }
    panel.destroy();
    shell.destroy();
  };
}
