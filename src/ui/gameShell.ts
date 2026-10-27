/** Common page frame for a game: header, controls column, stage, result strip & my-bets table. */
import type { GameMeta } from '../games/registry';
import { store } from '../engine/store';
import { walletEvents } from '../engine/wallet';
import { fmt, fmtMult, h, svg } from '../engine/util';
import { openModal } from './modal';
import { openFairness } from './fairPanel';

export interface GameShell {
  root: HTMLElement;
  controls: HTMLElement;
  stage: HTMLElement;
  /** Push a pill into the recent results strip. */
  addResult: (label: string, kind: 'win' | 'lose' | 'neutral' | 'red' | 'black' | 'green') => void;
  /** Register a cleanup callback. */
  onDestroy: (fn: () => void) => void;
  destroy: () => void;
}

export function gameShell(root: HTMLElement, meta: GameMeta, opts: { rules: (string | Node)[]; stageClass?: string }): GameShell {
  const disposers: (() => void)[] = [];
  const results = h('div.results-strip');
  const controls = h('aside.game-controls');
  const stage = h(`section.game-stage.glass${opts.stageClass ? '.' + opts.stageClass : ''}`);

  const rulesBtn = h('button.chip-btn', { type: 'button', onclick: () => openModal(`${meta.name} — how to play`, h('div.rules', h('ul', opts.rules.map((r) => h('li', r))), h('p.muted.small', `Return to player: ${meta.rtp}. Play money only — no real prizes.`))) }, svg('<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="10"/><path d="M12 16v-5M12 8h.01"/></svg>'), 'Rules');
  const fairBtn = h('button.chip-btn', { type: 'button', onclick: () => openFairness(meta.id) }, svg('<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 2l8 4v6c0 5-3.5 8.5-8 10-4.5-1.5-8-5-8-10V6z"/><path d="M8.5 12l2.5 2.5 4.5-5"/></svg>'), 'Fairness');

  const header = h('div.game-header',
    h('a.back-link', { href: '#/' }, '‹ Lobby'),
    h('div.game-title', { style: { '--c1': meta.colors[0], '--c2': meta.colors[1] } as any },
      h('span.game-title-icon', svg(meta.icon)),
      h('div', h('h1', meta.name), h('div.muted.small', meta.tagline)),
    ),
    h('div.game-header-actions', rulesBtn, fairBtn),
  );

  const tbody = h('tbody');
  const renderBets = () => {
    const hist = store.gameStats(meta.id).history.slice(0, 10);
    tbody.replaceChildren(
      ...(hist.length
        ? hist.map((it) => {
            const profit = it.payout - it.bet;
            return h('tr',
              h('td', new Date(it.t).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' })),
              h('td', fmt(it.bet)),
              h('td', it.payout > 0 ? fmtMult(it.mult) : '0.00×'),
              h(`td.${profit >= 0 ? 'pos' : 'neg'}`, (profit >= 0 ? '+' : '') + fmt(profit)),
              h('td.hide-sm.muted', it.detail ?? ''),
            );
          })
        : [h('tr', h('td.muted', { colSpan: 5 }, 'No bets yet — place one!'))]),
    );
  };
  renderBets();
  disposers.push(walletEvents.settled.on((e) => e.game === meta.id && renderBets()));

  const myBets = h('div.my-bets.glass',
    h('h3', 'My bets'),
    h('div.table-wrap', h('table.data', h('thead', h('tr', h('th', 'Time'), h('th', 'Bet'), h('th', 'Multi'), h('th', 'Profit'), h('th.hide-sm', 'Result'))), tbody)),
  );

  const page = h('div.game-page', { 'data-game': meta.id }, header, h('div.game-layout', controls, h('div.stage-col', stage, results)), myBets);
  root.replaceChildren(page);

  return {
    root: page,
    controls,
    stage,
    addResult(label, kind) {
      const pill = h(`span.result-pill.${kind}`, label);
      results.prepend(pill);
      while (results.children.length > 16) results.lastElementChild?.remove();
    },
    onDestroy: (fn) => disposers.push(fn),
    destroy() {
      disposers.forEach((d) => d());
    },
  };
}
