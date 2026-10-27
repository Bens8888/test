/** App frame: top bar, sidebar, ticker, router and footer. */
import { APP_NAME, WALLET } from '../config';
import { CATEGORIES, GAMES, gameById, type Category } from '../games/registry';
import { store } from '../engine/store';
import { wallet, walletEvents } from '../engine/wallet';
import { fx, toast } from '../engine/fx';
import { sound } from '../engine/sound';
import { fmt, h, svg } from '../engine/util';
import { renderLobby } from './lobby';
import { openFairness } from './fairPanel';
import { openBreakReminder, openSettings, openStats, openWelcome } from './panels';
import { describeWin, liveFeed, recent, startBots } from './ticker';

const ICONS = {
  menu: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 6h16M4 12h16M4 18h16"/></svg>',
  sound: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 5L6 9H3v6h3l5 4z"/><path d="M15.5 8.5a5 5 0 010 7M18.5 5.5a9 9 0 010 13"/></svg>',
  mute: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M11 5L6 9H3v6h3l5 4z"/><path d="M22 9l-6 6M16 9l6 6"/></svg>',
  gear: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.7 1.7 0 00.3 1.8l.1.1a2 2 0 11-2.8 2.8l-.1-.1a1.7 1.7 0 00-1.8-.3 1.7 1.7 0 00-1 1.5V21a2 2 0 01-4 0v-.1a1.7 1.7 0 00-1.1-1.5 1.7 1.7 0 00-1.8.3l-.1.1a2 2 0 11-2.8-2.8l.1-.1a1.7 1.7 0 00.3-1.8 1.7 1.7 0 00-1.5-1H3a2 2 0 010-4h.1a1.7 1.7 0 001.5-1.1 1.7 1.7 0 00-.3-1.8l-.1-.1a2 2 0 112.8-2.8l.1.1a1.7 1.7 0 001.8.3H9a1.7 1.7 0 001-1.5V3a2 2 0 014 0v.1a1.7 1.7 0 001 1.5 1.7 1.7 0 001.8-.3l.1-.1a2 2 0 112.8 2.8l-.1.1a1.7 1.7 0 00-.3 1.8V9a1.7 1.7 0 001.5 1H21a2 2 0 010 4h-.1a1.7 1.7 0 00-1.5 1z"/></svg>',
  stats: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round"><path d="M4 20V10M10 20V4M16 20v-7M22 20H2"/></svg>',
  shield: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M12 2l8 4v6c0 5-3.5 8.5-8 10-4.5-1.5-8-5-8-10V6z"/><path d="M8.5 12l2.5 2.5 4.5-5"/></svg>',
  home: '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><path d="M3 11l9-8 9 8M5 9.5V21h14V9.5"/></svg>',
};

const LOGO = `<svg viewBox="0 0 40 40"><defs><linearGradient id="lg" x1="0" y1="0" x2="1" y2="1"><stop offset="0" stop-color="#22e4ff"/><stop offset=".5" stop-color="#9b5cff"/><stop offset="1" stop-color="#ff3d9a"/></linearGradient></defs><path d="M20 3l14.7 8.5v17L20 37 5.3 28.5v-17z" fill="none" stroke="url(#lg)" stroke-width="3"/><path d="M13 14l7 13 7-13" fill="none" stroke="#fff" stroke-width="3" stroke-linecap="round" stroke-linejoin="round"/></svg>`;

