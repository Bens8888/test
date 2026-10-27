/** "Provably fair" seeds panel + verifier (for fun — play money only). */
import { fair, generateFloats, sha256Hex } from '../engine/fair';
import {
  chickenHitLane, crashPoint, diceRoll, kenoDraw, limboResult, minesPositions, plinkoPath, rouletteNumber, wheelIndex,
  cardFromFloat, RANK_LABELS, SUITS, dieFromFloat,
} from '../engine/outcomes';
import { CHICKEN } from '../config';
import { toast } from '../engine/fx';
import { h } from '../engine/util';
import { openModal } from './modal';

const VERIFY_GAMES: Record<string, { label: string; param?: { label: string; def: number } ; floats: (p: number) => number; out: (f: number[], p: number) => string }> = {
  dice: { label: 'Dice', floats: () => 1, out: (f) => `Roll: ${diceRoll(f[0]).toFixed(2)}` },
  limbo: { label: 'Limbo', floats: () => 1, out: (f) => `Result: ${limboResult(f[0]).toFixed(2)}×` },
  crash: { label: 'Crash', floats: () => 1, out: (f) => `Crash point: ${crashPoint(f[0]).toFixed(2)}×` },
  plinko: { label: 'Plinko', param: { label: 'Rows', def: 16 }, floats: (p) => p, out: (f, p) => { const r = plinkoPath(f, p); return `Path: ${r.path.map((x) => (x ? 'R' : 'L')).join('')} → bucket ${r.bucket}`; } },
  roulette: { label: 'Roulette', floats: () => 1, out: (f) => `Number: ${rouletteNumber(f[0])}` },
  wheel: { label: 'Wheel', param: { label: 'Segments', def: 10 }, floats: () => 1, out: (f, p) => `Segment index: ${wheelIndex(f[0], p)}` },
  keno: { label: 'Keno', floats: () => 10, out: (f) => `Drawn: ${kenoDraw(f).join(', ')}` },
  mines: { label: 'Mines', param: { label: 'Mines', def: 3 }, floats: (p) => p, out: (f, p) => `Mine tiles (0-24): ${minesPositions(f, p).sort((a, b) => a - b).join(', ')}` },
  chicken: { label: 'Road Rush (medium)', floats: () => CHICKEN.difficulty.medium.lanes, out: (f) => { const l = chickenHitLane(f, CHICKEN.difficulty.medium.hit, CHICKEN.difficulty.medium.lanes); return l >= CHICKEN.difficulty.medium.lanes ? 'Survives every lane' : `Hit on lane ${l + 1}`; } },
  hilo: { label: 'Hi-Lo / Blackjack cards', floats: () => 8, out: (f) => `First cards: ${f.map((x) => { const c = cardFromFloat(x); return RANK_LABELS[c.rank] + SUITS[c.suit]; }).join(' ')}` },
  craps: { label: 'Craps roll', floats: () => 2, out: (f) => `Dice: ${dieFromFloat(f[0])} + ${dieFromFloat(f[1])} = ${dieFromFloat(f[0]) + dieFromFloat(f[1])}` },
};

function copyable(text: string) {
  const v = h('code.seed', text);
  return h('div.copy-row', v, h('button.mini', { type: 'button', onclick: () => { navigator.clipboard?.writeText(v.textContent || ''); toast('Copied'); } }, 'Copy'));
}

