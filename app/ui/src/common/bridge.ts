// Typed bridge between the React UI and the native host (C++) through
// CEF's message router (window.cefQuery / window.cefQueryCancel).
// Only these requests exist - nothing else reaches native code.

export interface TabState {
  id: number
  title: string
  url: string
  loading: boolean
  internal: boolean
  private: boolean
}

export interface BookmarkUi {
  id: number
  title: string
  url: string
}

export interface DownloadUi {
  id: number
  filename: string
  url: string
  state: number // 0 active, 1 complete, 2 canceled, 3 failed
  received: number
  total: number // -1 = unknown
  risky?: boolean // executable/script type — opening requires confirmation
}

export interface ProfileUi {
  id: string
  name: string
  active: boolean
}

export interface PermissionUi {
  host: string
  perm: string // camera / microphone / geolocation / notifications / ...
  decision: number // 0 ask, 1 allow, 2 deny
}

export interface PermissionPromptUi {
  id: number
  origin: string
  types: string[]
  media: boolean
}

export interface HistoryItemUi {
  id: number
  url: string
  title: string
  visitedAt: number
}

export interface QuickLink {
  title: string
  url: string
}

export interface SearchEngine {
  id: string // 'google' | 'bing' | 'duckduckgo' | 'custom'
  custom: string // format string with %s, used when id === 'custom'
}

export interface SiteException {
  host: string
  allowed: boolean
}

export interface FilterListStatus {
  name: string
  url: string
  rules: number
  updated: string
  status: string // 'ok' | 'loading' | 'error: ...' | 'not loaded'
}

export interface PrivacyState {
  enabled: boolean
  thirdPartyCookies: boolean
  siteExceptions: SiteException[]
  lists: FilterListStatus[]
  updating: boolean
  blocked: { total: number }
  tabBlockedCounts: Record<string, number>
  siteBlockedCounts: Record<string, number>
  activeSite: string
  activeSiteAllowed: boolean
}

export interface AppState {
  tabs: TabState[]
  activeTabId: number
  searchEngine: SearchEngine
  quickLinks: QuickLink[]
  focusAddress?: boolean
  privacy?: PrivacyState
  bookmarks: BookmarkUi[]
  downloads: DownloadUi[]
  restoreSession: boolean
  historyEnabled: boolean
  retentionDays: number
  dbNote?: string
  profiles: ProfileUi[]
  permissions: PermissionUi[]
  permissionPrompt?: PermissionPromptUi
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
  | { type: 'createTab'; url?: string; private?: boolean }
  | { type: 'closeTab'; tabId: number }
  | { type: 'activateTab'; tabId: number }
  | { type: 'setQuickLinks'; links: QuickLink[] }
  | { type: 'setSearchEngine'; id: string; custom: string }
  // ---- Privacy / ad blocking (Phase 3) ----
  | { type: 'setBlockingEnabled'; enabled: boolean }
  | { type: 'setThirdPartyCookies'; enabled: boolean }
  | { type: 'setSiteAllowed'; host: string; allowed: boolean }
  | { type: 'removeSiteException'; host: string }
  | { type: 'clearSiteData'; host: string }
  | { type: 'clearBrowsingData' }
  | { type: 'updateFilterLists' }
  // ---- Storage (Phase 4) ----
  | { type: 'toggleBookmark'; title: string; url: string }
  | { type: 'removeBookmark'; id: number }
  | { type: 'updateBookmark'; id: number; title: string; url: string }
  | { type: 'searchHistory'; query: string; limit: number }
  | { type: 'clearHistory'; days: number }
  | { type: 'setHistoryEnabled'; enabled: boolean }
  | { type: 'setRetentionDays'; days: number }
  | { type: 'setRestoreSession'; enabled: boolean }
  | { type: 'cancelDownload'; id: number }
  | { type: 'removeDownload'; id: number }
  | { type: 'openDownload'; id: number }
  | { type: 'openDownloadsFolder' }
  // ---- Permissions & profiles (Phase 5) ----
  | { type: 'respondPermission'; id: number; action: 'allowOnce' | 'allow' | 'denyOnce' | 'deny' }
  | { type: 'setPermission'; host: string; perm: string; decision: 0 | 1 | 2 }
  | { type: 'resetPermission'; host: string; perm: string }
  | { type: 'addProfile'; name: string }
  | { type: 'switchProfile'; id: string }
  | { type: 'deleteProfile'; id: string }

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
export function sendRaw(req: Request): Promise<string> {
  if (!hasBridge()) {
    return Promise.reject(new Error('bridge unavailable (must run inside DASK Browser)'))
  }
  return new Promise((resolve, reject) => {
    window.cefQuery!({
      request: JSON.stringify(req),
      persistent: false,
      onSuccess: (response) => resolve(response),
      onFailure: (code, message) => reject(new Error(`bridge error ${code}: ${message}`)),
    })
  })
}

export async function sendTyped<T>(req: Request): Promise<T> {
  const raw = await sendRaw(req)
  return JSON.parse(raw || '{}') as T
}

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
  const parsed = (value ?? {}) as Partial<AppState & { bookmarks?: BookmarkUi[]; downloads?: DownloadUi[]; restoreSession?: boolean; historyEnabled?: boolean; retentionDays?: number; dbNote?: string }>
  const privacy = parsed.privacy
    ? {
        enabled: !!parsed.privacy.enabled,
        thirdPartyCookies: !!parsed.privacy.thirdPartyCookies,
        siteExceptions: parsed.privacy.siteExceptions ?? [],
        lists: parsed.privacy.lists ?? [],
        updating: !!parsed.privacy.updating,
        blocked: parsed.privacy.blocked ?? { total: 0 },
        tabBlockedCounts: parsed.privacy.tabBlockedCounts ?? {},
        siteBlockedCounts: parsed.privacy.siteBlockedCounts ?? {},
        activeSite: parsed.privacy.activeSite ?? '',
        activeSiteAllowed: !!parsed.privacy.activeSiteAllowed,
      }
    : undefined
  return {
    tabs: parsed.tabs ?? [],
    activeTabId: parsed.activeTabId ?? 0,
    searchEngine: parsed.searchEngine ?? { id: 'google', custom: '' },
    quickLinks: parsed.quickLinks ?? [],
    focusAddress: parsed.focusAddress,
    privacy,
    bookmarks: parsed.bookmarks ?? [],
    downloads: parsed.downloads ?? [],
    restoreSession: parsed.restoreSession ?? true,
    historyEnabled: parsed.historyEnabled ?? true,
    retentionDays: parsed.retentionDays ?? 90,
    dbNote: parsed.dbNote,
    profiles: parsed.profiles ?? [],
    permissions: parsed.permissions ?? [],
    permissionPrompt: parsed.permissionPrompt,
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
