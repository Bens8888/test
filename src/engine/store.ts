/** localStorage persistence for balance, stats, history, settings and fairness seeds. */
import { WALLET } from '../config';
import { Emitter, prefersReducedMotion } from './util';

const KEY = 'neonvault:v1';

export interface HistoryItem {
  t: number;
  bet: number;
  payout: number;
  mult: number;
  /** short human readable result, e.g. "Rolled 54.21" */
  detail?: string;
}

export interface GameStats {
  bets: number;
  wagered: number;
  won: number;
  wins: number;
  biggestWin: number;
  biggestMult: number;
  history: HistoryItem[];
}

export interface Settings {
  sound: boolean;
  volume: number;
  intensity: 'low' | 'normal' | 'high';
  reducedMotion: boolean;
  breakMinutes: number;
  welcomed: boolean;
}

export interface FairState {
  serverSeed: string;
  clientSeed: string;
  nonce: number;
  prev?: { serverSeed: string; clientSeed: string; nonce: number };
}

export interface SaveData {
  version: 1;
  balance: number;
  stats: {
    bets: number;
    wins: number;
    losses: number;
    wagered: number;
    won: number;
    biggestWin: { amount: number; game: string; mult: number };
    biggestMult: { mult: number; game: string };
    streak: number; // >0 win streak, <0 loss streak
    bestWinStreak: number;
    worstLossStreak: number;
    refills: number;
    createdAt: number;
  };
  games: Record<string, GameStats>;
  settings: Settings;
  fair: FairState | null;
}

function defaults(): SaveData {
  return {
    version: 1,
    balance: WALLET.startingBalance,
    stats: {
      bets: 0,
      wins: 0,
      losses: 0,
      wagered: 0,
      won: 0,
      biggestWin: { amount: 0, game: '', mult: 0 },
      biggestMult: { mult: 0, game: '' },
      streak: 0,
      bestWinStreak: 0,
      worstLossStreak: 0,
      refills: 0,
      createdAt: Date.now(),
    },
    games: {},
    settings: {
      sound: true,
      volume: 0.6,
      intensity: 'normal',
      reducedMotion: prefersReducedMotion(),
      breakMinutes: 30,
      welcomed: false,
    },
    fair: null,
  };
}

function load(): SaveData {
  const base = defaults();
  try {
    const raw = localStorage.getItem(KEY);
    if (!raw) return base;
    const data = JSON.parse(raw);
    if (!data || data.version !== 1) return base;
    return {
      ...base,
      ...data,
      stats: { ...base.stats, ...data.stats },
      settings: { ...base.settings, ...data.settings },
      games: data.games || {},
      balance: Number.isFinite(data.balance) ? data.balance : base.balance,
    };
  } catch {
    return base;
  }
}

export const store = {
  data: load(),
  changed: new Emitter<SaveData>(),
  saveTimer: 0 as any,
  save() {
    try {
      localStorage.setItem(KEY, JSON.stringify(this.data));
    } catch {
      /* storage full or blocked: keep playing in memory */
    }
  },
  /** batched save + change notification */
  touch() {
    this.changed.emit(this.data);
    clearTimeout(this.saveTimer);
    this.saveTimer = setTimeout(() => this.save(), 150);
  },
  gameStats(id: string): GameStats {
    let g = this.data.games[id];
    if (!g) {
      g = this.data.games[id] = { bets: 0, wagered: 0, won: 0, wins: 0, biggestWin: 0, biggestMult: 0, history: [] };
    }
    return g;
  },
  reset() {
    const settings = this.data.settings;
    this.data = defaults();
    this.data.settings = { ...settings };
    this.save();
    this.changed.emit(this.data);
  },
};

window.addEventListener('beforeunload', () => store.save());

export const settings = () => store.data.settings;
