/**
 * NEON VAULT — central game configuration.
 *
 * Everything here is PLAY MONEY. Tune house edges, payout tables and limits in this
 * single file. Payout tables for Plinko / Keno / Wheel are generated from these
 * parameters at startup (see engine/paytables.ts) so the RTP always matches
 * `1 - houseEdge` (minus a hair for rounding to friendly numbers).
 */

export const APP_NAME = 'NEON VAULT';

export const WALLET = {
  startingBalance: 10_000,
  refillAmount: 10_000,
  /** Refill button appears when balance drops below this. */
  refillThreshold: 1,
  minBet: 0.01,
  maxBet: 100_000,
  maxPayout: 10_000_000,
};

/** House edge per "original" game (0.01 = 1%, RTP 99%). */
export const HOUSE_EDGE = {
  dice: 0.01,
  limbo: 0.01,
  crash: 0.01,
  plinko: 0.01,
  mines: 0.01,
  hilo: 0.01,
  keno: 0.01,
  wheel: 0.01,
  tower: 0.02,
  chicken: 0.02,
} as const;

export const DICE = {
  minChance: 0.01,
  maxChance: 98,
};

export const LIMBO = {
  minTarget: 1.01,
  maxTarget: 1_000_000,
};

export const CRASH = {
  /** Seconds of betting window between rounds. */
  bettingSeconds: 6,
  /** Multiplier growth: m(t) = e^(growth * t) with t in seconds. */
  growth: 0.085,
  maxMultiplier: 1_000_000,
  botCount: 14,
};

export const PLINKO = {
  minRows: 8,
  maxRows: 16,
  /**
   * Bucket payout = center + s * (e^(k*d) - 1) / (e^k - 1), d = distance from centre (0..1),
   * k = k0 + kPerRow * (rows - 8). `s` is solved so RTP = 1 - houseEdge.
   */
  risk: {
    low: { center: 0.5, k0: 2.4, kPerRow: 0.2 },
    medium: { center: 0.4, k0: 3.6, kPerRow: 0.3 },
    high: { center: 0.2, k0: 5.6, kPerRow: 0.5 },
  },
};

export const MINES = {
  tiles: 25,
  minMines: 1,
  maxMines: 24,
  defaultMines: 3,
};

export const TOWER = {
  rows: 9,
  /** tiles per row & how many of them are bombs */
  difficulty: {
    easy: { tiles: 4, bombs: 1 },
    medium: { tiles: 3, bombs: 1 },
    hard: { tiles: 2, bombs: 1 },
    expert: { tiles: 3, bombs: 2 },
    master: { tiles: 4, bombs: 3 },
  },
};

export const CHICKEN = {
  /** Probability of getting hit on each lane & number of lanes */
  difficulty: {
    easy: { hit: 1 / 25, lanes: 24 },
    medium: { hit: 3 / 25, lanes: 22 },
    hard: { hit: 5 / 25, lanes: 20 },
    daredevil: { hit: 10 / 25, lanes: 15 },
  },
};

export const HILO = {
  /** Cap on a single step's multiplier (e.g. betting "higher" on an Ace). */
  maxStepMultiplier: 12.87,
};

export const KENO = {
  numbers: 40,
  drawn: 10,
  maxPicks: 10,
  /**
   * Relative payout "shapes" per risk, indexed [picks][hits]. These are scaled so the
   * RTP equals 1 - houseEdge. A zero stays a zero.
   */
  shapes: {
    classic: [
      [],
      [0, 3.96],
      [0, 1.9, 4.5],
      [0, 1, 3.1, 10.4],
      [0, 0.8, 1.8, 5, 22.5],
      [0, 0.25, 1.4, 4.1, 16.5, 36],
      [0, 0, 1, 3.68, 7, 16.5, 40],
      [0, 0, 0.47, 3, 4.5, 14, 31, 60],
      [0, 0, 0, 2.2, 4, 13, 22, 55, 70],
      [0, 0, 0, 1.55, 3, 8, 15, 44, 60, 85],
      [0, 0, 0, 1.4, 2.25, 4.5, 8, 17, 50, 80, 100],
    ],
    low: [
      [],
      [0.7, 1.85],
      [0, 2, 3.8],
      [0, 1.1, 1.38, 26],
      [0, 0, 2.2, 7.9, 90],
      [0, 0, 1.5, 4.2, 13, 300],
      [0, 0, 1.1, 2, 6.2, 100, 700],
      [0, 0, 1.1, 1.6, 3.5, 15, 225, 700],
      [0, 0, 1.1, 1.5, 2, 5.5, 39, 100, 800],
      [0, 0, 1.1, 1.3, 1.7, 2.5, 7.5, 50, 250, 1000],
      [0, 0, 1.1, 1.2, 1.3, 1.8, 3.5, 13, 50, 250, 1000],
    ],
    medium: [
      [],
      [0.4, 2.75],
      [0, 1.8, 5.1],
      [0, 0, 2.8, 50],
      [0, 0, 1.7, 10, 100],
      [0, 0, 1.4, 4, 14, 390],
      [0, 0, 0, 3, 9, 180, 710],
      [0, 0, 0, 2, 7, 30, 400, 800],
      [0, 0, 0, 2, 4, 11, 67, 400, 900],
      [0, 0, 0, 2, 2.5, 5, 15, 100, 500, 1000],
      [0, 0, 0, 1.6, 2, 4, 7, 26, 100, 500, 1000],
    ],
    high: [
      [],
      [0, 3.96],
      [0, 0, 17.1],
      [0, 0, 0, 81.5],
      [0, 0, 0, 10, 259],
      [0, 0, 0, 4.5, 48, 450],
      [0, 0, 0, 0, 11, 350, 710],
      [0, 0, 0, 0, 7, 90, 400, 800],
      [0, 0, 0, 0, 5, 20, 270, 600, 900],
      [0, 0, 0, 0, 4, 11, 56, 500, 800, 1000],
      [0, 0, 0, 0, 3.5, 8, 13, 63, 500, 800, 1000],
    ],
  },
};

