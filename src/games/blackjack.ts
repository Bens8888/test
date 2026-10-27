/** BLACKJACK — dealer vs player: hit, stand, double, split. Pays 3:2, dealer stands on soft 17. */
import '../styles/games/blackjack.css';
import type { GameMeta } from './registry';
import { BLACKJACK } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { cardFromFloat, RANK_LABELS, SUITS, type Card } from '../engine/outcomes';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { fmt, fmtShort, h, round2, sleep } from '../engine/util';
import { gameShell } from '../ui/gameShell';

class Abort extends Error {}

type Result = 'win' | 'lose' | 'push' | 'bj' | 'bust';
interface Hand {
  cards: Card[];
  bet: number;
  done: boolean;
  doubled: boolean;
  split: boolean;
  splitAces: boolean;
  result?: Result;
  payout: number;
  el: HTMLElement;
  cardsEl: HTMLElement;
  totalEl: HTMLElement;
  resultEl: HTMLElement;
  betEl: HTMLElement;
}
interface Round {
  base: number;
  stake: number;
  floats: number[];
  cursor: number;
  hands: Hand[];
  active: number;
  dealer: Card[];
  holeEl: HTMLElement | null;
  phase: 'dealing' | 'player' | 'dealer' | 'done';
  payout: number;
  finished: Promise<void>;
  finish: () => void;
}

const cardVal = (c: Card) => (c.rank === 1 ? 11 : Math.min(10, c.rank));
function score(cards: Card[]) {
  let t = 0, aces = 0;
  for (const c of cards) {
    t += cardVal(c);
    if (c.rank === 1) aces++;
  }
  while (t > 21 && aces) {
    t -= 10;
    aces--;
  }
  return { total: t, soft: aces > 0 };
}
const isRed = (c: Card) => c.suit === 1 || c.suit === 2;

