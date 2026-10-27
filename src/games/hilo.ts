/** HI-LO — guess if the next card is higher or lower and chain the multiplier. */
import '../styles/games/hilo.css';
import type { GameMeta } from './registry';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx, toast } from '../engine/fx';
import { cardFromFloat, RANK_LABELS, SUITS, type Card } from '../engine/outcomes';
import { hiloChances, hiloStepMultiplier, hiloWins } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { floor2, fmt, fmtMult, h, round2, sleep } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const FLOATS_PER_ROUND = 104;

interface Round {
  bet: number;
  floats: number[];
  cursor: number;
  card: Card;
  mult: number;
  steps: number;
}

const isRed = (c: Card) => c.suit === 1 || c.suit === 2;

function cardEl(c: Card | null, cls = '') {
  if (!c) return h(`div.pcard.back${cls}`, h('div.pcard-back-pattern'));
  const rank = RANK_LABELS[c.rank], suit = SUITS[c.suit];
  return h(`div.pcard.${isRed(c) ? 'red' : 'black'}${cls}`,
    h('div.pc-corner.tl', h('span', rank), h('span', suit)),
    h('div.pc-center', suit),
    h('div.pc-corner.br', h('span', rank), h('span', suit)),
  );
}

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Place a bet to deal the first card. Ace is low, King is high.',
      'Guess whether the next card is Higher-or-Same or Lower-or-Same. On an Ace or King you pick between Higher/Lower and Same.',
      'Each correct guess multiplies your winnings by that guess\'s multiplier (99 ÷ win chance %).',
      'Skip a card any time for free. Cash out after at least one correct guess.',
      'One wrong guess and the bet is lost. Cards are drawn from an infinite deck (1% house edge).',
    ],
    stageClass: 'hilo-stage',
  });

  let round: Round | null = null;
  let busy = false;
  let alive = true;
  let lastPayout = 0;

  // ---------------------------------------------------------------- DOM
  const cardSlot = h('div.hl-card-slot', cardEl(null));
  const deck = h('div.hl-deck', h('div.pcard.back.stack', h('div.pcard-back-pattern')), h('div.pcard.back.stack', h('div.pcard-back-pattern')), h('div.pcard.back.stack', h('div.pcard-back-pattern')));
  const multEl = h('div.hl-mult-val', '1.00×');
  const profitEl = h('div.hl-profit.muted.small', 'Place a bet to start');
  const hud = h('div.hl-hud', h('div.hl-mult-label.muted.small', 'Total multiplier'), multEl, profitEl);
  const mkGuess = (dir: 'hi' | 'lo') => {
    const b = h(`button.hl-guess.${dir}`, { type: 'button' },
      h('span.hl-g-icon', dir === 'hi' ? '▲' : '▼'),
      h('span.hl-g-text', h('span.hl-g-label', ''), h('span.hl-g-sub', '')),
      h('span.hl-g-mult', ''),
    ) as HTMLButtonElement;
    b.addEventListener('click', () => guess(dir));
    return b;
  };
  const hiBtn = mkGuess('hi');
  const loBtn = mkGuess('lo');
  const skipBtn = h('button.mini.hl-skip', { type: 'button', onclick: () => skip() }, 'Skip card ⟳') as HTMLButtonElement;
  const trail = h('div.hl-trail');
  const table = h('div.hl-table', deck, cardSlot, hud);
  shell.stage.append(table, h('div.hl-actions', hiBtn, loBtn), h('div.hl-skip-row', skipBtn), h('div.hl-trail-wrap', h('div.hl-trail-label.muted.small', 'Card history'), trail));

  function setCard(c: Card, animate = true) {
    const el = cardEl(c, animate && !store.data.settings.reducedMotion ? '.flip-in' : '');
    cardSlot.replaceChildren(el);
    if (animate) sound.play('card');
  }

  function addTrail(c: Card, mark: 'start' | 'hi' | 'lo' | 'skip' | 'lose', mult?: number) {
    const icon = mark === 'start' ? '●' : mark === 'hi' ? '▲' : mark === 'lo' ? '▼' : mark === 'skip' ? '⟳' : '✕';
    const item = h(`div.hl-trail-item.${mark}`, cardEl(c, '.mini-card'), h('div.hl-trail-mark', icon, mult ? h('span', fmtMult(mult)) : null));
    trail.appendChild(item);
    while (trail.children.length > 24) trail.firstElementChild?.remove();
    trail.scrollLeft = trail.scrollWidth;
  }

  function renderButtons() {
    const active = !!round && !busy;
    const c = round?.card;
    const ch = hiloChances(c?.rank ?? 7);
    const hiM = hiloStepMultiplier(ch.hi), loM = hiloStepMultiplier(ch.lo);
    const set = (b: HTMLButtonElement, label: string, p: number, m: number) => {
      (b.querySelector('.hl-g-label') as HTMLElement).textContent = label;
      (b.querySelector('.hl-g-sub') as HTMLElement).textContent = `${(p * 100).toFixed(2)}%`;
      (b.querySelector('.hl-g-mult') as HTMLElement).textContent = fmtMult(m);
      b.disabled = !active || panel?.isAuto;
    };
    set(hiBtn, ch.hiLabel, ch.hi, hiM);
    set(loBtn, ch.loLabel, ch.lo, loM);
    skipBtn.disabled = !active || panel?.isAuto;
    const m = round?.mult ?? 1;
    multEl.textContent = fmtMult(m);
    if (round) {
      const profit = round2(round.bet * m - round.bet);
      profitEl.textContent = round.steps ? `Profit ${profit >= 0 ? '+' : ''}${fmt(profit)}` : 'Make your first guess';
    }
    if (panel) {
      if (round) {
        panel.setLabel(round.steps ? `Cash out ${fmt(round.bet * round.mult)}` : 'Cash out');
        panel.setVariant('cashout');
        panel.setEnabled(!busy && round.steps > 0);
      } else {
        panel.setLabel('Bet');
        panel.setVariant('play');
        panel.setEnabled(!busy);
      }
    }
  }

  function nextCard(): Card | null {
    if (!round || round.cursor >= round.floats.length) return null;
    return cardFromFloat(round.floats[round.cursor++]);
  }

  // ---------------------------------------------------------------- round flow
  async function start(bet: number): Promise<boolean> {
    if (round || busy) return false;
    if (!wallet.placeBet(meta.id, bet)) return false;
    busy = true;
    try {
      const { floats } = await fair.next(FLOATS_PER_ROUND);
      round = { bet, floats, cursor: 0, card: cardFromFloat(floats[0]), mult: 1, steps: 0 };
      round.cursor = 1;
      lastPayout = 0;
      trail.replaceChildren();
      shell.stage.classList.remove('lost');
      if (!panel.isAuto) panel.lock(true);
      setCard(round.card);
      addTrail(round.card, 'start');
      return true;
    } finally {
      busy = false;
      renderButtons();
    }
  }

  async function guess(dir: 'hi' | 'lo', fromAuto = false): Promise<'win' | 'lose' | null> {
    if (!round || busy || (panel.isAuto && !fromAuto)) return null;
    const r = round;
    const ch = hiloChances(r.card.rank);
    const p = dir === 'hi' ? ch.hi : ch.lo;
    const next = nextCard();
    if (!next) {
      toast('Deck exhausted — cashing out', 'info');
      await cashout();
      return null;
    }
    busy = true;
    renderButtons();
    const win = hiloWins(r.card.rank, next.rank, dir);
    setCard(next);
    await sleep(fromAuto ? 200 : 320);
    r.card = next;
    if (win) {
      r.mult = floor2(r.mult * hiloStepMultiplier(p));
      r.steps++;
      addTrail(next, dir, r.mult);
      sound.play('gem', 1 + Math.min(1, r.steps * 0.08));
      fx.burstAt(cardSlot, { count: 18, colors: ['#2dff8a', '#22e4ff', '#ffffff'], speed: 240 });
      multEl.classList.remove('bump');
      void multEl.offsetWidth;
      multEl.classList.add('bump');
      busy = false;
      if (r.cursor >= r.floats.length) {
        toast('Deck exhausted — cashing out', 'info');
        await cashout();
      } else renderButtons();
      return 'win';
    }
    addTrail(next, 'lose');
    cardSlot.firstElementChild?.classList.add('lose-card');
    shell.stage.classList.add('lost');
    wallet.settle(meta.id, r.bet, 0, `Lost after ${r.steps} ${r.steps === 1 ? 'guess' : 'guesses'}`, 0);
    shell.addResult(`${r.steps}✕`, 'lose');
    fx.celebrate(shell.stage, 0, r.bet, 0);
    fx.shake(shell.stage, 0.6);
    endRound();
    profitEl.textContent = 'Wrong guess — try again!';
    return 'lose';
  }

  async function skip(fromAuto = false) {
    if (!round || busy || (panel.isAuto && !fromAuto)) return;
    const next = nextCard();
    if (!next) {
      toast('Deck exhausted — cashing out', 'info');
      await cashout();
      return;
    }
    busy = true;
    renderButtons();
    sound.play('whoosh');
    setCard(next);
    await sleep(220);
    round.card = next;
    addTrail(next, 'skip');
    busy = false;
    renderButtons();
  }

  async function cashout(): Promise<number> {
    const r = round;
    if (!r) return 0;
    if (r.steps === 0) {
      // nothing won yet: give the stake back
      wallet.refund(meta.id, r.bet);
      endRound();
      return r.bet;
    }
    const payout = round2(r.bet * r.mult);
    lastPayout = payout;
    wallet.settle(meta.id, r.bet, payout, `Cashed out at ${fmtMult(r.mult)} (${r.steps} guesses)`, r.mult);
    shell.addResult(fmtMult(r.mult), 'win');
    if (alive) {
      sound.play('cashout');
      fx.pop(shell.stage, `+${fmt(payout - r.bet)}`, r.mult >= 10 ? 'big' : 'win');
      fx.celebrate(shell.stage, payout, r.bet, r.mult);
    }
    endRound();
    profitEl.textContent = `Won ${fmt(payout)}!`;
    return payout;
  }

  function endRound() {
    round = null;
    busy = false;
    if (!panel.isAuto) panel.lock(false);
    renderButtons();
  }

  // ---------------------------------------------------------------- auto
  const targetInput = h('input', { type: 'number', min: '1.01', step: '0.01', value: '2.00', inputMode: 'decimal' }) as HTMLInputElement;
  const autoExtra = h('div.field', h('label.field-label', 'Cash out at multiplier'), h('div.suffix-input', targetInput, h('span', '×')));

  async function autoRound(bet: number) {
    const target = Math.max(1.01, parseFloat(targetInput.value) || 2);
    if (!(await start(bet))) return null;
    while (round && alive) {
      await sleep(260);
      if (!round) break;
      const ch = hiloChances(round.card.rank);
      const res = await guess(ch.hi >= ch.lo ? 'hi' : 'lo', true);
      if (res === 'lose') return { bet, payout: 0 };
      if (!round) return { bet, payout: lastPayout }; // auto-cashed (deck exhausted)
      if (round && round.mult >= target) {
        await sleep(150);
        const payout = await cashout();
        return { bet, payout };
      }
    }
    return { bet, payout: lastPayout };
  }

  // ---------------------------------------------------------------- panel
  const panel: BetPanel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: async (bet) => {
      if (round) await cashout();
      else await start(bet);
    },
    autoRound,
    autoExtra,
    autoDelay: 500,
    onAutoChange: () => renderButtons(),
  });
  shell.controls.appendChild(panel.el);
  renderButtons();

  return () => {
    alive = false;
    panel.destroy();
    if (round) cashout(); // settle winnings / refund an untouched bet
    shell.destroy();
  };
}
