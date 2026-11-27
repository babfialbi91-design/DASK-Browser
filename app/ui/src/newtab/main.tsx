import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import { send, subscribe, type AppState, type QuickLink } from '../common/bridge'

const DEFAULT_LINKS: QuickLink[] = [
  { title: 'ويكيبيديا', url: 'https://ar.wikipedia.org' },
  { title: 'GitHub', url: 'https://github.com' },
  { title: 'MDN', url: 'https://developer.mozilla.org' },
  { title: 'YouTube', url: 'https://youtube.com' },
]

const ENGINES = [
  { id: 'google', label: 'Google' },
  { id: 'bing', label: 'Bing' },
  { id: 'duckduckgo', label: 'DuckDuckGo' },
  { id: 'custom', label: 'مخصص' },
]

const faviconLetter = (url: string) => {
  try {
    const host = new URL(url).hostname.replace(/^www\./, '')
    return host.charAt(0).toUpperCase()
  } catch {
    return '?'
  }
}

// Real site favicon (Chromium's own NTP uses the same Google service).
// Falls back to the letter tile when the icon can't load (offline sites).
const faviconUrl = (url: string) => {
  try {
    const host = new URL(url).hostname
    // DuckDuckGo icon service: single origin, no redirect (better than
    // Google's s2 which 301s to t3.gstatic.com and trips CSP).
    return `https://icons.duckduckgo.com/ip3/${host}.ico`
  } catch {
    return ''
  }
}

// Deterministic gradient background per site, from the domain hash.
const GRADIENTS = [
  ['#7c3aed', '#4f46e5'],
  ['#e11d48', '#be123c'],
  ['#0ea5e9', '#0369a1'],
  ['#f59e0b', '#d97706'],
  ['#10b981', '#047857'],
  ['#8b5cf6', '#7c3aed'],
  ['#ef4444', '#b91c1c'],
  ['#14b8a6', '#0f766e'],
]
const tileGradient = (url: string) => {
  try {
    const host = new URL(url).hostname
    let h = 0
    for (let i = 0; i < host.length; i++) h = (h * 31 + host.charCodeAt(i)) >>> 0
    const [a, b] = GRADIENTS[h % GRADIENTS.length]
    return `linear-gradient(135deg, ${a}55, ${b}55)`
  } catch {
    return 'none'
  }
}