export function openFairness(gameId?: string) {
  const seedsTab = h('div.fair-tab');
  const verifyTab = h('div.fair-tab');
  verifyTab.hidden = true;

  const renderSeeds = async () => {
    const s = fair.state;
    const hash = await fair.serverSeedHash();
    const clientInput = h('input', { value: s.clientSeed, maxLength: 64 }) as HTMLInputElement;
    seedsTab.replaceChildren(
      h('p.muted.small', 'Every result comes from HMAC-SHA256(server seed, "client seed:nonce:round"). The server seed is hidden — only its SHA-256 hash is shown — until you rotate, then you can verify every past bet. It\'s all play money, this is just for fun.'),
      h('div.field', h('label.field-label', 'Active server seed (SHA-256 hash)'), copyable(hash)),
      h('div.field', h('label.field-label', 'Active client seed'), h('div.copy-row', clientInput, h('button.mini', { type: 'button', onclick: () => { fair.setClientSeed(clientInput.value); toast('Client seed changed — seed pair rotated'); renderSeeds(); } }, 'Change'))),
      h('div.field', h('label.field-label', 'Nonce (bets made with this pair)'), h('code.seed', String(s.nonce))),
      h('button.btn-secondary', { type: 'button', onclick: () => { fair.rotate(); toast('Seed pair rotated'); renderSeeds(); } }, 'Rotate seed pair & reveal server seed'),
      ...(s.prev
        ? [h('div.prev-seeds',
            h('h3', 'Previous seed pair (revealed)'),
            h('div.field', h('label.field-label', 'Server seed'), copyable(s.prev.serverSeed)),
            h('div.field', h('label.field-label', 'Client seed'), copyable(s.prev.clientSeed)),
            h('div.field', h('label.field-label', 'Bets made (nonces 0…n-1)'), h('code.seed', String(s.prev.nonce))),
            h('button.mini', { type: 'button', onclick: () => { showTab('verify'); fillVerify(s.prev!.serverSeed, s.prev!.clientSeed, Math.max(0, s.prev!.nonce - 1)); } }, 'Verify last bet ›'),
          )]
        : []),
    );
  };

  // verify tab
  const vServer = h('input', { placeholder: 'revealed server seed' }) as HTMLInputElement;
  const vClient = h('input', { placeholder: 'client seed' }) as HTMLInputElement;
  const vNonce = h('input', { type: 'number', min: '0', value: '0' }) as HTMLInputElement;
  const vGame = h('select', Object.entries(VERIFY_GAMES).map(([k, g]) => h('option', { value: k }, g.label))) as HTMLSelectElement;
  const vParam = h('input', { type: 'number', min: '1' }) as HTMLInputElement;
  const vParamLabel = h('label.field-label');
  const vParamWrap = h('div.field', vParamLabel, vParam);
  const vOut = h('div.verify-out');
  if (gameId && VERIFY_GAMES[gameId]) vGame.value = gameId;
  const syncParam = () => {
    const g = VERIFY_GAMES[vGame.value];
    vParamWrap.hidden = !g.param;
    if (g.param) {
      vParamLabel.textContent = g.param.label;
      vParam.value = String(g.param.def);
    }
  };
  syncParam();
  const run = async () => {
    const g = VERIFY_GAMES[vGame.value];
    const p = g.param ? Math.max(1, parseInt(vParam.value) || g.param.def) : 0;
    if (!vServer.value.trim()) {
      vOut.replaceChildren(h('p.muted', 'Enter a revealed server seed to verify.'));
      return;
    }
    const floats = await generateFloats(vServer.value.trim(), vClient.value.trim(), parseInt(vNonce.value) || 0, g.floats(p));
    const hash = await sha256Hex(vServer.value.trim());
    vOut.replaceChildren(
      h('div.field', h('label.field-label', 'SHA-256(server seed)'), h('code.seed', hash)),
      h('div.verify-result', g.out(floats, p)),
      h('div.muted.small', 'Floats: ' + floats.slice(0, 10).map((f) => f.toFixed(6)).join(', ') + (floats.length > 10 ? '…' : '')),
    );
  };
  [vServer, vClient, vNonce, vParam].forEach((el) => el.addEventListener('input', run));
  vGame.addEventListener('change', () => { syncParam(); run(); });
  const fillVerify = (s: string, c: string, n: number) => {
    vServer.value = s;
    vClient.value = c;
    vNonce.value = String(n);
    run();
  };
  verifyTab.append(
    h('div.field', h('label.field-label', 'Game'), vGame),
    h('div.field', h('label.field-label', 'Server seed'), vServer),
    h('div.grid2', h('div.field', h('label.field-label', 'Client seed'), vClient), h('div.field', h('label.field-label', 'Nonce'), vNonce)),
    vParamWrap,
    vOut,
  );
  run();

  const tabs = h('div.seg.tabs',
    h('button.active', { type: 'button', onclick: () => showTab('seeds') }, 'Seeds'),
    h('button', { type: 'button', onclick: () => showTab('verify') }, 'Verify'),
  );
  const showTab = (t: 'seeds' | 'verify') => {
    seedsTab.hidden = t !== 'seeds';
    verifyTab.hidden = t !== 'verify';
    tabs.children[0].classList.toggle('active', t === 'seeds');
    tabs.children[1].classList.toggle('active', t === 'verify');
  };
  renderSeeds();
  openModal('Provably fair', [tabs, seedsTab, verifyTab], { wide: true });
}
