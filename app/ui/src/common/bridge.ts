// Typed bridge between the React UI and the native host (C++) through
// CEF's message router (window.cefQuery / window.cefQueryCancel).
// Only these requests exist - nothing else reaches native code.

export interface TabState {
  id: number
  title: string
  url: string
  loading: boolean
  internal: boolean
}

export interface QuickLink {
  title: string
  url: string
}

export interface SearchEngine {
  id: string // 'google' | 'bing' | 'duckduckgo' | 'custom'
  custom: string // format string with %s, used when id === 'custom'
}

export interface AppState {
  tabs: TabState[]
  activeTabId: number
  searchEngine: SearchEngine
  quickLinks: QuickLink[]
  focusAddress?: boolean
}

// Requests the UI is allowed to send. Tab-scoped actions without an explicit
// tabId are executed by the host against the context that sent them.
export type Request =
  | { type: 'getState' }
  | { type: 'wait' }
  | { type: 'navigate'; url: string }
  | { type: 'back' }
  | { type: 'forward' }
  | { type: 'reload' }
  | { type: 'stop' }
  | { type: 'home' }
  | { type: 'createTab'; url?: string }
  | { type: 'closeTab'; tabId: number }
  | { type: 'activateTab'; tabId: number }
  | { type: 'setQuickLinks'; links: QuickLink[] }
  | { type: 'setSearchEngine'; id: string; custom: string }

interface CefQueryRequest {
  request: string
  persistent?: boolean
  onSuccess: (response: string) => void
  onFailure: (code: number, message: string) => void
}

declare global {
  interface Window {
    cefQuery?: (req: CefQueryRequest) => number
    cefQueryCancel?: (id: number) => void
  }
}

export function hasBridge(): boolean {
  return typeof window !== 'undefined' && typeof window.cefQuery === 'function'
}

/** Send one request; resolves with the parsed JSON response. */
export function send(req: Request): Promise<unknown> {
  if (!hasBridge()) {
    return Promise.reject(new Error('bridge unavailable (must run inside DASK Browser)'))
  }
  return new Promise((resolve, reject) => {
    window.cefQuery!({
      request: JSON.stringify(req),
      persistent: false,
      onSuccess: (response) => {
        try {
          resolve(response ? JSON.parse(response) : null)
        } catch (err) {
          reject(err)
        }
      },
      onFailure: (code, message) => reject(new Error(`bridge error ${code}: ${message}`)),
    })
  })
}

function coerceState(value: unknown): AppState {
  const parsed = (value ?? {}) as Partial<AppState>
  return {
    tabs: parsed.tabs ?? [],
    activeTabId: parsed.activeTabId ?? 0,
    searchEngine: parsed.searchEngine ?? { id: 'google', custom: '' },
    quickLinks: parsed.quickLinks ?? [],
    focusAddress: parsed.focusAddress,
  }
}

/**
 * Subscribe to host state events.
 *
 * Loop per page: getState (immediate snapshot) -> wait (persistent query that
 * the host answers on the next event) -> getState -> ... Each page owns its
 * own wait query, so several pages stay in sync without racing on a shared
 * slot.
 */
export function subscribe(onEvent: (state: AppState) => void): () => void {
  let stopped = false
  let timer: number | undefined

  const waitOnce = (): Promise<AppState> =>
    new Promise((resolve, reject) => {
      if (!hasBridge()) return reject(new Error('bridge unavailable'))
      window.cefQuery!({
        request: JSON.stringify({ type: 'wait' }),
        persistent: true,
        onSuccess: (response) => {
          try {
            resolve(coerceState(JSON.parse(response || '{}')))
          } catch (err) {
            reject(err)
          }
        },
        onFailure: (code, message) => reject(new Error(`wait ${code}: ${message}`)),
      })
    })

  const loop = async () => {
    while (!stopped) {
      try {
        onEvent(coerceState(await send({ type: 'getState' })))
        if (stopped) return
        onEvent(await waitOnce())
      } catch (err) {
        if (stopped) return
        console.error('bridge loop error', err)
        timer = window.setTimeout(loop, 300) // retry after a hiccup
        return
      }
    }
  }

  loop()
  return () => {
    stopped = true
    if (timer !== undefined) window.clearTimeout(timer)
  }
}
