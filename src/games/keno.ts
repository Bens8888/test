/** KENO — pick up to 10 of 40 numbers, 10 are drawn. */
import '../styles/games/keno.css';
import type { GameMeta } from './registry';
import { KENO } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { kenoDraw } from '../engine/outcomes';
import { kenoHitProbs, kenoTable, type KenoRisk } from '../engine/paytables';
import { sound } from '../engine/sound';
import { wallet } from '../engine/wallet';
import { fmt, h, round2, sleep } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const RISKS: KenoRisk[] = ['classic', 'low', 'medium', 'high'];

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      `Pick 1 to ${KENO.maxPicks} numbers from 1–${KENO.numbers}.`,
      `${KENO.drawn} numbers are drawn. The more of your picks that get hit, the bigger the multiplier.`,
      'Choose a risk level: higher risk pays more for lots of hits and less (or nothing) for a few.',
      'The payout row under the board shows what each number of hits pays for your current picks.',
      'All tables are scaled to 99% RTP (1% house edge).',
    ],
    stageClass: 'keno-stage',
  });

  const picks = new Set<number>();
  let risk: KenoRisk = 'classic';
  let playing = false;
  let alive = true;
  const cells = new Map<number, HTMLElement>();

  // ---------------------------------------------------------------- board
  const board = h('div.keno-board');
  for (let n = 1; n <= KENO.numbers; n++) {
    const cell = h('button.keno-cell', { type: 'button' }, h('span', String(n)));
    cell.addEventListener('click', () => toggle(n));
    cells.set(n, cell);
    board.appendChild(cell);
  }
  const payRow = h('div.keno-pays');
  const payHint = h('div.keno-hint.muted.small');
  const tools = h('div.keno-tools',
    h('button.mini', { type: 'button', onclick: () => autoPick() }, '🎲 Auto pick'),
    h('button.mini', { type: 'button', onclick: () => clearPicks() }, '✕ Clear'),
    h('span.keno-count.muted.small'),
  );
  shell.stage.append(board, payRow, payHint, tools);

  function toggle(n: number) {
    if (playing || panel.isAuto) return;
    clearReveal();
    if (picks.has(n)) {
      picks.delete(n);
      sound.play('click', 0.8);
    } else {
      if (picks.size >= KENO.maxPicks) {
        toast(`Max ${KENO.maxPicks} picks`, 'info');
        sound.play('error');
        return;
      }
      picks.add(n);
      sound.play('pop', 1 + picks.size * 0.05);
    }
    renderPicks();
  }
  function autoPick() {
    if (playing || panel.isAuto) return;
    clearReveal();
    const count = picks.size || 10;
    picks.clear();
    const pool = Array.from({ length: KENO.numbers }, (_, i) => i + 1);
    for (let i = 0; i < count; i++) picks.add(pool.splice(Math.floor(Math.random() * pool.length), 1)[0]);
    sound.play('whoosh');
    renderPicks();
    [...picks].forEach((n, i) => {
      const c = cells.get(n)!;
      c.classList.remove('pop-in');
      setTimeout(() => { void c.offsetWidth; c.classList.add('pop-in'); }, i * 25);
    });
  }
  function clearPicks() {
    if (playing || panel.isAuto) return;
    picks.clear();
    clearReveal();
    sound.play('click');
    renderPicks();
  }
  function clearReveal() {
    cells.forEach((c) => c.classList.remove('hit', 'miss', 'drawn'));
    payRow.querySelectorAll('.keno-pay').forEach((p) => p.classList.remove('active'));
  }

  function renderPicks() {
    cells.forEach((c, n) => c.classList.toggle('picked', picks.has(n)));
    (tools.querySelector('.keno-count') as HTMLElement).textContent = `${picks.size} / ${KENO.maxPicks} picked`;
    renderPays();
    updateProfit();
  }

  function renderPays() {
    const k = picks.size;
    if (!k) {
      payRow.replaceChildren(h('div.keno-empty.muted', 'Pick numbers on the board to see payouts'));
      payHint.textContent = '';
      return;
    }
    const table = kenoTable(risk, k);
    const probs = kenoHitProbs(k);
    payRow.replaceChildren(...table.map((m, hits) =>
      h(`div.keno-pay${m > 0 ? '.pays' : ''}${m >= 10 ? '.big' : ''}`, { title: `${hits} hits · ${(probs[hits] * 100).toFixed(probs[hits] < 0.001 ? 4 : 2)}% chance` },
        h('div.kp-mult', `${m > 0 ? fmt(m) : '0.00'}×`),
        h('div.kp-hits', `${hits} ${hits === 1 ? 'hit' : 'hits'}`),
      )));
    payHint.textContent = 'Hover a payout to see its chance.';
    payRow.querySelectorAll<HTMLElement>('.keno-pay').forEach((el, hits) => {
      el.addEventListener('pointerenter', () => (payHint.textContent = `${hits} ${hits === 1 ? 'hit' : 'hits'} pays ${fmt(table[hits])}× · chance ${(probs[hits] * 100).toFixed(probs[hits] < 0.001 ? 4 : 2)}%`));
    });
  }

  // ---------------------------------------------------------------- play
  async function play(bet: number, fast = false): Promise<{ bet: number; payout: number } | null> {
    if (playing) return null;
    if (!picks.size) {
      toast('Pick at least one number first', 'info');
      return null;
    }
    const snapPicks = new Set(picks);
    const table = kenoTable(risk, snapPicks.size);
    if (!wallet.placeBet(meta.id, bet)) return null;
    playing = true;
    shell.stage.classList.add('drawing');
    try {
      clearReveal();
      const { floats } = await fair.next(KENO.drawn);
      const drawn = kenoDraw(floats);
      let hits = 0;
      const pays = () => payRow.querySelectorAll<HTMLElement>('.keno-pay');
      for (const n of drawn) {
        if (alive) await sleep(fast ? 55 : 120);
        const c = cells.get(n)!;
        c.classList.add('drawn');
        if (snapPicks.has(n)) {
          hits++;
          c.classList.add('hit');
          sound.play('gem', 0.9 + hits * 0.06);
          if (alive) fx.burstAt(c, { count: 14, colors: ['#2dff8a', '#22e4ff', '#ffffff'], speed: 220, ring: false });
          pays().forEach((p, i) => p.classList.toggle('active', i === hits));
        } else {
          c.classList.add('miss');
          sound.play('pop', 0.7);
        }
      }
      pays().forEach((p, i) => p.classList.toggle('active', i === hits));
      const mult = table[hits] ?? 0;
      const payout = round2(bet * mult);
      wallet.settle(meta.id, bet, payout, `${hits}/${snapPicks.size} hits`, mult);
      shell.addResult(`${hits}/${snapPicks.size}`, payout > bet ? 'win' : payout > 0 ? 'neutral' : 'lose');
      if (payout > 0) fx.pop(shell.stage, `${fmt(mult)}×`, mult >= 10 ? 'big' : payout >= bet ? 'win' : 'neutral');
      if (alive) fx.celebrate(shell.stage, payout, bet, mult);
      return { bet, payout };
    } finally {
      playing = false;
      shell.stage.classList.remove('drawing');
    }
  }

  // ---------------------------------------------------------------- panel
  const riskSel = h('select', RISKS.map((r) => h('option', { value: r }, r[0].toUpperCase() + r.slice(1)))) as HTMLSelectElement;
  riskSel.addEventListener('change', () => {
    risk = riskSel.value as KenoRisk;
    sound.play('click');
    renderPays();
    updateProfit();
  });
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Play',
    onAction: (bet) => play(bet),
    autoRound: (bet) => play(bet, true),
    autoDelay: 450,
    onAutoChange: (on) => board.classList.toggle('locked', on),
  });
  const updateProfit = () => {
    const k = picks.size;
    const max = k ? Math.max(...kenoTable(risk, k)) : 0;
    panel.setProfitHint(k ? `Max win: ${fmt(panel.bet * max)}` : '');
  };
  panel.onBetChange = updateProfit;
  panel.settings.append(h('div.field', h('label.field-label', 'Risk'), riskSel));
  shell.controls.appendChild(panel.el);
  renderPicks();

  return () => {
    alive = false; // an in-flight draw finishes instantly and settles
    panel.destroy();
    shell.destroy();
  };
}
