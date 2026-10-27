/** MINES — reveal gems on a 5×5 grid, avoid the mines, cash out whenever you like. */
import '../styles/games/mines.css';
import type { GameMeta } from './registry';
import { MINES } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { minesPositions } from '../engine/outcomes';
import { minesMultiplier } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fmt, fmtMult, h, round2, sleep, svg } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const GEM = `<svg viewBox="0 0 64 64"><defs><linearGradient id="mg1" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#b9ffd9"/><stop offset=".5" stop-color="#2dff8a"/><stop offset="1" stop-color="#0b9d6a"/></linearGradient><linearGradient id="mg2" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#e8fff2"/><stop offset="1" stop-color="#5dffae"/></linearGradient></defs><path d="M18 10h28l12 14-26 32L6 24z" fill="url(#mg1)"/><path d="M6 24h52L32 56z" fill="#0fb87a" opacity=".55"/><path d="M18 10l6 14h16l6-14z" fill="url(#mg2)"/><path d="M24 24l8 32 8-32z" fill="#7dffc0" opacity=".7"/><path d="M18 10L6 24h18zM46 10l12 14H40z" fill="#c8ffe4" opacity=".8"/><circle cx="22" cy="17" r="2.2" fill="#fff"/></svg>`;
const MINE = `<svg viewBox="0 0 64 64"><defs><radialGradient id="mm1" cx=".35" cy=".35" r=".7"><stop offset="0" stop-color="#ff9aac"/><stop offset=".45" stop-color="#ff3d6e"/><stop offset="1" stop-color="#6b0a24"/></radialGradient></defs><g stroke="#ff8fa3" stroke-width="4" stroke-linecap="round"><path d="M32 6v10M32 48v10M6 32h10M48 32h10M13.6 13.6l7 7M43.4 43.4l7 7M50.4 13.6l-7 7M20.6 43.4l-7 7"/></g><circle cx="32" cy="32" r="17" fill="url(#mm1)"/><circle cx="26" cy="26" r="4" fill="#fff" opacity=".75"/></svg>`;

