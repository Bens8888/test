import { sound } from '../engine/sound';
import { h } from '../engine/util';

export interface ModalHandle {
  el: HTMLElement;
  body: HTMLElement;
  close: () => void;
}

export function openModal(title: string, content: Node | Node[], opts: { wide?: boolean; onClose?: () => void; dismissable?: boolean; className?: string } = {}): ModalHandle {
  const body = h('div.modal-body', content as any);
  const closeBtn = h('button.icon-btn.modal-close', { type: 'button', 'aria-label': 'Close' }, '✕');
  const box = h(`div.modal.glass${opts.wide ? '.wide' : ''}${opts.className ? '.' + opts.className : ''}`, { role: 'dialog', 'aria-modal': 'true' },
    h('div.modal-head', h('h2', title), opts.dismissable === false ? null : closeBtn),
    body,
  );
  const backdrop = h('div.modal-backdrop', box);
  let closed = false;
  const close = () => {
    if (closed) return;
    closed = true;
    backdrop.classList.add('out');
    window.removeEventListener('keydown', onKey);
    setTimeout(() => backdrop.remove(), 200);
    opts.onClose?.();
  };
  const onKey = (e: KeyboardEvent) => {
    if (e.key === 'Escape' && opts.dismissable !== false) close();
  };
  closeBtn.addEventListener('click', () => {
    sound.play('click');
    close();
  });
  backdrop.addEventListener('pointerdown', (e) => {
    if (e.target === backdrop && opts.dismissable !== false) close();
  });
  window.addEventListener('keydown', onKey);
  document.body.appendChild(backdrop);
  return { el: box, body, close };
}
