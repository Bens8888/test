/** Settings, stats, welcome and "take a break" dialogs. */
import { APP_NAME } from '../config';
import { store } from '../engine/store';
import { sound } from '../engine/sound';
import { toast } from '../engine/fx';
import { fmt, fmtMult, h } from '../engine/util';
import { gameName } from './ticker';
import { openModal } from './modal';

function seg<T extends string>(options: { value: T; label: string }[], current: T, onChange: (v: T) => void) {
  const wrap = h('div.seg');
  options.forEach((o) => {
    const b = h('button', { type: 'button', class: o.value === current ? 'active' : '' }, o.label);
    b.onclick = () => {
      wrap.querySelectorAll('button').forEach((x) => x.classList.remove('active'));
      b.classList.add('active');
      sound.play('click');
      onChange(o.value);
    };
    wrap.appendChild(b);
  });
  return wrap;
}

function toggle(on: boolean, onChange: (v: boolean) => void) {
  const t = h('button.toggle', { type: 'button', role: 'switch', 'aria-checked': String(on), class: on ? 'on' : '' }, h('span'));
  t.onclick = () => {
    on = !on;
    t.classList.toggle('on', on);
    t.setAttribute('aria-checked', String(on));
    onChange(on);
    sound.play('click');
  };
  return t;
}

export function openSettings() {
  const s = store.data.settings;
  const vol = h('input', { type: 'range', min: '0', max: '1', step: '0.05', value: String(s.volume) }) as HTMLInputElement;
  vol.oninput = () => {
    s.volume = parseFloat(vol.value);
    store.touch();
  };
  vol.onchange = () => sound.play('chip');
  const row = (label: string, desc: string, control: HTMLElement) => h('div.setting-row', h('div', h('div.setting-label', label), h('div.muted.small', desc)), control);
  openModal('Settings', [
    row('Sound effects', 'Generated with Web Audio — no files.', toggle(s.sound, (v) => { s.sound = v; store.touch(); document.dispatchEvent(new Event('settings-changed')); })),
    row('Volume', '', vol),
    row('Animation intensity', 'Particles, confetti and effects.', seg([{ value: 'low', label: 'Low' }, { value: 'normal', label: 'Normal' }, { value: 'high', label: 'High' }], s.intensity, (v) => { s.intensity = v; store.touch(); })),
    row('Reduced motion', 'Turns off shake and most movement.', toggle(s.reducedMotion, (v) => { s.reducedMotion = v; store.touch(); document.documentElement.classList.toggle('reduced-motion', v); })),
    row('Break reminder', 'Friendly nudge after playing a while.', seg([{ value: '15', label: '15m' }, { value: '30', label: '30m' }, { value: '60', label: '60m' }, { value: '0', label: 'Off' }], String(s.breakMinutes) as any, (v) => { s.breakMinutes = parseInt(v); store.touch(); })),
    h('div.setting-row.danger-zone',
      h('div', h('div.setting-label', 'Reset everything'), h('div.muted.small', 'Balance back to 10,000, stats & history cleared.')),
      h('button.btn-secondary.danger', { type: 'button', onclick: () => { if (confirm('Reset balance, stats and history?')) { store.reset(); toast('Fresh start! 10,000 play chips added.', 'success'); } } }, 'Reset'),
    ),
  ]);
}