export const WHEEL = {
  segmentOptions: [10, 20, 30, 40, 50],
  /**
   * Segment patterns (repeated around the wheel). Numbers are relative weights that get
   * scaled so the average multiplier equals 1 - houseEdge. 0 stays 0.
   */
  patterns: {
    low: [1.5, 1.2, 1.2, 1.2, 0, 1.2, 1.2, 1.2, 1.2, 0],
    medium: [0, 1.9, 0, 1.5, 0, 2, 0, 1.5, 0, 3, 0, 1.6, 0, 2, 0, 1.5, 0, 3, 0, 1.8],
    /** high risk: one jackpot segment, the rest are zero */
    high: 'jackpot',
  },
};

export const ROULETTE = {
  /** Standard European single-zero payouts (to 1). House edge 2.70%. */
  payouts: { straight: 35, split: 17, street: 11, corner: 8, sixline: 5, dozen: 2, column: 2, even: 1 },
  chips: [1, 5, 10, 25, 100, 500, 1000, 5000],
  maxTableBet: 100_000,
};

export const CRAPS = {
  chips: [1, 5, 10, 25, 100, 500, 1000],
  /** place bet payouts (win:stake) */
  place: { 4: [9, 5], 5: [7, 5], 6: [7, 6], 8: [7, 6], 9: [7, 5], 10: [9, 5] } as Record<number, [number, number]>,
  /** field: 2 pays 2:1, 12 pays 3:1, 3/4/9/10/11 pay 1:1 */
  field: { 2: 2, 3: 1, 4: 1, 9: 1, 10: 1, 11: 1, 12: 3 } as Record<number, number>,
  /** max odds multiple of the flat bet (3-4-5x) */
  maxOdds: { 4: 3, 10: 3, 5: 4, 9: 4, 6: 5, 8: 5 } as Record<number, number>,
};

export const BLACKJACK = {
  blackjackPays: 1.5,
  dealerHitsSoft17: false,
  maxSplits: 3,
  doubleAfterSplit: true,
  decks: 6,
};

export const SESSION = {
  /** Show the friendly "take a break" reminder after this many minutes of play. */
  breakReminderMinutes: 30,
};

export const BOT_NAMES = [
  'NeonNomad', 'LuckyLlama', 'PixelPanda', 'ByteBandit', 'CosmicCat', 'VaporViper', 'ChipMunk42',
  'SpinDoctor', 'GlowWorm', 'RetroRocket', 'MidnightMoth', 'StarDust', 'ZigZagZed', 'FuzzyDice',
  'OrbitOwl', 'TurboTaco', 'QuietQuokka', 'JollyJackal', 'NovaNinja', 'ElectricEel', 'WaffleWizard',
  'MangoMamba', 'HoloHippo', 'SynthSloth', 'DiscoDingo', 'LaserLemur', 'PlasmaPug', 'CometCrab',
  'GummyGolem', 'VelvetVole', 'BlipBlop', 'SirSpinsALot', 'RubyRaccoon', 'NightNarwhal', 'AquaAxolotl',
  'TinyTitan', 'MoonMuffin', 'KiwiKnight', 'BubbleBison', 'PrismPenguin', 'EchoEmu', 'SaltySeal',
];
