/**
 * Shared bet controls used by every game: amount input with ½ / 2× / Max, a main
 * action button, and an Auto tab with stop-on-profit / stop-on-loss and
 * on-win / on-loss bet adjustments.
 */
import { WALLET } from '../config';
import { sound } from './sound';
import { toast } from './fx';
import { store } from './store';
import { wallet } from './wallet';
import { fmt, h, round2, sleep } from './util';

export interface RoundResult {
  bet: number;
  payout: number;
}

export interface BetPanelOptions {
  game: string;
  actionLabel: string;
  /** Manual play. Return a promise to keep the button busy until it settles. */
  onAction: (bet: number) => Promise<unknown> | unknown;
  /** Enables the Auto tab. Plays one full round, returns null to abort auto. */
  autoRound?: (bet: number) => Promise<RoundResult | null>;
  /** Game-specific auto settings (e.g. auto cash-out target). */
  autoExtra?: HTMLElement;
  /** Delay between auto rounds (ms). */
  autoDelay?: number;
  /** Launch the next auto round after this many ms even if the previous one is still running. */
  autoOverlap?: number;
  /** Allow overlapping manual rounds (Plinko balls). */
  concurrent?: boolean;
  /** Hide the bet amount input (Roulette / Craps use table chips instead). */
  hideAmount?: boolean;
  defaultBet?: number;
  /** Called when auto starts/stops so games can lock their own UI. */
  onAutoChange?: (running: boolean) => void;
}

const lastBet: Record<string, number> = {};

export class BetPanel {
  el: HTMLElement;
  /** Put game settings (risk, rows, mines…) in here. */
  settings: HTMLElement;
  button: HTMLButtonElement;
  input!: HTMLInputElement;
  private opts: BetPanelOptions;
  private mode: 'manual' | 'auto' = 'manual';
  private busy = 0;
  private locked = false;
  private auto = { running: false, stopRequested: false };
  private autoEls!: {
    count: HTMLInputElement; winMode: 'reset' | 'inc'; winPct: HTMLInputElement; lossMode: 'reset' | 'inc';
    lossPct: HTMLInputElement; stopProfit: HTMLInputElement; stopLoss: HTMLInputElement; status: HTMLElement;
  };
  private tabs?: HTMLElement;
  private autoBox?: HTMLElement;
  private label: string;
  private disposers: (() => void)[] = [];
  private profitEl?: HTMLElement;

  constructor(opts: BetPanelOptions) {
    this.opts = opts;
    this.label = opts.actionLabel;
    this.settings = h('div.panel-settings');
    this.button = h('button.btn-play', { type: 'button' }, opts.actionLabel) as HTMLButtonElement;
    this.button.addEventListener('click', () => this.handleClick());

    const parts: (HTMLElement | null)[] = [];
    if (opts.autoRound) {
      this.tabs = h('div.seg.tabs',
        h('button.active', { type: 'button', onclick: () => this.setMode('manual') }, 'Manual'),
        h('button', { type: 'button', onclick: () => this.setMode('auto') }, 'Auto'),
      );
      parts.push(this.tabs);
    }
    if (!opts.hideAmount) parts.push(this.buildAmount());
    parts.push(this.settings);
    if (opts.autoRound) {
      this.autoBox = this.buildAuto();
      this.autoBox.hidden = true;
      parts.push(this.autoBox);
    }
    parts.push(this.button);
    this.el = h('div.bet-panel.glass', parts);

    // Space bar = main action
    const onKey = (e: KeyboardEvent) => {
      if (e.code !== 'Space' || e.repeat) return;
      const t = e.target as HTMLElement;
      if (t && (t.tagName === 'INPUT' || t.tagName === 'TEXTAREA' || t.tagName === 'SELECT' || t.tagName === 'BUTTON' || t.isContentEditable)) return;
      if (document.querySelector('.modal-backdrop')) return;
      e.preventDefault();
      this.handleClick();
    };
    window.addEventListener('keydown', onKey);
    this.disposers.push(() => window.removeEventListener('keydown', onKey));
  }

