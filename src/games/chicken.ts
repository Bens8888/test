import type { GameMeta } from './registry';
import { gameShell } from '../ui/gameShell';
import { h } from '../engine/util';

export default function mount(root: HTMLElement, meta: GameMeta) {
  const shell = gameShell(root, meta, { rules: ['Coming soon.'] });
  shell.stage.appendChild(h('div.coming-soon', 'Coming soon'));
  return () => shell.destroy();
}