/** Basic strategy for S17, DAS, no surrender. */
function strategy(cards: Card[], up: number, canDouble: boolean, canSplit: boolean): 'H' | 'S' | 'D' | 'P' {
  const { total, soft } = score(cards);
  if (canSplit && cards.length === 2 && cardVal(cards[0]) === cardVal(cards[1])) {
    const v = cardVal(cards[0]);
    if (v === 11 || v === 8) return 'P';
    if (v === 9 && ((up >= 2 && up <= 6) || up === 8 || up === 9)) return 'P';
    if (v === 7 && up <= 7) return 'P';
    if (v === 6 && up <= 6) return 'P';
    if (v === 4 && (up === 5 || up === 6)) return 'P';
    if ((v === 3 || v === 2) && up <= 7) return 'P';
  }
  const D = (fallback: 'H' | 'S') => (canDouble ? 'D' : fallback);
  if (soft) {
    if (total >= 20) return 'S';
    if (total === 19) return up === 6 ? D('S') : 'S';
    if (total === 18) return up <= 6 ? D('S') : up <= 8 ? 'S' : 'H';
    if (total === 17) return up >= 3 && up <= 6 ? D('H') : 'H';
    if (total >= 15) return up >= 4 && up <= 6 ? D('H') : 'H';
    if (total >= 13) return up >= 5 && up <= 6 ? D('H') : 'H';
    return 'H';
  }
  if (total >= 17) return 'S';
  if (total >= 13) return up <= 6 ? 'S' : 'H';
  if (total === 12) return up >= 4 && up <= 6 ? 'S' : 'H';
  if (total === 11) return D('H');
  if (total === 10) return up <= 9 ? D('H') : 'H';
  if (total === 9) return up >= 3 && up <= 6 ? D('H') : 'H';
  return 'H';
}

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Get closer to 21 than the dealer without going over. Face cards are 10, Aces are 1 or 11.',
      'Blackjack (Ace + 10-value card on the first two cards) pays 3:2. Other wins pay 1:1, ties push.',
      'Dealer stands on soft 17 and peeks for blackjack when showing an Ace or a 10. No insurance.',
      'Double on any first two cards (also after a split): double the bet, take exactly one card.',
      `Split pairs into separate hands (up to ${BLACKJACK.maxSplits} splits). Split Aces get one card each.`,
      'Keyboard: H = Hit, S = Stand, D = Double, P = Split. Auto mode plays perfect basic strategy.',
    ],
    stageClass: 'bj-stage',
  });

  let round: Round | null = null;
  let acting = false;
  let destroyed = false;
  const fast = () => panel.isAuto || store.data.settings.reducedMotion;
  const wait = async (ms: number) => {
    await sleep(ms);
    if (destroyed) throw new Abort();
  };
  const onAbort = (e: unknown) => {
    if (!(e instanceof Abort)) console.error(e);
  };

  // ---------------------------------------------------------------- DOM
  const shoe = h('div.bj-shoe', h('div.bj-shoe-card'), h('div.bj-shoe-card'), h('div.bj-shoe-card'), h('span.bj-shoe-label', 'SHOE'));
  const dealerCards = h('div.bj-cards');
  const dealerTotal = h('div.bj-total.hidden', '0');
  const dealerArea = h('div.bj-dealer', h('div.bj-label', 'Dealer'), h('div.bj-hand-wrap', dealerCards, dealerTotal));
  const feltText = h('div.bj-felt-text', {
    html: `<svg viewBox="0 0 600 120" preserveAspectRatio="xMidYMid meet"><defs><path id="bjarc" d="M40,30 Q300,140 560,30"/></defs><text><textPath href="#bjarc" startOffset="50%" text-anchor="middle">BLACKJACK PAYS 3 TO 2</textPath></text></svg><div class="bj-felt-sub">Dealer stands on soft 17 · Insurance not offered</div>`,
  });
  const handsEl = h('div.bj-hands');
  const playerArea = h('div.bj-player', handsEl, h('div.bj-label', 'You'));
  const btn = (label: string, key: string, cls: string, fn: () => Promise<void>) => {
    const b = h(`button.bj-btn.${cls}`, { type: 'button', title: `${label} (${key})`, disabled: true, onclick: () => { if (!panel.isAuto) fn().catch(onAbort); } }, h('span', label), h('kbd', key)) as HTMLButtonElement;
    return b;
  };
  const hitBtn = btn('Hit', 'H', 'hit', () => act('H'));
  const standBtn = btn('Stand', 'S', 'stand', () => act('S'));
  const doubleBtn = btn('Double', 'D', 'double', () => act('D'));
  const splitBtn = btn('Split', 'P', 'split', () => act('P'));
  const actions = h('div.bj-actions', hitBtn, standBtn, doubleBtn, splitBtn);
  const message = h('div.bj-message');
  const table = h('div.bj-table', shoe, dealerArea, feltText, message, playerArea);
  shell.stage.append(table, actions);

  // ---------------------------------------------------------------- cards
  function cardEl(card: Card | null, faceUp: boolean) {
    const front = h('div.bj-face.bj-front');
    if (card) fillFront(front, card);
    const el = h('div.bj-card', h('div.bj-card-inner', front, h('div.bj-face.bj-back')));
    if (faceUp) el.classList.add('up');
    return el;
  }
  function fillFront(front: HTMLElement, c: Card) {
    const r = RANK_LABELS[c.rank], s = SUITS[c.suit];
    front.classList.toggle('red', isRed(c));
    front.replaceChildren(
      h('div.bj-corner', h('b', r), h('span', s)),
      h('div.bj-pip', s),
      h('div.bj-corner.br', h('b', r), h('span', s)),
    );
  }
  async function animateIn(el: HTMLElement, container: HTMLElement, faceUp: boolean, sideways = false) {
    container.appendChild(el);
    if (sideways) el.classList.add('sideways');
    const dur = fast() ? 170 : 360;
    const from = shoe.getBoundingClientRect();
    const to = el.getBoundingClientRect();
    el.style.transition = 'none';
    el.style.transform = `translate(${from.left - to.left}px, ${from.top - to.top}px) rotate(-25deg) scale(0.75)`;
    void el.offsetWidth;
    el.style.transition = `transform ${dur}ms cubic-bezier(.2,.8,.3,1)`;
    el.style.transform = sideways ? 'rotate(90deg)' : '';
    sound.play('card');
    await wait(dur * 0.65);
    if (faceUp) el.classList.add('up');
    await wait(dur * 0.45);
  }

  function draw(r: Round): Card {
    const f = r.floats[r.cursor % r.floats.length];
    r.cursor++;
    return cardFromFloat(f);
  }

  function totalLabel(cards: Card[]) {
    const { total, soft } = score(cards);
    if (cards.length === 2 && total === 21) return '21';
    return soft && total < 21 ? `${total - 10}/${total}` : String(total);
  }

  function renderHand(hd: Hand) {
    const { total } = score(hd.cards);
    hd.totalEl.textContent = hd.cards.length ? totalLabel(hd.cards) : '';
    hd.totalEl.classList.toggle('bust', total > 21);
    hd.totalEl.classList.toggle('twentyone', total === 21);
    hd.betEl.textContent = fmtShort(hd.bet);
    hd.el.classList.toggle('active', !!round && round.phase === 'player' && round.hands[round.active] === hd && round.hands.length > 1);
  }
  function renderAll() {
    round?.hands.forEach(renderHand);
    syncButtons();
  }
  function renderDealer(r: Round, revealHole: boolean) {
    const shown = revealHole ? r.dealer : r.dealer.slice(0, 1);
    dealerTotal.classList.toggle('hidden', !shown.length);
    dealerTotal.textContent = shown.length ? totalLabel(shown) : '';
    dealerTotal.classList.toggle('bust', score(shown).total > 21);
  }

  function newHand(bet: number): Hand {
    const cardsEl = h('div.bj-cards');
    const totalEl = h('div.bj-total');
    const resultEl = h('div.bj-result');
    const betEl = h('div.bj-bet');
    const el = h('div.bj-hand', h('div.bj-hand-wrap', cardsEl, totalEl, resultEl), betEl);
    return { cards: [], bet, done: false, doubled: false, split: false, splitAces: false, payout: 0, el, cardsEl, totalEl, resultEl, betEl };
  }

  // ---------------------------------------------------------------- buttons
  function can(r: Round | null) {
    const live = !!r && r.phase === 'player' && !acting;
    const hd = live ? r!.hands[r!.active] : null;
    const two = !!hd && hd.cards.length === 2 && !hd.splitAces;
    return {
      hit: live && !!hd && !hd.done,
      stand: live && !!hd && !hd.done,
      double: two && (!hd!.split || BLACKJACK.doubleAfterSplit) && wallet.canAfford(hd!.bet),
      split: two && cardVal(hd!.cards[0]) === cardVal(hd!.cards[1]) && r!.hands.length < 1 + BLACKJACK.maxSplits && wallet.canAfford(hd!.bet),
    };
  }
  function syncButtons() {
    const c = can(round);
    const auto = panel.isAuto;
    hitBtn.disabled = !c.hit || auto;
    standBtn.disabled = !c.stand || auto;
    doubleBtn.disabled = !c.double || auto;
    splitBtn.disabled = !c.split || auto;
    actions.classList.toggle('live', !!round && round.phase === 'player');
  }

  function say(text: string, kind = '') {
    message.className = `bj-message show ${kind}`;
    message.textContent = text;
  }

  // ---------------------------------------------------------------- round flow
  async function deal(bet: number): Promise<Round | null> {
    if (round) return null;
    if (!wallet.placeBet(meta.id, bet)) return null;
    let finish!: () => void;
    const finished = new Promise<void>((res) => (finish = res));
    const r: Round = { base: bet, stake: bet, floats: [], cursor: 0, hands: [], active: 0, dealer: [], holeEl: null, phase: 'dealing', payout: 0, finished, finish };
    round = r;
    panel.lock(true);
    if (!panel.isAuto) panel.setEnabled(false);
    message.className = 'bj-message';
    // clear the felt
    const old = [...dealerCards.children, ...handsEl.querySelectorAll('.bj-card')] as HTMLElement[];
    old.forEach((c) => c.classList.add('gone'));
    if (old.length) await wait(fast() ? 120 : 260);
    dealerCards.replaceChildren();
    handsEl.replaceChildren();
    dealerTotal.classList.add('hidden');
    const { floats } = await fair.next(52);
    if (destroyed) throw new Abort();
    r.floats = floats;
    const hand = newHand(bet);
    r.hands.push(hand);
    handsEl.appendChild(hand.el);
    renderHand(hand);
    // deal: player, dealer up, player, dealer hole
    hand.cards.push(draw(r));
    await animateIn(cardEl(hand.cards[0], true), hand.cardsEl, true);
    renderHand(hand);
    r.dealer.push(draw(r));
    await animateIn(cardEl(r.dealer[0], true), dealerCards, true);
    renderDealer(r, false);
    hand.cards.push(draw(r));
    await animateIn(cardEl(hand.cards[1], true), hand.cardsEl, true);
    renderHand(hand);
    r.dealer.push(draw(r));
    r.holeEl = cardEl(null, false);
    await animateIn(r.holeEl, dealerCards, false);

    const upVal = cardVal(r.dealer[0]);
    const dealerBJ = score(r.dealer).total === 21;
    const playerBJ = score(hand.cards).total === 21;
    if (upVal >= 10) {
      // dealer peeks
      dealerArea.classList.add('peek');
      await wait(fast() ? 150 : 550);
      dealerArea.classList.remove('peek');
    }
    if (dealerBJ || playerBJ) {
      await revealHole(r);
      hand.done = true;
      await resolve(r);
      return r;
    }
    r.phase = 'player';
    r.active = 0;
    renderAll();
    return r;
  }

  async function revealHole(r: Round) {
    if (!r.holeEl) return;
    fillFront(r.holeEl.querySelector('.bj-front') as HTMLElement, r.dealer[1]);
    r.holeEl.classList.add('up');
    sound.play('card');
    r.holeEl = null;
    await wait(fast() ? 150 : 380);
    renderDealer(r, true);
  }

  async function act(a: 'H' | 'S' | 'D' | 'P') {
    const r = round;
    if (!r || r.phase !== 'player' || acting) return;
    const c = can(r);
    const hd = r.hands[r.active];
    if ((a === 'H' && !c.hit) || (a === 'S' && !c.stand) || (a === 'D' && !c.double) || (a === 'P' && !c.split)) return;
    acting = true;
    syncButtons();
    try {
      if (a === 'H') {
        hd.cards.push(draw(r));
        await animateIn(cardEl(hd.cards[hd.cards.length - 1], true), hd.cardsEl, true);
        renderHand(hd);
        const t = score(hd.cards).total;
        if (t > 21) {
          hd.done = true;
          hd.result = 'bust';
          showResult(hd);
          sound.play('lose');
          fx.shake(hd.el, 0.6);
        } else if (t === 21) hd.done = true;
      } else if (a === 'S') {
        hd.done = true;
        sound.play('click');
      } else if (a === 'D') {
        if (!wallet.addToBet(meta.id, hd.bet)) return;
        r.stake = round2(r.stake + hd.bet);
        hd.bet = round2(hd.bet * 2);
        hd.doubled = true;
        sound.play('chip');
        renderHand(hd);
        hd.cards.push(draw(r));
        await animateIn(cardEl(hd.cards[hd.cards.length - 1], true), hd.cardsEl, true, true);
        renderHand(hd);
        hd.done = true;
        if (score(hd.cards).total > 21) {
          hd.result = 'bust';
          showResult(hd);
          sound.play('lose');
        }
      } else if (a === 'P') {
        if (!wallet.addToBet(meta.id, hd.bet)) return;
        r.stake = round2(r.stake + hd.bet);
        sound.play('chip');
        const aces = hd.cards[0].rank === 1;
        const nh = newHand(hd.bet);
        nh.split = hd.split = true;
        nh.splitAces = hd.splitAces = aces;
        const moved = hd.cards.pop()!;
        nh.cards.push(moved);
        // move the card element over
        const movedEl = hd.cardsEl.lastElementChild as HTMLElement;
        r.hands.splice(r.active + 1, 0, nh);
        hd.el.after(nh.el);
        nh.cardsEl.appendChild(movedEl);
        movedEl.classList.add('slide');
        renderAll();
        await wait(fast() ? 120 : 300);
        // second card to the current hand
        hd.cards.push(draw(r));
        await animateIn(cardEl(hd.cards[1], true), hd.cardsEl, true);
        renderHand(hd);
        if (aces || score(hd.cards).total === 21) hd.done = true;
      }
      await advance(r);
    } finally {
      acting = false;
      if (round === r) renderAll();
    }
  }

  /** Move to the next unfinished hand (dealing its second card after a split) or the dealer. */
  async function advance(r: Round) {
    while (r.active < r.hands.length && r.hands[r.active].done) {
      r.active++;
      const nh = r.hands[r.active];
      if (!nh) break;
      renderAll();
      if (nh.cards.length === 1) {
        nh.cards.push(draw(r));
        await animateIn(cardEl(nh.cards[1], true), nh.cardsEl, true);
        renderHand(nh);
        if (nh.splitAces || score(nh.cards).total === 21) nh.done = true;
      }
    }
    if (r.active >= r.hands.length) await dealerTurn(r);
    else renderAll();
  }

  async function dealerTurn(r: Round) {
    r.phase = 'dealer';
    renderAll();
    await revealHole(r);
    const live = r.hands.some((hd) => score(hd.cards).total <= 21);
    if (live) {
      for (;;) {
        const { total, soft } = score(r.dealer);
        const hit = total < 17 || (total === 17 && soft && BLACKJACK.dealerHitsSoft17);
        if (!hit) break;
        r.dealer.push(draw(r));
        await animateIn(cardEl(r.dealer[r.dealer.length - 1], true), dealerCards, true);
        renderDealer(r, true);
      }
    }
    await resolve(r);
  }

  function showResult(hd: Hand) {
    const labels: Record<Result, string> = { win: 'WIN', lose: 'LOSE', push: 'PUSH', bj: 'BLACKJACK', bust: 'BUST' };
    hd.resultEl.className = `bj-result show ${hd.result}`;
    hd.resultEl.textContent = hd.result ? labels[hd.result] : '';
  }

  async function resolve(r: Round) {
    r.phase = 'done';
    const d = score(r.dealer).total;
    const dealerBJ = r.dealer.length === 2 && d === 21;
    let payout = 0;
    for (const hd of r.hands) {
      const p = score(hd.cards).total;
      const bj = !hd.split && hd.cards.length === 2 && p === 21;
      if (p > 21) { hd.result = 'bust'; hd.payout = 0; }
      else if (bj && !dealerBJ) { hd.result = 'bj'; hd.payout = round2(hd.bet * (1 + BLACKJACK.blackjackPays)); }
      else if (dealerBJ) { hd.result = bj ? 'push' : 'lose'; hd.payout = bj ? hd.bet : 0; }
      else if (d > 21 || p > d) { hd.result = 'win'; hd.payout = round2(hd.bet * 2); }
      else if (p === d) { hd.result = 'push'; hd.payout = hd.bet; }
      else { hd.result = 'lose'; hd.payout = 0; }
      payout += hd.payout;
      showResult(hd);
    }
    payout = round2(payout);
    r.payout = payout;
    const mult = r.stake > 0 ? payout / r.stake : 0;
    const handsTxt = r.hands.map((hd) => score(hd.cards).total).join('/');
    const detail = `${r.hands.some((x) => x.result === 'bj') ? 'Blackjack! ' : ''}You ${handsTxt} vs dealer ${dealerBJ ? 'BJ' : d}`;
    wallet.settle(meta.id, r.stake, payout, detail, mult);
    const net = payout - r.stake;
    if (net > 0) {
      say(`${r.hands.some((x) => x.result === 'bj') ? 'BLACKJACK! ' : 'You win '}+${fmt(net)}`, 'win');
      shell.addResult(`+${fmtShort(round2(net))}`, 'win');
      fx.celebrate(shell.stage, payout, r.stake, mult);
    } else if (net === 0) {
      say('Push — bet returned', '');
      sound.play('chip');
      shell.addResult('Push', 'neutral');
    } else {
      say(d > 21 ? 'Dealer busts… but so did you' : dealerBJ ? 'Dealer has Blackjack' : 'Dealer wins', 'lose');
      shell.addResult(`-${fmtShort(round2(-net))}`, 'lose');
      fx.celebrate(shell.stage, payout, r.stake, mult);
    }
    round = null;
    if (!panel.isAuto) panel.lock(false);
    panel.setEnabled(true);
    renderAll();
    r.finish();
  }

  // ---------------------------------------------------------------- keyboard
  const onKey = (e: KeyboardEvent) => {
    const t = e.target as HTMLElement;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) return;
    if (document.querySelector('.modal-backdrop') || panel.isAuto || e.ctrlKey || e.metaKey || e.altKey) return;
    const map: Record<string, 'H' | 'S' | 'D' | 'P'> = { h: 'H', s: 'S', d: 'D', p: 'P' };
    const a = map[e.key.toLowerCase()];
    if (a && round?.phase === 'player') {
      e.preventDefault();
      act(a).catch(onAbort);
    }
  };
  window.addEventListener('keydown', onKey);

  // ---------------------------------------------------------------- panel
  const panel: BetPanel = new BetPanel({
    game: meta.id,
    actionLabel: 'Deal',
    onAction: async (bet) => {
      await deal(bet).catch(onAbort);
    },
    autoRound: async (bet) => {
      try {
        const r = await deal(bet);
        if (!r) return null;
        while (r.phase === 'player') {
          await wait(fast() ? 120 : 300);
          const hd = r.hands[r.active];
          const c = can(r);
          const up = cardVal(r.dealer[0]);
          const move = strategy(hd.cards, up, c.double, c.split);
          await act(move === 'D' && !c.double ? 'H' : move === 'P' && !c.split ? 'H' : move);
        }
        await r.finished;
        return { bet: r.stake, payout: r.payout };
      } catch (e) {
        if (e instanceof Abort) return null;
        throw e;
      }
    },
    autoDelay: 700,
    onAutoChange: () => syncButtons(),
  });
  panel.settings.append(h('div.info-box',
    h('div.stat-row', h('span.muted', 'Blackjack pays'), h('b', '3 : 2')),
    h('div.stat-row', h('span.muted', 'Dealer'), h('b', 'Stands on soft 17')),
    h('div.stat-row', h('span.muted', 'Double / Split'), h('b', `Any 2 · up to ${BLACKJACK.maxSplits}×`)),
  ));
  shell.controls.appendChild(panel.el);
  say('Place your bet and press Deal');

  return () => {
    destroyed = true;
    window.removeEventListener('keydown', onKey);
    const r = round;
    if (r && r.phase !== 'done') {
      // hand never finished — give every chip on the table back
      wallet.refund(meta.id, r.stake);
      round = null;
      r.finish();
    }
    panel.destroy();
    shell.destroy();
  };
}