export function startApp(root: HTMLElement) {
  document.documentElement.classList.toggle('reduced-motion', store.data.settings.reducedMotion);
  startBots();

  // ---------------------------------------------------------------- top bar
  const balanceEl = h('span.balance-amount', fmt(wallet.balance));
  const refillBtn = h('button.refill-btn', { type: 'button', title: 'Free refill', onclick: () => doRefill() }, '+ Free refill');
  const balancePill = h('div.balance-pill', h('span.chip-icon'), balanceEl, h('span.balance-cur', 'chips'));
  const muteBtn = h('button.icon-btn', { type: 'button', title: 'Sound on/off', 'aria-label': 'Toggle sound' });
  const renderMute = () => muteBtn.replaceChildren(svg(store.data.settings.sound ? ICONS.sound : ICONS.mute));
  renderMute();
  muteBtn.onclick = () => {
    store.data.settings.sound = !store.data.settings.sound;
    store.touch();
    renderMute();
    sound.play('click');
  };
  document.addEventListener('settings-changed', renderMute);
  const menuBtn = h('button.icon-btn.menu-btn', { type: 'button', 'aria-label': 'Menu', onclick: () => document.body.classList.toggle('nav-open') }, svg(ICONS.menu));

  const topbar = h('header.topbar',
    h('div.topbar-left', menuBtn, h('a.logo', { href: '#/' }, svg(LOGO), h('span.logo-text', APP_NAME.split(' ')[0], h('b', APP_NAME.split(' ')[1] ?? '')))),
    h('div.topbar-center', balancePill, refillBtn),
    h('div.topbar-right',
      h('button.icon-btn', { type: 'button', title: 'Provably fair', 'aria-label': 'Provably fair', onclick: () => openFairness() }, svg(ICONS.shield)),
      h('button.icon-btn', { type: 'button', title: 'Stats', 'aria-label': 'Stats', onclick: () => openStats() }, svg(ICONS.stats)),
      muteBtn,
      h('button.icon-btn', { type: 'button', title: 'Settings', 'aria-label': 'Settings', onclick: () => openSettings() }, svg(ICONS.gear)),
    ),
  );

  const updateRefill = () => refillBtn.classList.toggle('show', wallet.canRefill());
  updateRefill();
  walletEvents.balance.on(({ from, to }) => {
    fx.countUp(balanceEl, from, to, { duration: to > from ? 700 : 250 });
    if (to > from) {
      balancePill.classList.remove('up');
      void balancePill.offsetWidth;
      balancePill.classList.add('up');
    }
    updateRefill();
  });
  store.changed.on(() => {
    if (balanceEl.textContent !== fmt(wallet.balance) && !(balanceEl as any)._countRaf) balanceEl.textContent = fmt(wallet.balance);
    updateRefill();
  });
  walletEvents.insufficient.on(() => {
    sound.play('error');
    balancePill.classList.remove('nope');
    void balancePill.offsetWidth;
    balancePill.classList.add('nope');
    toast(wallet.canRefill() ? 'Out of chips — grab a free refill!' : 'Not enough chips for that bet', 'error');
  });
  function doRefill() {
    if (wallet.refill()) {
      sound.play('cashout');
      fx.coinShower(40);
      toast(`+${fmt(WALLET.refillAmount)} free play chips. Have fun!`, 'success');
    }
  }

  // ---------------------------------------------------------------- ticker
  const track = h('div.ticker-track');
  const addTick = (b: (typeof recent)[number]) => {
    if (b.mult < 2 && !b.you) return;
    if (b.you && b.payout <= b.bet) return;
    const item = h(`span.tick-item${b.you ? '.you' : ''}${b.mult >= 10 ? '.big' : ''}`, describeWin(b));
    track.appendChild(item);
    while (track.children.length > 24) track.firstElementChild?.remove();
  };
  recent.slice().reverse().forEach(addTick);
  liveFeed.on(addTick);
  const ticker = h('div.ticker', h('span.ticker-label', h('span.live-dot'), 'RECENT WINS'), h('div.ticker-viewport', track));
  // continuous marquee
  let offset = 0;
  let lastT = performance.now();
  const marquee = (now: number) => {
    const dt = Math.min(0.05, (now - lastT) / 1000);
    lastT = now;
    if (!store.data.settings.reducedMotion) {
      offset += dt * 45;
      const first = track.firstElementChild as HTMLElement | null;
      if (first && offset > first.offsetWidth + 32) {
        offset -= first.offsetWidth + 32;
        track.appendChild(first);
      }
      track.style.transform = `translateX(${-offset}px)`;
    }
    requestAnimationFrame(marquee);
  };
  requestAnimationFrame(marquee);

  // ---------------------------------------------------------------- sidebar
  const navLinks: HTMLAnchorElement[] = [];
  const link = (href: string, icon: string, label: string, extra?: HTMLElement) => {
    const a = h('a.nav-link', { href, onclick: () => document.body.classList.remove('nav-open') }, svg(icon), h('span', label), extra ?? null) as HTMLAnchorElement;
    navLinks.push(a);
    return a;
  };
  const sidebar = h('nav.sidebar',
    h('div.nav-section', link('#/', ICONS.home, 'Lobby')),
    h('div.nav-title', 'Categories'),
    h('div.nav-section', CATEGORIES.filter((c) => c.id !== 'all').map((c) => link(`#/category/${c.id}`, c.icon, c.name))),
    h('div.nav-title', 'Games'),
    h('div.nav-section.games', GAMES.map((g) => link(`#/game/${g.id}`, g.icon, g.name, h('span.nav-dot', { style: { background: g.colors[0] } })))),
    h('div.nav-section.mobile-only',
      h('button.nav-link', { type: 'button', onclick: () => { document.body.classList.remove('nav-open'); openFairness(); } }, svg(ICONS.shield), h('span', 'Provably fair')),
      h('button.nav-link', { type: 'button', onclick: () => { document.body.classList.remove('nav-open'); openStats(); } }, svg(ICONS.stats), h('span', 'My stats')),
    ),
    h('div.nav-foot', h('div.play-badge', '🎟 Play money only'), h('div.muted.small', 'No real money. No prizes.')),
  );
  const scrim = h('div.nav-scrim', { onclick: () => document.body.classList.remove('nav-open') });

  // ---------------------------------------------------------------- main
  const view = h('div.view');
  const footer = h('footer.footer',
    h('div.play-badge', '🎟 Play money only – no real prizes'),
    h('p.muted.small', `${APP_NAME} is a free game for entertainment. Chips are fake and have no cash value, can't be bought, sold or withdrawn. Nothing here is real gambling. No ads, no accounts, no links to gambling sites.`),
    h('p.muted.small', 'Feeling like gaming stops being fun? Take a break — your chips will be here.'),
  );
  const main = h('main.app-main', ticker, view, footer);
  root.replaceChildren(topbar, h('div.app-body', sidebar, scrim, main));

  // ---------------------------------------------------------------- router
  let cleanup: (() => void) | null = null;
  let navToken = 0;
  async function route() {
    const hash = location.hash || '#/';
    const token = ++navToken;
    cleanup?.();
    cleanup = null;
    document.body.classList.remove('nav-open');
    navLinks.forEach((a) => a.classList.toggle('active', a.getAttribute('href') === hash || (hash === '#' && a.getAttribute('href') === '#/')));
    const gm = hash.match(/^#\/game\/([\w-]+)/);
    const cm = hash.match(/^#\/category\/([\w-]+)/);
    main.scrollTop = 0;
    window.scrollTo(0, 0);
    if (gm) {
      const meta = gameById(gm[1]);
      if (!meta) {
        location.hash = '#/';
        return;
      }
      document.title = `${meta.name} · ${APP_NAME}`;
      view.replaceChildren(h('div.loading', h('div.spinner'), 'Loading ' + meta.name + '…'));
      try {
        const mod = await meta.load();
        if (token !== navToken) return;
        const off = mod.default(view, meta);
        cleanup = off;
      } catch (err) {
        console.error(err);
        view.replaceChildren(h('div.loading', 'Could not load game. ', h('a', { href: '#/' }, 'Back to lobby')));
      }
    } else {
      document.title = `${APP_NAME} · Free play-money arcade`;
      const cat = (cm?.[1] as Category) ?? 'all';
      cleanup = renderLobby(view, CATEGORIES.some((c) => c.id === cat) ? cat : 'all');
    }
  }
  window.addEventListener('hashchange', route);
  window.addEventListener('beforeunload', () => {
    cleanup?.();
    cleanup = null;
    store.save(); // persist anything the game refunded/settled on the way out
  });
  route();

  // ---------------------------------------------------------------- welcome & breaks
  if (!store.data.settings.welcomed) setTimeout(openWelcome, 400);
  let playedMs = 0;
  let lastTick = Date.now();
  setInterval(() => {
    const now = Date.now();
    if (document.visibilityState === 'visible') playedMs += now - lastTick;
    lastTick = now;
    const mins = store.data.settings.breakMinutes;
    if (mins > 0 && playedMs >= mins * 60_000 && !document.querySelector('.modal-backdrop')) {
      playedMs = 0;
      openBreakReminder(mins, () => {});
    }
  }, 5000);
}