  private buildAmount(): HTMLElement {
    const start = lastBet[this.opts.game] ?? this.opts.defaultBet ?? 10;
    this.input = h('input.amount', { type: 'number', min: '0', step: '0.01', inputMode: 'decimal', value: start.toFixed(2) }) as HTMLInputElement;
    this.input.addEventListener('change', () => this.setBet(this.bet));
    this.input.addEventListener('input', () => {
      lastBet[this.opts.game] = this.bet;
      this.onBetChange();
    });
    const mk = (label: string, fn: () => void, title: string) =>
      h('button.mini', { type: 'button', title, onclick: () => { if (this.locked) return; fn(); sound.play('click'); } }, label);
    this.profitEl = h('span.muted.small');
    return h('div.field',
      h('label.field-label', h('span', 'Bet amount'), this.profitEl),
      h('div.amount-row',
        h('div.amount-wrap', h('span.chip-icon'), this.input),
        mk('½', () => this.setBet(this.bet / 2), 'Half'),
        mk('2×', () => this.setBet(this.bet * 2), 'Double'),
        mk('Max', () => this.setBet(Math.min(wallet.balance, WALLET.maxBet)), 'Max'),
      ),
    );
  }

  private buildAuto(): HTMLElement {
    const num = (val: string, ph = '') => h('input', { type: 'number', min: '0', step: 'any', value: val, placeholder: ph, inputMode: 'decimal' }) as HTMLInputElement;
    const els = {
      count: num('0', '∞'),
      winMode: 'reset' as 'reset' | 'inc',
      winPct: num('0'),
      lossMode: 'reset' as 'reset' | 'inc',
      lossPct: num('0'),
      stopProfit: num('', 'off'),
      stopLoss: num('', 'off'),
      status: h('div.auto-status.muted.small'),
    };
    this.autoEls = els;
    const modeRow = (key: 'winMode' | 'lossMode', pct: HTMLInputElement) => {
      const reset = h('button.active', { type: 'button' }, 'Reset');
      const inc = h('button', { type: 'button' }, 'Increase');
      const set = (m: 'reset' | 'inc') => {
        els[key] = m;
        reset.classList.toggle('active', m === 'reset');
        inc.classList.toggle('active', m === 'inc');
        pct.disabled = m === 'reset';
      };
      reset.onclick = () => set('reset');
      inc.onclick = () => set('inc');
      pct.disabled = true;
      return h('div.auto-mode', h('div.seg.small', reset, inc), h('div.pct-wrap', pct, h('span', '%')));
    };
    return h('div.auto-box',
      this.opts.autoExtra ?? null,
      h('div.field', h('label.field-label', 'Number of bets (0 = ∞)'), els.count),
      this.opts.hideAmount ? null : h('div.field', h('label.field-label', 'On win'), modeRow('winMode', els.winPct)),
      this.opts.hideAmount ? null : h('div.field', h('label.field-label', 'On loss'), modeRow('lossMode', els.lossPct)),
      h('div.grid2',
        h('div.field', h('label.field-label', 'Stop on profit'), els.stopProfit),
        h('div.field', h('label.field-label', 'Stop on loss'), els.stopLoss),
      ),
      els.status,
    );
  }

  private setMode(m: 'manual' | 'auto') {
    if (this.auto.running || this.locked) return;
    this.mode = m;
    sound.play('click');
    this.tabs?.querySelectorAll('button').forEach((b, i) => b.classList.toggle('active', (i === 0) === (m === 'manual')));
    if (this.autoBox) this.autoBox.hidden = m !== 'auto';
    this.refreshButton();
  }

  get bet(): number {
    if (this.opts.hideAmount) return 0;
    return wallet.normalize(parseFloat(this.input.value));
  }

  setBet(v: number) {
    if (!this.input) return;
    v = wallet.normalize(v);
    this.input.value = v.toFixed(2);
    lastBet[this.opts.game] = v;
    this.onBetChange();
  }

  /** Optional hook: text shown next to "Bet amount" (e.g. "Profit on win"). */
  setProfitHint(text: string) {
    if (this.profitEl) this.profitEl.textContent = text;
  }
  onBetChange: () => void = () => {};

  setLabel(label: string) {
    this.label = label;
    this.refreshButton();
  }
  setVariant(v: 'play' | 'cashout' | 'danger') {
    this.button.classList.toggle('cashout', v === 'cashout');
    this.button.classList.toggle('danger', v === 'danger');
  }
  setEnabled(on: boolean) {
    this.button.disabled = !on;
  }
  /** Lock bet amount & settings (while a multi-step round is in progress). */
  lock(on: boolean) {
    this.locked = on;
    this.el.classList.toggle('locked', on);
    if (this.input) this.input.disabled = on;
    this.settings.querySelectorAll<HTMLInputElement | HTMLButtonElement | HTMLSelectElement>('input,button,select').forEach((e) => {
      if (!e.dataset.keepEnabled) e.disabled = on;
    });
    this.tabs?.querySelectorAll('button').forEach((b) => ((b as HTMLButtonElement).disabled = on));
  }
  get isAuto() {
    return this.auto.running;
  }

