import type { AppState } from './bridge'

// Phase 9.5: the user's accent color (first-run theme picker + settings).
// Applied as CSS custom properties on every DASK UI page.
const DEFAULT_ACCENT = '#8b5cf6'

const hexToRgb = (hex: string): [number, number, number] => {
  const clean = hex.replace('#', '')
  const full =
    clean.length === 3
      ? clean
          .split('')
          .map((c) => c + c)
          .join('')
      : clean
  const n = parseInt(full, 16)
  if (Number.isNaN(n) || full.length !== 6) return [139, 92, 246]
  return [(n >> 16) & 0xff, (n >> 8) & 0xff, n & 0xff]
}

export function accentOf(state: AppState | null): string {
  const a = state?.accent
  return a && a.startsWith('#') ? a : DEFAULT_ACCENT
}

export function applyAccent(state: AppState | null): void {
  const accent = accentOf(state)
  const [r, g, b] = hexToRgb(accent)
  const root = document.documentElement
  if (root.style.getPropertyValue('--accent') !== accent) {
    root.style.setProperty('--accent', accent)
    root.style.setProperty(
      '--accent-soft',
      `rgba(${r}, ${g}, ${b}, 0.18)`,
    )
  }
}