type TileState = 'hidden' | 'gem' | 'mine';

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Choose how many mines (1–24) hide in the 5×5 grid and press Bet.',
      'Reveal tiles one by one. Every gem raises your multiplier — every mine ends the round.',
      'Cash out at any time after finding at least one gem.',
      'More mines = bigger multipliers per gem. RTP 99%.',
      'Auto: switch to the Auto tab, select tiles on the grid, then start. Each round reveals those tiles and cashes out if all are safe.',
    ],
    stageClass: 'mines-stage',
  });

  let mines = MINES.defaultMines;
  let round: null | { bet: number; mines: number; positions: Set<number>; revealed: number; over: boolean } = null;
  let starting = false;
  let destroyed = false;
  const picks = new Set<number>(); // auto preselection
  const state: TileState[] = new Array(MINES.tiles).fill('hidden');

  // ---------------------------------------------------------------- stage
  const tiles: HTMLButtonElement[] = [];
  const grid = h('div.mines-grid');
  for (let i = 0; i < MINES.tiles; i++) {
    const t = h('button.mine-tile', { type: 'button', 'aria-label': `Tile ${i + 1}` },
      h('span.mine-face.front'),
      h('span.mine-face.back'),
    ) as HTMLButtonElement;
    t.addEventListener('click', () => onTile(i));
    tiles.push(t);
    grid.appendChild(t);
  }
  const hudMult = h('b', '1.00×');
  const hudNext = h('b', '—');
  const hudGems = h('b', String(MINES.tiles - mines));
  const hud = h('div.mines-hud',
    h('div.mines-hud-item', h('span', 'Multiplier'), hudMult),
    h('div.mines-hud-item', h('span', 'Next gem'), hudNext),
    h('div.mines-hud-item', h('span', 'Gems left'), hudGems),
  );
  const banner = h('div.mines-banner');
  shell.stage.append(hud, h('div.mines-board', grid, banner));

  // ---------------------------------------------------------------- tile rendering
  function setTile(i: number, s: TileState, opts: { dim?: boolean; animate?: boolean } = {}) {
    state[i] = s;
    const t = tiles[i];
    const back = t.querySelector('.back') as HTMLElement;
    t.classList.remove('gem', 'mine', 'revealed', 'dim', 'flip');
    if (s === 'hidden') {
      back.replaceChildren();
      return;
    }
    back.replaceChildren(svg(s === 'gem' ? GEM : MINE));
    t.classList.add('revealed', s);
    if (opts.dim) t.classList.add('dim');
    if (opts.animate !== false) {
      void t.offsetWidth;
      t.classList.add('flip');
    }
  }
  function resetBoard() {
    for (let i = 0; i < MINES.tiles; i++) setTile(i, 'hidden');
    banner.className = 'mines-banner';
    banner.replaceChildren();
    renderPicks();
  }
  function renderPicks() {
    const show = isAutoTab();
    tiles.forEach((t, i) => t.classList.toggle('picked', show && picks.has(i) && state[i] === 'hidden'));
  }
  function updateHud() {
    const revealed = round?.revealed ?? 0;
    const m = round?.mines ?? mines;
    const cur = minesMultiplier(m, revealed);
    hudMult.textContent = fmtMult(cur);
    const safeLeft = MINES.tiles - m - revealed;
    hudNext.textContent = safeLeft > 0 ? fmtMult(minesMultiplier(m, revealed + 1)) : '—';
    hudGems.textContent = String(safeLeft);
    hud.classList.toggle('live', !!round && !round.over);
    updateButton();
  }

  // ---------------------------------------------------------------- round flow
  async function startRound(bet: number, keepBoard = false): Promise<boolean> {
    if (round || starting) return false;
    if (!wallet.placeBet(meta.id, bet)) return false;
    starting = true;
    try {
      const { floats } = await fair.next(mines);
      round = { bet, mines, positions: new Set(minesPositions(floats, mines)), revealed: 0, over: false };
    } finally {
      starting = false;
    }
    if (destroyed) {
      wallet.refund(meta.id, bet);
      round = null;
      return false;
    }
    if (!keepBoard) resetBoard();
    else for (let i = 0; i < MINES.tiles; i++) setTile(i, 'hidden');
    banner.className = 'mines-banner';
    sound.play('chip');
    panel.lock(true);
    // keep the "random tile" helper usable mid-round
    randomBtn.disabled = false;
    updateHud();
    return true;
  }

  function reveal(i: number): 'gem' | 'mine' | null {
    if (!round || round.over || state[i] !== 'hidden') return null;
    if (round.positions.has(i)) {
      setTile(i, 'mine');
      return 'mine';
    }
    round.revealed++;
    setTile(i, 'gem');
    const tier = round.revealed / (MINES.tiles - round.mines);
    sound.play('gem', 1 + tier * 0.6);
    fx.burstAt(tiles[i], { count: 16, colors: ['#2dff8a', '#b9ffd9', '#22e4ff', '#fff'], speed: 220, ring: true });
    updateHud();
    return 'gem';
  }

  function revealRest() {
    if (!round) return;
    for (let i = 0; i < MINES.tiles; i++) {
      if (state[i] !== 'hidden') continue;
      setTile(i, round.positions.has(i) ? 'mine' : 'gem', { dim: true, animate: !store.data.settings.reducedMotion });
    }
  }

  function bust(i: number): { bet: number; payout: number } {
    const r = round!;
    r.over = true;
    sound.play('boom');
    tiles[i].classList.add('exploded');
    fx.burstAt(tiles[i], { count: 40, colors: ['#ff3d6e', '#ff8a3d', '#ffc94d', '#fff'], speed: 420 });
    fx.shake(shell.stage, 1.3);
    revealRest();
    wallet.settle(meta.id, r.bet, 0, `Mine after ${r.revealed} gem${r.revealed === 1 ? '' : 's'} (${r.mines} mines)`, 0);
    fx.celebrate(shell.stage, 0, r.bet, 0);
    shell.addResult('0.00×', 'lose');
    banner.className = 'mines-banner show lose';
    banner.replaceChildren(h('div.mb-title', 'BOOM'), h('div.mb-sub', 'Better luck next time'));
    endRound();
    return { bet: r.bet, payout: 0 };
  }

  function cashout(): { bet: number; payout: number } | null {
    const r = round;
    if (!r || r.over || r.revealed < 1) return null;
    r.over = true;
    const mult = minesMultiplier(r.mines, r.revealed);
    const payout = round2(r.bet * mult);
    revealRest();
    wallet.settle(meta.id, r.bet, payout, `${r.revealed} gem${r.revealed === 1 ? '' : 's'} · ${r.mines} mines`, mult);
    sound.play('cashout');
    shell.addResult(fmtMult(mult), payout > r.bet ? 'win' : 'lose');
    banner.className = 'mines-banner show win';
    banner.replaceChildren(h('div.mb-title', fmtMult(mult)), h('div.mb-sub', `+${fmt(payout - r.bet)}`));
    if (payout > r.bet) fx.burstAt(banner, { count: 30, colors: ['#2dff8a', '#ffc94d', '#22e4ff', '#fff'] });
    fx.celebrate(shell.stage, payout, r.bet, mult);
    endRound();
    return { bet: r.bet, payout };
  }

  function endRound() {
    const r = round;
    round = null;
    if (!panel.isAuto) panel.lock(false);
    updateHud();
    if (r) {
      hudGems.textContent = String(MINES.tiles - r.mines - r.revealed);
    }
  }

  function onTile(i: number) {
    if (isAutoTab() && !round) {
      if (panel.isAuto) return;
      // toggle auto preselection
      if (state.some((s) => s !== 'hidden')) resetBoard();
      if (picks.has(i)) picks.delete(i);
      else if (picks.size < MINES.tiles - mines) picks.add(i);
      else toast(`You can select at most ${MINES.tiles - mines} tiles with ${mines} mines`, 'info');
      sound.play('click');
      renderPicks();
      pickCount.textContent = String(picks.size);
      return;
    }
    if (!round) {
      if (!starting) {
        toast('Press Bet to start a round', 'info');
        flashButton();
      }
      return;
    }
    if (panel.isAuto) return;
    const res = reveal(i);
    if (res === 'mine') bust(i);
    else if (res === 'gem' && round && round.revealed >= MINES.tiles - round.mines) cashout();
  }

  function pickRandom() {
    if (!round || round.over) return;
    const hidden = state.map((s, i) => (s === 'hidden' ? i : -1)).filter((i) => i >= 0);
    if (!hidden.length) return;
    onTile(hidden[Math.floor(Math.random() * hidden.length)]);
  }

  function flashButton() {
    panel.button.classList.remove('nudge');
    void panel.button.offsetWidth;
    panel.button.classList.add('nudge');
  }

  const isAutoTab = () => {
    const box = panel.el.querySelector('.auto-box') as HTMLElement | null;
    return !!box && !box.hidden;
  };

  // ---------------------------------------------------------------- auto
  async function autoRound(bet: number) {
    if (!picks.size) {
      toast('Select some tiles on the grid for Autobet first', 'info');
      return null;
    }
    if (picks.size > MINES.tiles - mines) {
      toast('Too many tiles selected for that mine count', 'error');
      return null;
    }
    if (!(await startRound(bet, true))) return null;
    renderPicks();
    const order = [...picks];
    const fast = store.data.settings.reducedMotion;
    for (const i of order) {
      await sleep(fast ? 40 : 170);
      if (!round) break;
      tiles[i].classList.remove('picked');
      const res = reveal(i);
      if (res === 'mine') return bust(i);
    }
    await sleep(fast ? 40 : 160);
    const res = cashout();
    setTimeout(renderPicks, 650);
    return res;
  }

  // ---------------------------------------------------------------- panel
  const minesSelect = h('select', Array.from({ length: MINES.maxMines - MINES.minMines + 1 }, (_, k) => {
    const n = k + MINES.minMines;
    return h('option', { value: String(n), selected: n === mines }, String(n));
  })) as HTMLSelectElement;
  minesSelect.addEventListener('change', () => {
    mines = parseInt(minesSelect.value);
    while (picks.size > MINES.tiles - mines) picks.delete([...picks].pop()!);
    pickCount.textContent = String(picks.size);
    renderPicks();
    updateHud();
    sound.play('click');
  });
  const gemsOut = h('div.mines-gems-out', String(MINES.tiles - mines));
  const randomBtn = h('button.btn-secondary.mines-random', { type: 'button', onclick: () => pickRandom(), 'data-keep-enabled': '1' }, '🎲 Pick random tile') as HTMLButtonElement;
  randomBtn.dataset.keepEnabled = '1';
  const pickCount = h('b', '0');
  const autoExtra = h('div.info-box.small', h('div', 'Tiles selected: ', pickCount), h('div.muted', 'Click tiles on the grid to select them for every auto round.'),
    h('button.mini', { type: 'button', onclick: () => { if (!panel.isAuto) { picks.clear(); pickCount.textContent = '0'; renderPicks(); } } }, 'Clear selection'));

  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: async (bet) => {
      if (round) {
        cashout();
        return;
      }
      await startRound(bet);
    },
    autoRound,
    autoExtra,
    autoDelay: 700,
    onAutoChange: (on) => {
      if (!on && !round) panel.lock(false);
      randomBtn.disabled = on || !round;
      updateButton();
    },
  });
  panel.settings.append(
    h('div.grid2',
      h('div.field', h('label.field-label', 'Mines'), minesSelect),
      h('div.field', h('label.field-label', 'Gems'), gemsOut),
    ),
    randomBtn,
  );
  shell.controls.appendChild(panel.el);
  panel.el.querySelector('.tabs')?.addEventListener('click', () => setTimeout(() => {
    if (isAutoTab() && !round && state.some((st) => st !== 'hidden')) resetBoard();
    renderPicks();
  }));

  function updateButton() {
    gemsOut.textContent = String(MINES.tiles - (round?.mines ?? mines));
    randomBtn.disabled = !round || round.over || panel.isAuto;
    if (panel.isAuto) return;
    if (round && !round.over) {
      const mult = minesMultiplier(round.mines, round.revealed);
      panel.setVariant('cashout');
      panel.setLabel(round.revealed > 0 ? `Cash out ${fmt(round.bet * mult)}` : 'Pick a tile…');
      panel.setEnabled(round.revealed > 0);
      panel.setProfitHint(`Profit: ${fmt(round.bet * mult - round.bet)} (${fmtMult(mult)})`);
    } else {
      panel.setVariant('play');
      panel.setLabel('Bet');
      panel.setEnabled(true);
      panel.setProfitHint('');
    }
  }
  updateHud();

  return () => {
    destroyed = true;
    // never lose chips when leaving mid-round
    if (round && !round.over) {
      if (round.revealed > 0) cashout();
      else {
        wallet.refund(meta.id, round.bet);
        round = null;
      }
    }
    panel.destroy();
    shell.destroy();
  };
}
