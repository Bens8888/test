/** Game catalogue: metadata, lobby art and lazy loaders (one file per game). */

export type Category = 'originals' | 'table' | 'cards' | 'instant';

export interface GameMeta {
  id: string;
  name: string;
  tagline: string;
  categories: Category[];
  colors: [string, string];
  icon: string; // inline SVG
  rtp: string;
  load: () => Promise<{ default: GameMount }>;
  isNew?: boolean;
  hot?: boolean;
}

export type GameMount = (root: HTMLElement, meta: GameMeta) => () => void;

const S = (body: string) =>
  `<svg viewBox="0 0 64 64" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round" stroke-linejoin="round">${body}</svg>`;

export const GAMES: GameMeta[] = [
  {
    id: 'dice', name: 'Dice', tagline: 'Roll over or under', categories: ['originals', 'instant'], colors: ['#22e4ff', '#5b3dff'], rtp: '99%', hot: true,
    icon: S('<rect x="10" y="10" width="44" height="44" rx="10"/><circle cx="22" cy="22" r="3" fill="currentColor"/><circle cx="42" cy="22" r="3" fill="currentColor"/><circle cx="32" cy="32" r="3" fill="currentColor"/><circle cx="22" cy="42" r="3" fill="currentColor"/><circle cx="42" cy="42" r="3" fill="currentColor"/>'),
    load: () => import('./dice'),
  },
  {
    id: 'plinko', name: 'Plinko', tagline: 'Drop it like it\'s hot', categories: ['originals', 'instant'], colors: ['#ff3d9a', '#7a2bff'], rtp: '≈99%', hot: true,
    icon: S('<circle cx="32" cy="12" r="2.5" fill="currentColor"/><circle cx="24" cy="22" r="2.5" fill="currentColor"/><circle cx="40" cy="22" r="2.5" fill="currentColor"/><circle cx="16" cy="32" r="2.5" fill="currentColor"/><circle cx="32" cy="32" r="2.5" fill="currentColor"/><circle cx="48" cy="32" r="2.5" fill="currentColor"/><circle cx="24" cy="42" r="2.5" fill="currentColor"/><circle cx="40" cy="42" r="2.5" fill="currentColor"/><path d="M8 52h48M18 52v6M32 52v6M46 52v6"/><circle cx="34" cy="14" r="5" fill="currentColor" stroke="none" opacity=".9"/>'),
    load: () => import('./plinko'),
  },
  {
    id: 'roulette', name: 'Roulette', tagline: 'European single zero', categories: ['table'], colors: ['#ff4d6d', '#2a0f4f'], rtp: '97.3%', hot: true,
    icon: S('<circle cx="32" cy="32" r="24"/><circle cx="32" cy="32" r="15"/><circle cx="32" cy="32" r="5" fill="currentColor"/><path d="M32 8v9M32 47v9M8 32h9M47 32h9M15 15l6 6M43 43l6 6M49 15l-6 6M21 43l-6 6"/><circle cx="44" cy="22" r="3" fill="#fff" stroke="none"/>'),
    load: () => import('./roulette'),
  },
  {
    id: 'crash', name: 'Crash', tagline: 'Cash out before it pops', categories: ['originals'], colors: ['#ffc94d', '#ff3d6e'], rtp: '99%', hot: true,
    icon: S('<path d="M8 54C24 52 36 42 48 16"/><path d="M42 14l8-2-1 8"/><path d="M8 56h48M8 8v48"/>'),
    load: () => import('./crash'),
  },
  {
    id: 'mines', name: 'Mines', tagline: 'Find gems, dodge mines', categories: ['originals'], colors: ['#2dff8a', '#0b6b62'], rtp: '99%',
    icon: S('<path d="M20 14h24l10 12-22 26L10 26z"/><path d="M10 26h44M24 14l-4 12 12 26 12-26-4-12"/>'),
    load: () => import('./mines'),
  },
  {
    id: 'limbo', name: 'Limbo', tagline: 'How high can you go?', categories: ['originals', 'instant'], colors: ['#9b5cff', '#ff3d9a'], rtp: '99%',
    icon: S('<path d="M32 6l6 14h-4v14h-4V20h-4z" fill="currentColor"/><path d="M14 44h36M20 52h24"/><circle cx="32" cy="40" r="2" fill="currentColor"/>'),
    load: () => import('./limbo'),
  },
  {
    id: 'blackjack', name: 'Blackjack', tagline: 'Beat the dealer to 21', categories: ['cards', 'table'], colors: ['#1fd3a0', '#123a6b'], rtp: '99.5%',
    icon: S('<rect x="10" y="12" width="26" height="38" rx="4" transform="rotate(-10 23 31)"/><rect x="28" y="12" width="26" height="38" rx="4" transform="rotate(10 41 31)"/><path d="M41 24l-4 8h8z" fill="currentColor"/><path d="M41 32v6"/>'),
    load: () => import('./blackjack'),
  },
  {
    id: 'craps', name: 'Craps', tagline: 'Pass line, odds & the field', categories: ['table'], colors: ['#ff8a3d', '#6b1240'], rtp: '98.6%',
    icon: S('<rect x="6" y="20" width="26" height="26" rx="6" transform="rotate(-12 19 33)"/><rect x="32" y="18" width="26" height="26" rx="6" transform="rotate(14 45 31)"/><circle cx="19" cy="33" r="2.5" fill="currentColor"/><circle cx="40" cy="25" r="2.5" fill="currentColor"/><circle cx="50" cy="37" r="2.5" fill="currentColor"/>'),
    load: () => import('./craps'),
  },
  {
    id: 'hilo', name: 'Hi-Lo', tagline: 'Higher or lower? Chain it', categories: ['originals', 'cards'], colors: ['#22e4ff', '#1a7a4f'], rtp: '99%',
    icon: S('<rect x="16" y="10" width="32" height="44" rx="5"/><path d="M32 18l7 9H25zM32 46l7-9H25z" fill="currentColor"/>'),
    load: () => import('./hilo'),
  },
  {
    id: 'keno', name: 'Keno', tagline: 'Pick up to 10 numbers', categories: ['originals', 'instant'], colors: ['#ff3dd2', '#3d5bff'], rtp: '99%',
    icon: S('<circle cx="20" cy="20" r="9"/><circle cx="44" cy="20" r="9"/><circle cx="20" cy="44" r="9"/><circle cx="44" cy="44" r="9" fill="currentColor"/><path d="M18 17l3-1v8M42 17h4l-4 7"/>'),
    load: () => import('./keno'),
  },
  {
    id: 'wheel', name: 'Wheel', tagline: 'Spin for multipliers', categories: ['originals', 'instant'], colors: ['#ffc94d', '#9b5cff'], rtp: '99%',
    icon: S('<circle cx="32" cy="34" r="22"/><path d="M32 12v44M10 34h44M16.5 18.5l31 31M47.5 18.5l-31 31"/><path d="M26 4h12l-6 9z" fill="currentColor"/>'),
    load: () => import('./wheel'),
  },
  {
    id: 'tower', name: 'Tower', tagline: 'Climb to the top', categories: ['originals'], colors: ['#7affd4', '#5b3dff'], rtp: '98%',
    icon: S('<rect x="12" y="44" width="40" height="10" rx="2"/><rect x="16" y="32" width="32" height="10" rx="2"/><rect x="20" y="20" width="24" height="10" rx="2"/><path d="M32 6l6 10H26z" fill="currentColor"/>'),
    load: () => import('./tower'),
  },
  {
    id: 'chicken', name: 'Road Rush', tagline: 'Help Bolt cross the lanes', categories: ['originals'], colors: ['#ffe14d', '#ff3d6e'], rtp: '98%', isNew: true,
    icon: S('<path d="M22 8c-2 6-2 12 2 16M42 8c2 6 2 12-2 16"/><circle cx="32" cy="34" r="14"/><circle cx="27" cy="32" r="2.5" fill="currentColor"/><circle cx="37" cy="32" r="2.5" fill="currentColor"/><path d="M28 40c2.5 2 5.5 2 8 0"/><path d="M6 56h12M26 56h12M46 56h12"/>'),
    load: () => import('./chicken'),
  },
];

export const CATEGORIES: { id: Category | 'all'; name: string; icon: string }[] = [
  { id: 'all', name: 'All Games', icon: S('<rect x="10" y="10" width="18" height="18" rx="4"/><rect x="36" y="10" width="18" height="18" rx="4"/><rect x="10" y="36" width="18" height="18" rx="4"/><rect x="36" y="36" width="18" height="18" rx="4"/>') },
  { id: 'originals', name: 'Originals', icon: S('<path d="M32 6l7 16 17 2-13 11 4 17-15-9-15 9 4-17L8 24l17-2z"/>') },
  { id: 'instant', name: 'Instant Win', icon: S('<path d="M36 6L14 36h16l-4 22 24-32H34z"/>') },
  { id: 'table', name: 'Table Games', icon: S('<circle cx="32" cy="32" r="22"/><circle cx="32" cy="32" r="8"/>') },
  { id: 'cards', name: 'Card Games', icon: S('<rect x="14" y="10" width="28" height="40" rx="4"/><path d="M22 54h24a4 4 0 004-4V18"/>') },
];

export const gameById = (id: string) => GAMES.find((g) => g.id === id);
