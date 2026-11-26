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

function App() {
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
            title={link.url}
            onClick={() => navigateSelf(link.url)}
          >
            <span className="link-favicon">{faviconLetter(link.url)}</span>
            <span className="link-title">{link.title}</span>
          </button>
        ))}
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
