/** Small DOM + math helpers shared by every screen. */

type Attrs = Record<string, any> & { class?: string; style?: string | Partial<CSSStyleDeclaration> };
type Child = Node | string | number | null | undefined | false | Child[];

/** Hyperscript-style element factory: h('div.card#id', {onclick}, children) */
export function h<K extends keyof HTMLElementTagNameMap>(tag: K | string, attrs?: Attrs | Child, ...children: Child[]): HTMLElement {
  let tagName = tag as string;
  const classes: string[] = [];
  let id = '';
  const m = tagName.match(/^[a-z0-9-]+/i);
  const rest = tagName.slice(m ? m[0].length : 0);
  tagName = m ? m[0] : 'div';
  rest.replace(/([.#])([\w-]+)/g, (_, t, v) => {
    if (t === '.') classes.push(v);
    else id = v;
    return '';
  });
  const el = document.createElement(tagName);
  if (classes.length) el.className = classes.join(' ');
  if (id) el.id = id;
  if (attrs && (typeof attrs !== 'object' || attrs instanceof Node || Array.isArray(attrs))) {
    children.unshift(attrs as Child);
    attrs = undefined;
  }
  if (attrs) {
    for (const [k, v] of Object.entries(attrs as Attrs)) {
      if (v === undefined || v === null || v === false) continue;
      if (k === 'class') el.className = [el.className, v].filter(Boolean).join(' ');
      else if (k === 'style' && typeof v === 'object') {
        for (const [sk, sv] of Object.entries(v)) {
          if (sk.startsWith('--')) el.style.setProperty(sk, String(sv));
          else (el.style as any)[sk] = sv;
        }
      }
      else if (k.startsWith('on') && typeof v === 'function') el.addEventListener(k.slice(2).toLowerCase(), v);
      else if (k === 'html') el.innerHTML = v;
      else if (k in el && k !== 'list' && k !== 'type' && k !== 'form') (el as any)[k] = v;
      else el.setAttribute(k, v === true ? '' : String(v));
    }
  }
  appendChildren(el, children);
  return el;
}

function appendChildren(el: Node, children: Child[]) {
  for (const c of children) {
    if (c === null || c === undefined || c === false) continue;
    if (Array.isArray(c)) appendChildren(el, c);
    else el.appendChild(c instanceof Node ? c : document.createTextNode(String(c)));
  }
}

export function svg(markup: string): HTMLElement {
  const t = document.createElement('span');
  t.className = 'svg-wrap';
  t.innerHTML = markup;
  return t;
}

export const clamp = (v: number, lo: number, hi: number) => Math.min(hi, Math.max(lo, v));
export const lerp = (a: number, b: number, t: number) => a + (b - a) * t;
export const round2 = (v: number) => Math.round((v + Number.EPSILON) * 100) / 100;
export const floor2 = (v: number) => Math.floor((v + 1e-9) * 100) / 100;
export const sleep = (ms: number) => new Promise<void>((r) => setTimeout(r, ms));
export const rand = (a: number, b: number) => a + Math.random() * (b - a);
export const randInt = (a: number, b: number) => Math.floor(rand(a, b + 1));
export const pick = <T>(arr: readonly T[]): T => arr[Math.floor(Math.random() * arr.length)];

export const easeOutCubic = (t: number) => 1 - Math.pow(1 - t, 3);
export const easeInOutCubic = (t: number) => (t < 0.5 ? 4 * t * t * t : 1 - Math.pow(-2 * t + 2, 3) / 2);
export const easeOutBack = (t: number) => {
  const c1 = 1.70158, c3 = c1 + 1;
  return 1 + c3 * Math.pow(t - 1, 3) + c1 * Math.pow(t - 1, 2);
};
export const easeOutQuart = (t: number) => 1 - Math.pow(1 - t, 4);

/** 12,345.67 */
export function fmt(v: number, decimals = 2): string {
  return v.toLocaleString('en-US', { minimumFractionDigits: decimals, maximumFractionDigits: decimals });
}
/** 1.2K / 3.4M */
export function fmtShort(v: number): string {
  const a = Math.abs(v);
  if (a >= 1e6) return (v / 1e6).toFixed(a >= 1e7 ? 0 : 1).replace(/\.0$/, '') + 'M';
  if (a >= 1e3) return (v / 1e3).toFixed(a >= 1e4 ? 0 : 1).replace(/\.0$/, '') + 'K';
  return Number.isInteger(v) ? String(v) : v.toFixed(2);
}
export function fmtMult(m: number): string {
  if (m >= 1000) return fmt(m, 0) + '×';
  return m.toFixed(2) + '×';
}

export function hexToBytes(hex: string): Uint8Array {
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}
export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, '0')).join('');
}

export function prefersReducedMotion(): boolean {
  return typeof matchMedia !== 'undefined' && matchMedia('(prefers-reduced-motion: reduce)').matches;
}

/** requestAnimationFrame loop helper; returns stop function. dt in seconds (capped). */
export function rafLoop(fn: (dt: number, now: number) => void | false): () => void {
  let last = performance.now();
  let id = 0;
  let stopped = false;
  const tick = (now: number) => {
    if (stopped) return;
    const dt = Math.max(0, Math.min(0.05, (now - last) / 1000));
    last = now;
    if (fn(dt, now) === false) return;
    id = requestAnimationFrame(tick);
  };
  id = requestAnimationFrame(tick);
  return () => {
    stopped = true;
    cancelAnimationFrame(id);
  };
}

/** Sets up a HiDPI canvas that tracks its container size. */
export function fitCanvas(canvas: HTMLCanvasElement, onResize?: (w: number, h: number) => void): () => void {
  const ctx = canvas.getContext('2d')!;
  const apply = () => {
    // client size ignores CSS transforms (e.g. the tilted roulette wheel)
    const dpr = Math.min(window.devicePixelRatio || 1, 2);
    const w = Math.max(1, Math.round(canvas.clientWidth));
    const hgt = Math.max(1, Math.round(canvas.clientHeight));
    canvas.width = w * dpr;
    canvas.height = hgt * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    onResize?.(w, hgt);
  };
  const ro = new ResizeObserver(apply);
  ro.observe(canvas);
  apply();
  return () => ro.disconnect();
}

export class Emitter<T> {
  private fns = new Set<(v: T) => void>();
  on(fn: (v: T) => void): () => void {
    this.fns.add(fn);
    return () => this.fns.delete(fn);
  }
  emit(v: T) {
    this.fns.forEach((f) => f(v));
  }
}
