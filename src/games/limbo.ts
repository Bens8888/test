/** LIMBO — pick a target multiplier; if the rolled multiplier reaches it, you win. */
import '../styles/games/limbo.css';
import type { GameMeta } from './registry';
import { LIMBO } from '../config';
import { BetPanel } from '../engine/betPanel';
import { fair } from '../engine/fair';
import { fx } from '../engine/fx';
import { limboResult } from '../engine/outcomes';
import { limboChance, rtp } from '../engine/paytables';
import { sound } from '../engine/sound';
import { store } from '../engine/store';
import { wallet } from '../engine/wallet';
import { clamp, easeOutCubic, fmt, fmtMult, h, round2, svg } from '../engine/util';
import { gameShell } from '../ui/gameShell';

const ROCKET = `<svg viewBox="0 0 64 96" fill="none"><defs><linearGradient id="lrb" x1="0" x2="1"><stop offset="0" stop-color="#c9cbef"/><stop offset=".5" stop-color="#fff"/><stop offset="1" stop-color="#8d8fb8"/></linearGradient><linearGradient id="lrf" x1="0" y1="0" x2="0" y2="1"><stop offset="0" stop-color="#ffe14d"/><stop offset=".5" stop-color="#ff8a3d"/><stop offset="1" stop-color="#ff3d9a" stop-opacity="0"/></linearGradient></defs><path class="flame" d="M22 70 Q32 98 42 70 Z" fill="url(#lrf)"/><path d="M32 4 C46 16 48 40 44 66 H20 C16 40 18 16 32 4 Z" fill="url(#lrb)"/><circle cx="32" cy="32" r="7" fill="#22e4ff" stroke="#5b3dff" stroke-width="3"/><path d="M20 50 L8 68 L20 64 Z M44 50 L56 68 L44 64 Z" fill="#ff3d9a"/><path d="M28 66 h8 v6 h-8z" fill="#5b3dff"/></svg>`;

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, {
    rules: [
      'Choose a target multiplier (1.01× and up).',
      'A random multiplier is rolled. If it reaches your target, you win bet × target.',
      'Win chance = 99 ÷ target (1% house edge). Higher targets pay more but hit less often.',
      'Space bar plays.',
    ],
    stageClass: 'limbo-stage',
  });

  let target = 2;
  let busy = false;

  // ---------------------------------------------------------------- stage
  const num = h('div.limbo-num', '1.00×');
  const sub = h('div.limbo-sub', 'Target 2.00×');
  const rocket = h('div.limbo-rocket', svg(ROCKET));
  const stars = h('div.limbo-stars', Array.from({ length: 36 }, () => h('i', { style: { left: `${Math.random() * 100}%`, top: `${Math.random() * 100}%`, animationDelay: `${-Math.random() * 3}s`, animationDuration: `${1.2 + Math.random() * 2}s` } })));
  const display = h('div.limbo-display', stars, rocket, h('div.limbo-readout', num, sub));

  const targetInput = h('input', { type: 'number', step: '0.01', min: String(LIMBO.minTarget), max: String(LIMBO.maxTarget), inputMode: 'decimal', value: '2.00' }) as HTMLInputElement;
  const chanceInput = h('input', { type: 'number', step: '0.01', min: '0.0001', max: '98.02', inputMode: 'decimal' }) as HTMLInputElement;
  const quick = h('div.limbo-quick', [1.5, 2, 5, 10, 100, 1000].map((m) =>
    h('button.mini', { type: 'button', onclick: () => { if (!panel.isAuto) { setTarget(m); sound.play('click'); } } }, `${m}×`)));
  const info = h('div.limbo-info',
    h('div.field', h('label.field-label', 'Target multiplier'), h('div.suffix-input', targetInput, h('span', '×'))),
    h('div.field', h('label.field-label', 'Win chance'), h('div.suffix-input', chanceInput, h('span', '%'))),
  );
  shell.stage.append(display, info, quick);

  function setTarget(v: number, from?: 'target' | 'chance') {
    target = round2(clamp(v, LIMBO.minTarget, LIMBO.maxTarget));
    if (from !== 'target') targetInput.value = target.toFixed(2);
    if (from !== 'chance') chanceInput.value = limboChance(target).toFixed(limboChance(target) < 0.01 ? 6 : 4).replace(/0+$/, '').replace(/\.$/, '');
    sub.textContent = `Target ${fmtMult(target)}`;
    updateProfit();
  }
  targetInput.addEventListener('input', () => {
    const v = parseFloat(targetInput.value);
    if (v >= LIMBO.minTarget) setTarget(v, 'target');
  });
  targetInput.addEventListener('change', () => setTarget(parseFloat(targetInput.value) || LIMBO.minTarget));
  chanceInput.addEventListener('input', () => {
    const c = parseFloat(chanceInput.value);
    if (c > 0) setTarget((rtp('limbo') * 100) / c, 'chance');
  });
  chanceInput.addEventListener('change', () => setTarget(target));

  // ---------------------------------------------------------------- play
  function animate(result: number, win: boolean, fast: boolean) {
    const dur = store.data.settings.reducedMotion ? 120 : fast ? 360 : 620;
    display.classList.remove('win', 'lose', 'launch', 'fall');
    display.classList.add('rolling');
    sound.play('whoosh');
    const t0 = performance.now();
    const lnR = Math.log(result);
    let lastTick = 0;
    return new Promise<void>((resolve) => {
      const step = (now: number) => {
        const t = Math.min(1, (now - t0) / dur);
        const v = Math.exp(lnR * easeOutCubic(t));
        num.textContent = fmtMult(t < 1 ? v : result);
        if (now - lastTick > 40 && t < 1) {
          lastTick = now;
          sound.play('tick', 0.8 + t);
        }
        if (t < 1) requestAnimationFrame(step);
        else {
          display.classList.remove('rolling');
          display.classList.add(win ? 'win' : 'lose', win ? 'launch' : 'fall');
          resolve();
        }
      };
      requestAnimationFrame(step);
    });
  }

  async function play(bet: number, fast = false) {
    if (busy) return null;
    const snapTarget = target;
    if (!wallet.placeBet(meta.id, bet)) return null;
    busy = true;
    try {
      const { floats } = await fair.next(1);
      const result = limboResult(floats[0]);
      const win = result >= snapTarget;
      const payout = win ? round2(bet * snapTarget) : 0;
      await animate(result, win, fast);
      wallet.settle(meta.id, bet, payout, `${fmtMult(result)} vs ${fmtMult(snapTarget)}`, win ? snapTarget : 0);
      shell.addResult(fmtMult(result), win ? 'win' : 'lose');
      if (win) {
        fx.burstAt(num, { count: 30, colors: ['#2dff8a', '#22e4ff', '#ffc94d', '#fff'] });
        fx.pop(shell.stage, `+${fmt(payout - bet)}`, snapTarget >= 10 ? 'big' : 'win');
      }
      fx.celebrate(shell.stage, payout, bet, snapTarget);
      return { bet, payout };
    } finally {
      busy = false;
    }
  }

  const panel = new BetPanel({
    game: meta.id,
    actionLabel: 'Bet',
    onAction: (bet) => play(bet),
    autoRound: (bet) => play(bet, true),
    autoDelay: 200,
    onAutoChange: (on) => {
      targetInput.disabled = on;
      chanceInput.disabled = on;
      quick.querySelectorAll('button').forEach((b) => ((b as HTMLButtonElement).disabled = on));
    },
  });
  const updateProfit = () => panel.setProfitHint(`Profit on win: ${fmt(Math.max(0, panel.bet * target - panel.bet))}`);
  panel.onBetChange = updateProfit;
  shell.controls.appendChild(panel.el);
  setTarget(2);

  return () => {
    panel.destroy();
    shell.destroy();
  };
}