function App() {
  // Phase 7: follow the app theme (dark default) on <html data-theme>.
  useEffect(() => {
    return subscribe((s2) => {
      document.documentElement.setAttribute(
        'data-theme',
        s2.lightTheme ? 'light' : 'dark',
      )
    })
  }, [])
  const [state, setState] = useState<AppState | null>(null)
  const [query, setQuery] = useState('')
  const [editing, setEditing] = useState(false)
  const [showSettings, setShowSettings] = useState(false)
  const [draftLinks, setDraftLinks] = useState<QuickLink[]>([])

  useEffect(
    () =>
      subscribe((s) => {
        setState(s)
        setDraftLinks(s.quickLinks.length ? s.quickLinks : [...DEFAULT_LINKS])
      }),
    [],
  )

  const go = (req: Parameters<typeof send>[0]) => {
    void send(req).catch((err) => console.error(err))
  }

  const navigateSelf = (url: string) => {
    if (url.trim()) go({ type: 'navigate', url })
  }

  if (!state) return <div className="newtab-root" />

  const links = state.quickLinks.length ? state.quickLinks : DEFAULT_LINKS

  const saveLinks = (updated: QuickLink[]) => {
    setDraftLinks(updated)
    go({ type: 'setQuickLinks', links: updated })
  }

  return (
    <div className="newtab-root">
      <div className="wordmark">DASK</div>

      <div className="search-wrap">
        <input
          id="nt-search"
          type="text"
          autoComplete="off"
          spellCheck={false}
          placeholder="ابحث في الويب أو أدخل عنواناً…"
          aria-label="بحث"
          autoFocus
          value={query}
          onChange={(e) => setQuery(e.target.value)}
          onKeyDown={(e) => {
            if (e.key === 'Enter' && query.trim()) {
              navigateSelf(query.trim())
              setQuery('')
            }
          }}
        />
      </div>

      <div className="links-grid">
        {links.map((link, i) => (
          <button
            key={i}
            className="link-tile"
            style={{ background: tileGradient(link.url) }}
            title={link.url}
            onClick={() => navigateSelf(link.url)}
          >
            <span className="link-favicon">
              {faviconUrl(link.url) && (
                <img
                  src={faviconUrl(link.url)}
                  alt=""
                  width={22}
                  height={22}
                  loading="lazy"
                  onError={(e) => {
                    // Offline/blocked icon: fall back to the letter tile.
                    const img = e.currentTarget
                    img.style.display = 'none'
                    const span = img.parentElement
                    if (span && !span.dataset.fallback) {
                      span.dataset.fallback = '1'
                      span.textContent = faviconLetter(link.url)
                    }
                  }}
                />
              )}
              {!faviconUrl(link.url) && faviconLetter(link.url)}
            </span>
            <span className="link-title">{link.title}</span>
          </button>
        ))}
        {/* Add-site tile (Phase 7.5): opens the inline editor with a new row. */}
        <button
          className="link-tile add-tile"
          title="إضافة موقع"
          aria-label="إضافة موقع"
          onClick={() => {
            setDraftLinks([...links, { title: '', url: 'https://' }])
            setEditing(true)
            setShowSettings(false)
          }}
        >
          <span className="link-favicon add-favicon">
            <svg viewBox="0 0 24 24" width="20" height="20" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
              <line x1="12" y1="5" x2="12" y2="19" />
              <line x1="5" y1="12" x2="19" y2="12" />
            </svg>
          </span>
          <span className="link-title">إضافة</span>
        </button>
      </div>

      <div className="nt-row">
        <button
          className={`nt-btn${editing ? ' on' : ''}`}
          aria-expanded={editing}
          onClick={() => {
            setDraftLinks(links)
            setEditing(!editing)
            setShowSettings(false)
          }}
        >
          تعديل الروابط
        </button>
        <button
          className={`nt-btn${showSettings ? ' on' : ''}`}
          aria-expanded={showSettings}
          onClick={() => {
            if (showSettings) {
              setShowSettings(false)
            } else {
              // Inline section: search engine + shortcuts to full settings.
              setShowSettings(true)
              setEditing(false)
            }
          }}
        >
          الإعدادات
        </button>
        <button
          className="nt-btn"
          onClick={() =>
            go({ type: 'createTab', url: 'http://dask.ui/settings.html' })
          }
        >
          الخصوصية
        </button>
        <button
          className="nt-btn"
          title="Ctrl+Shift+N"
          onClick={() => go({ type: 'createTab', private: true })}
        >
          تبويب خاص
        </button>
      </div>

      {editing && (
        <div className="editor">
          <h3>روابط سريعة (تُحفظ تلقائياً)</h3>
          {draftLinks.map((link, i) => (
            <div className="link-edit-row" key={i}>
              <input
                className="title-input"
                value={link.title}
                aria-label="اسم الرابط"
                onChange={(e) => {
                  const next = [...draftLinks]
                  next[i] = { ...next[i], title: e.target.value }
                  setDraftLinks(next)
                }}
                onBlur={() => saveLinks(draftLinks)}
              />
              <input
                value={link.url}
                aria-label="عنوان الرابط"
                onChange={(e) => {
                  const next = [...draftLinks]
                  next[i] = { ...next[i], url: e.target.value }
                  setDraftLinks(next)
                }}
                onBlur={() => saveLinks(draftLinks)}
              />
              <button
                className="link-edit-del"
                title="حذف"
                aria-label="حذف الرابط"
                onClick={() => {
                  saveLinks(draftLinks.filter((_, j) => j !== i))
                }}
              >
                ✕
              </button>
            </div>
          ))}
          <div className="nt-row">
            <button className="nt-btn" onClick={() => saveLinks([...draftLinks, { title: 'جديد', url: 'https://' }])}>
              + إضافة رابط
            </button>
          </div>
        </div>
      )}

      {showSettings && (
        <div className="settings">
          <h3>محرك البحث الافتراضي</h3>
          {ENGINES.map((engine) => (
            <label key={engine.id}>
              <input
                type="radio"
                name="engine"
                checked={state.searchEngine.id === engine.id}
                onChange={() =>
                  go({
                    type: 'setSearchEngine',
                    id: engine.id,
                    custom: state.searchEngine.custom,
                  })
                }
              />
              {engine.label}
            </label>
          ))}
          {state.searchEngine.id === 'custom' && (
            <>
              <input
                className="custom-engine"
                type="text"
                placeholder="https://example.com/search?q=%s"
                aria-label="رابط محرك البحث المخصص"
                defaultValue={state.searchEngine.custom}
                onBlur={(e) =>
                  go({
                    type: 'setSearchEngine',
                    id: 'custom',
                    custom: e.target.value.trim(),
                  })
                }
              />
              <div className="hint">استخدم %s مكان نص البحث في الرابط.</div>
            </>
          )}
        </div>
      )}
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<App />)
