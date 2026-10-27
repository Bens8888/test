/** DICE — roll over / under a target on a 0–100 slider. */
import type { GameMeta } from './registry';
import { DICE } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { diceRoll } from '../engine/outcomes';
import { diceMultiplier, rtp } from '../engine/paytables';
import { sound } from '../engine/sound';
import { wallet } from '../engine/wallet';
import { clamp, easeOutBack, fmt, h, round2 } from '../engine/util';
import { gameShell } from '../ui/gameShell';

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Pick a target with the slider and choose Roll Over or Roll Under.',
      'A number from 0.00 to 100.00 is rolled. Land on the green side to win.',
      'Lower win chance = higher multiplier. Multiplier = 99 ÷ win chance (1% house edge).',
      'Click the Roll Over / Under box to flip sides. Space bar rolls.',
    ],
    stageClass: 'dice-stage',
  });

  let target = 50.5;
  let over = true;
  let lastRoll = 50;
  let rolling = false;

  const chance = () => round2(over ? 100 - target : target);
  const mult = () => diceMultiplier(chance());

  // ---------------------------------------------------------------- stage DOM
  const resultNum = h('div.dice-result-num', '—');
  const resultBox = h('div.dice-result', resultNum);
  const fill = h('div.dice-fill');
  const handle = h('div.dice-handle', { role: 'slider', tabIndex: 0, 'aria-label': 'Target' }, h('span.grip'), h('span.dice-handle-val'));
  const marker = h('div.dice-marker', h('div.dice-marker-hex', h('span', '50.00')));
  const track = h('div.dice-track', fill, handle);
  const scale = h('div.dice-scale', [0, 25, 50, 75, 100].map((v) => h('span', String(v))));
  const slider = h('div.dice-slider', h('div.dice-lane', marker), track, scale);

  const multInput = h('input', { type: 'number', step: '0.01', min: '1.0102', inputMode: 'decimal' }) as HTMLInputElement;
  const targetBtn = h('button.dice-target', { type: 'button', title: 'Click to switch Over/Under' });
  const chanceInput = h('input', { type: 'number', step: '0.01', min: String(DICE.minChance), max: String(DICE.maxChance), inputMode: 'decimal' }) as HTMLInputElement;
  const infoRow = h('div.dice-info',
    h('div.field', h('label.field-label', 'Multiplier'), h('div.suffix-input', multInput, h('span', '×'))),
    h('div.field', h('label.field-label', h('span.ou-label', 'Roll over')), targetBtn),
    h('div.field', h('label.field-label', 'Win chance'), h('div.suffix-input', chanceInput, h('span', '%'))),
  );
  shell.stage.append(resultBox, slider, infoRow);

  // ---------------------------------------------------------------- state sync
  function render(skipInputs = false) {
    const t = target;
    fill.style.background = over
      ? `linear-gradient(90deg, var(--red) 0 ${t}%, var(--green) ${t}% 100%)`
      : `linear-gradient(90deg, var(--green) 0 ${t}%, var(--red) ${t}% 100%)`;
    handle.style.left = `${t}%`;
    (handle.querySelector('.dice-handle-val') as HTMLElement).textContent = t.toFixed(2);
    targetBtn.textContent = '';
    targetBtn.append(h('span', t.toFixed(2)), h('span.swap', '⇄'));
    (infoRow.querySelector('.ou-label') as HTMLElement).textContent = over ? 'Roll over' : 'Roll under';
    if (!skipInputs) {
      multInput.value = mult().toFixed(4).replace(/0+$/, '').replace(/\.$/, '.00');
      chanceInput.value = chance().toFixed(2);
    }
    updateProfit();
  }
  function setTarget(v: number, skipInputs = false) {
    const lo = over ? 100 - DICE.maxChance : DICE.minChance;
    const hi = over ? 100 - DICE.minChance : DICE.maxChance;
    const nv = round2(clamp(v, lo, hi));
    if (Math.floor(nv) !== Math.floor(target)) sound.play('tick');
    target = nv;
    render(skipInputs);
  }
  function setChance(c: number, skipInputs = false) {
    c = clamp(c, DICE.minChance, DICE.maxChance);
    setTarget(over ? 100 - c : c, skipInputs);
  }

  multInput.addEventListener('input', () => {
    const m = parseFloat(multInput.value);
    if (m > 1) {
      setChance((100 * rtp('dice')) / m, true);
      chanceInput.value = chance().toFixed(2);
    }
  });
  multInput.addEventListener('change', () => render());
  chanceInput.addEventListener('input', () => {
    const c = parseFloat(chanceInput.value);
    if (c > 0) {
      setChance(c, true);
      multInput.value = mult().toFixed(4);
    }
  });
  chanceInput.addEventListener('change', () => render());
  targetBtn.addEventListener('click', () => {
    if (rolling && panel.isAuto) return;
    over = !over;
    target = round2(100 - target);
    sound.play('whoosh');
    setTarget(target);
  });

  // drag
  let dragging = false;
  const fromPointer = (e: PointerEvent) => {
    const r = track.getBoundingClientRect();
    setTarget(Math.round(((e.clientX - r.left) / r.width) * 100));
  };
  track.addEventListener('pointerdown', (e) => {
    if (panel.isAuto) return;
    dragging = true;
    track.setPointerCapture(e.pointerId);
    handle.classList.add('active');
    fromPointer(e);
  });
  track.addEventListener('pointermove', (e) => dragging && fromPointer(e));
  const endDrag = () => {
    dragging = false;
    handle.classList.remove('active');
  };
  track.addEventListener('pointerup', endDrag);
  track.addEventListener('pointercancel', endDrag);
  handle.addEventListener('keydown', (e) => {
    if (e.key === 'ArrowLeft' || e.key === 'ArrowDown') setTarget(target - 1);
    else if (e.key === 'ArrowRight' || e.key === 'ArrowUp') setTarget(target + 1);
    else return;
    e.preventDefault();
  });

  // ---------------------------------------------------------------- roll
  async function animateRoll(roll: number, win: boolean, fast: boolean) {
    const from = lastRoll;
    const dur = fast ? 260 : 520;
    marker.classList.remove('win', 'lose');
    marker.classList.add('show', 'moving');
    resultBox.classList.remove('win', 'lose');
    resultBox.classList.add('rolling');
    sound.play('roll');
    const t0 = performance.now();
    let lastTick = 0;
    await new Promise<void>((resolve) => {
      const step = (now: number) => {
        const t = Math.min(1, (now - t0) / dur);
        const e = easeOutBack(t);
        const v = from + (roll - from) * e;
        marker.style.left = `${clamp(v, 0, 100)}%`;
        const shown = t < 1 ? clamp(v + (Math.random() - 0.5) * 8 * (1 - t), 0, 100) : roll;
        resultNum.textContent = shown.toFixed(2);
        (marker.querySelector('span') as HTMLElement).textContent = shown.toFixed(2);
        if (now - lastTick > 45 && t < 1) {
          lastTick = now;
          sound.play('tick', 0.8 + t * 0.6);
        }
        if (t < 1) requestAnimationFrame(step);
        else resolve();
      };
      requestAnimationFrame(step);
    });
    lastRoll = roll;
    marker.classList.remove('moving');
    marker.classList.add(win ? 'win' : 'lose');
    resultBox.classList.remove('rolling');
    resultBox.classList.add(win ? 'win' : 'lose');
  }

  async function play(bet: number, fast = false) {
    if (rolling) return null;
    const snapTarget = target, snapOver = over, snapMult = mult();
    if (!wallet.placeBet(meta.id, bet)) return null;
    rolling = true;
    try {
      const { floats } = await fair.next(1);
      const roll = diceRoll(floats[0]);
      const win = snapOver ? roll > snapTarget : roll < snapTarget;
      const payout = win ? round2(bet * snapMult) : 0;
      await animateRoll(roll, win, fast);
      wallet.settle(meta.id, bet, payout, `${roll.toFixed(2)} ${snapOver ? '>' : '<'} ${snapTarget.toFixed(2)}`, win ? snapMult : 0);
      shell.addResult(roll.toFixed(2), win ? 'win' : 'lose');
      if (win) {
        fx.burstAt(marker, { count: 26, colors: ['#2dff8a', '#22e4ff', '#fff'] });
        fx.pop(shell.stage, `+${fmt(payout - bet)}`, snapMult >= 10 ? 'big' : 'win');
      }
      fx.celebrate(shell.stage, payout, bet, snapMult);
      return { bet, payout };
    } finally {
      rolling = false;
    }
  }

  // ---------------------------------------------------------------- panel
  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Roll',
    onAction: (bet) => play(bet),
    autoRound: (bet) => play(bet, true),
    autoDelay: 180,
  });
  const updateProfit = () => panel.setProfitHint(`Profit on win: ${fmt(Math.max(0, panel.bet * mult() - panel.bet))}`);
  panel.onBetChange = updateProfit;
  shell.controls.appendChild(panel.el);
  render();

  return () => {
    panel.destroy();
    shell.destroy();
  };
}