export function openStats() {
  const st = store.data.stats;
  const profit = st.won - st.wagered;
  const card = (label: string, value: string, cls = '') => h('div.stat-card', h('div.muted.small', label), h(`div.stat-value${cls ? '.' + cls : ''}`, value));
  const games = Object.entries(store.data.games).filter(([, g]) => g.bets > 0).sort((a, b) => b[1].wagered - a[1].wagered);
  openModal('Your stats', [
    h('div.stat-grid',
      card('Total bets', st.bets.toLocaleString()),
      card('Total wagered', fmt(st.wagered)),
      card('Net profit', (profit >= 0 ? '+' : '') + fmt(profit), profit >= 0 ? 'pos' : 'neg'),
      card('Wins / losses', `${st.wins} / ${st.losses}`),
      card('Biggest win', st.biggestWin.amount > 0 ? `+${fmt(st.biggestWin.amount)}` : '—'),
      card('Biggest multiplier', st.biggestMult.mult > 0 ? `${fmtMult(st.biggestMult.mult)}` : '—'),
      card('Current streak', st.streak === 0 ? '—' : st.streak > 0 ? `${st.streak} W` : `${-st.streak} L`, st.streak >= 0 ? 'pos' : 'neg'),
      card('Best win streak', String(st.bestWinStreak)),
      card('Worst loss streak', String(st.worstLossStreak)),
      card('Free refills used', String(st.refills)),
    ),
    games.length
      ? h('div.table-wrap', h('table.data',
          h('thead', h('tr', h('th', 'Game'), h('th', 'Bets'), h('th', 'Wagered'), h('th', 'Profit'), h('th.hide-sm', 'Best multi'))),
          h('tbody', games.map(([id, g]) => h('tr', h('td', gameName(id)), h('td', String(g.bets)), h('td', fmt(g.wagered)), h(`td.${g.won - g.wagered >= 0 ? 'pos' : 'neg'}`, fmt(g.won - g.wagered)), h('td.hide-sm', g.biggestMult ? fmtMult(g.biggestMult) : '—')))),
        ))
      : h('p.muted', 'Play a few rounds and your stats will show up here.'),
    h('p.muted.small', 'Stats are stored only in this browser (localStorage). Chips have no value.'),
  ], { wide: true });
}

export function openWelcome() {
  const m = openModal(`Welcome to ${APP_NAME}`, [
    h('div.welcome',
      h('div.welcome-chips', '10,000'),
      h('p', 'You\'ve got ', h('b', '10,000 free play chips'), '. Spin, drop, roll and climb across 13 neon games.'),
      h('ul.welcome-list',
        h('li', '💜 100% free, for fun — fake chips only.'),
        h('li', '🚫 No real money, no deposits, no withdrawals, no prizes.'),
        h('li', '🔄 Run out? Grab a free refill any time.'),
        h('li', '🧠 Nothing here is real gambling. If gambling ever stops being fun in real life, talk to someone and take a break.'),
      ),
      h('button.btn-play', { type: 'button', onclick: () => m.close() }, 'Let\'s play'),
    ),
  ], { onClose: () => { store.data.settings.welcomed = true; store.touch(); } });
}

export function openBreakReminder(minutes: number, onContinue: () => void) {
  const m = openModal('Time for a breather?', [
    h('div.break',
      h('div.break-icon', '☕'),
      h('p', `You've been playing for about ${minutes} minutes. Stretch, grab some water, look out a window — the chips will wait for you.`),
      h('p.muted.small', 'Reminder: this is a free play-money game. Nothing here is real gambling and chips can\'t be won or cashed out.'),
      h('div.break-actions',
        h('button.btn-secondary', { type: 'button', onclick: () => { m.close(); takeBreak(); } }, 'Take a 5 min break'),
        h('button.btn-play', { type: 'button', onclick: () => m.close() }, 'Keep playing'),
      ),
    ),
  ], { onClose: onContinue });
}

function takeBreak() {
  let left = 300;
  const timer = h('div.break-timer', '5:00');
  const m = openModal('Break time', [h('div.break', h('div.break-icon.breathe', '🌙'), h('p', 'Breathe in… and out. See you in a few.'), timer, h('button.btn-secondary', { type: 'button', onclick: () => m.close() }, 'I\'m back'))], { onClose: () => clearInterval(iv) });
  const iv = setInterval(() => {
    left--;
    timer.textContent = `${Math.floor(left / 60)}:${String(left % 60).padStart(2, '0')}`;
    if (left <= 0) m.close();
  }, 1000);
}