  private refreshButton() {
    if (this.mode === 'auto') {
      this.button.textContent = this.auto.running ? (this.auto.stopRequested ? 'Stopping…' : 'Stop Autobet') : 'Start Autobet';
      this.button.classList.toggle('danger', this.auto.running);
    } else {
      this.button.textContent = this.label;
    }
  }

  private async handleClick() {
    if (this.button.disabled) return;
    if (this.mode === 'auto') {
      if (this.auto.running) {
        this.auto.stopRequested = true;
        this.refreshButton();
      } else this.runAuto();
      return;
    }
    if (this.busy && !this.opts.concurrent) return;
    sound.play('click');
    this.busy++;
    if (!this.opts.concurrent) this.button.classList.add('busy');
    try {
      await this.opts.onAction(this.bet);
    } catch (err) {
      console.error(err);
    } finally {
      this.busy--;
      if (!this.busy) this.button.classList.remove('busy');
    }
  }

  private async runAuto() {
    const els = this.autoEls;
    const base = this.bet;
    let current = base;
    let count = Math.max(0, Math.floor(parseFloat(els.count.value) || 0));
    const totalCount = count;
    const stopProfit = parseFloat(els.stopProfit.value) || 0;
    const stopLoss = parseFloat(els.stopLoss.value) || 0;
    let net = 0;
    let played = 0;
    this.auto.running = true;
    this.auto.stopRequested = false;
    this.refreshButton();
    this.lockAutoInputs(true);
    this.opts.onAutoChange?.(true);
    const status = () => (els.status.innerHTML = `Bets: <b>${played}</b> &nbsp; Net: <b class="${net >= 0 ? 'pos' : 'neg'}">${net >= 0 ? '+' : ''}${fmt(net)}</b>`);
    status();
    const pending = new Set<Promise<void>>();
    let launched = 0;
    const handle = (res: RoundResult | null) => {
      if (!res) {
        this.auto.stopRequested = true;
        return;
      }
      played++;
      net = round2(net + res.payout - res.bet);
      status();
      const won = res.payout > res.bet;
      if (!this.opts.hideAmount) {
        if (won) current = els.winMode === 'inc' ? current * (1 + (parseFloat(els.winPct.value) || 0) / 100) : base;
        else current = els.lossMode === 'inc' ? current * (1 + (parseFloat(els.lossPct.value) || 0) / 100) : base;
        current = wallet.normalize(current);
        this.setBet(current);
      }
      if (stopProfit > 0 && net >= stopProfit && !this.auto.stopRequested) { toast('Autobet stopped: profit target reached', 'success'); this.auto.stopRequested = true; }
      if (stopLoss > 0 && -net >= stopLoss && !this.auto.stopRequested) { toast('Autobet stopped: loss limit reached', 'info'); this.auto.stopRequested = true; }
    };
    try {
      while (!this.auto.stopRequested) {
        if (!this.opts.hideAmount && (current < WALLET.minBet || !wallet.canAfford(current))) {
          if (pending.size) {
            await Promise.race(pending);
            continue;
          }
          toast('Not enough chips to continue Autobet', 'error');
          break;
        }
        const p: Promise<void> = this.opts.autoRound!(current).then(handle, (err) => { console.error(err); this.auto.stopRequested = true; }).finally(() => pending.delete(p));
        pending.add(p);
        launched++;
        if (count > 0 && launched >= count) break;
        if (this.opts.autoOverlap) await Promise.race([p, sleep(this.opts.autoOverlap)]);
        else {
          await p;
          if (this.auto.stopRequested) break;
          await sleep(this.opts.autoDelay ?? (store.data.settings.reducedMotion ? 250 : 350));
        }
      }
      await Promise.all(pending);
    } catch (err) {
      console.error(err);
    }
    count = count > 0 ? Math.max(0, count - played) : 0;
    if (totalCount > 0) els.count.value = String(count || totalCount);
    this.auto.running = false;
    this.auto.stopRequested = false;
    this.lockAutoInputs(false);
    this.opts.onAutoChange?.(false);
    this.refreshButton();
  }

  private lockAutoInputs(on: boolean) {
    this.lock(on);
    this.autoBox?.querySelectorAll<HTMLInputElement | HTMLButtonElement>('input,button').forEach((e) => {
      if (on) {
        e.dataset.wasDisabled = e.disabled ? '1' : '';
        e.disabled = true;
      } else e.disabled = e.dataset.wasDisabled === '1';
    });
  }

  stopAuto() {
    this.auto.stopRequested = true;
  }

  destroy() {
    this.auto.stopRequested = true;
    this.disposers.forEach((d) => d());
  }
}

