/** Lobby: hero banner, game grid with category filter and the live bets table. */
import { APP_NAME } from '../config';
import { CATEGORIES, GAMES, type Category } from '../games/registry';
import { store } from '../engine/store';
import { fmt, fmtMult, h, svg } from '../engine/util';
import { gameName, liveFeed, recent, type LiveBet } from './ticker';

export function gameCard(g: (typeof GAMES)[number]) {
  const card = h('a.game-card', { href: `#/game/${g.id}`, style: { '--c1': g.colors[0], '--c2': g.colors[1] } as any, 'aria-label': g.name },
    h('div.game-card-art',
      h('div.game-card-glow'),
      h('div.game-card-icon', svg(g.icon)),
      g.hot ? h('span.badge.hot', 'HOT') : g.isNew ? h('span.badge.new', 'NEW') : null,
    ),
    h('div.game-card-body', h('div.game-card-name', g.name), h('div.game-card-tag', g.tagline)),
  );
  // subtle 3D tilt on hover (pointer devices)
  card.addEventListener('pointermove', (e) => {
    if (e.pointerType !== 'mouse' || store.data.settings.reducedMotion) return;
    const r = card.getBoundingClientRect();
    const x = (e.clientX - r.left) / r.width - 0.5;
    const y = (e.clientY - r.top) / r.height - 0.5;
    card.style.transform = `perspective(700px) rotateY(${x * 10}deg) rotateX(${-y * 10}deg) translateY(-4px)`;
    card.style.setProperty('--mx', `${(x + 0.5) * 100}%`);
    card.style.setProperty('--my', `${(y + 0.5) * 100}%`);
  });
  card.addEventListener('pointerleave', () => (card.style.transform = ''));
  return card;
}

function liveRow(b: LiveBet) {
  const profit = b.payout - b.bet;
  return h(`tr${b.you ? '.you' : ''}`,
    h('td', h('span.live-game', gameName(b.game))),
    h('td', h('span.user', b.user)),
    h('td.hide-sm', fmt(b.bet)),
    h('td', b.mult > 0 ? fmtMult(b.mult) : '0.00×'),
    h(`td.${profit > 0 ? 'pos' : 'neg'}`, (profit > 0 ? '+' : '') + fmt(profit)),
  );
}

export function renderLobby(root: HTMLElement, cat: Category | 'all' = 'all'): () => void {
  const games = cat === 'all' ? GAMES : GAMES.filter((g) => g.categories.includes(cat));
  const catName = CATEGORIES.find((c) => c.id === cat)?.name ?? 'All Games';

  const hero = h('section.hero.glass',
    h('div.hero-text',
      h('div.hero-kicker', 'FREE PLAY-MONEY ARCADE'),
      h('h1.hero-title', 'Welcome to ', h('span.neon-text', APP_NAME)),
      h('p.hero-sub', '13 original neon games. Fake chips, real fun. No deposits, no prizes — just good vibes.'),
      h('div.hero-cta',
        h('a.btn-play.inline', { href: '#/game/plinko' }, 'Play Plinko'),
        h('a.btn-secondary', { href: '#/game/roulette' }, 'Try Roulette'),
      ),
    ),
    h('div.hero-art', ...[0, 1, 2, 3, 4, 5].map((i) => h(`div.hero-orb.o${i}`)), h('div.hero-chip.c1', '100'), h('div.hero-chip.c2', '25'), h('div.hero-chip.c3', '5K')),
  );

  const pills = h('div.cat-pills', CATEGORIES.map((c) => h(`a.cat-pill${c.id === cat ? '.active' : ''}`, { href: c.id === 'all' ? '#/' : `#/category/${c.id}` }, svg(c.icon), c.name)));

  const tbody = h('tbody', recent.slice(0, 12).map(liveRow));
  const off = liveFeed.on((b) => {
    tbody.prepend(liveRow(b));
    (tbody.firstElementChild as HTMLElement).classList.add('enter');
    while (tbody.children.length > 12) tbody.lastElementChild?.remove();
  });

  root.replaceChildren(
    h('div.lobby',
      cat === 'all' ? hero : null,
      pills,
      h('div.section-head', h('h2', catName), h('span.muted.small', `${games.length} games`)),
      h('div.game-grid', games.map(gameCard)),
      h('section.live-bets.glass',
        h('div.section-head', h('h2', h('span.live-dot'), 'Live bets'), h('span.muted.small', 'Bots with fake names — play money')),
        h('div.table-wrap', h('table.data.live', h('thead', h('tr', h('th', 'Game'), h('th', 'Player'), h('th.hide-sm', 'Bet'), h('th', 'Multi'), h('th', 'Profit'))), tbody)),
      ),
    ),
  );
  return off;
}
