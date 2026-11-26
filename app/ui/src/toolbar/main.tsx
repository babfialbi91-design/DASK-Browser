import { useCallback, useEffect, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import { send, subscribe, type AppState, type TabState } from '../common/bridge'

// Icons (inline SVG, 24x24 viewBox scaled by CSS).
const icons = {
  back: <path d="M15.5 4.5 8 12l7.5 7.5 1.4-1.4L10.8 12l6.1-6.1z" />,
  forward: <path d="M8.5 4.5 16 12l-7.5 7.5-1.4-1.4 6.1-6.1-6.1-6.1z" />,
  reload: (
    <path d="M17.7 6.3A8 8 0 1 0 19.7 13h-2.1A6 6 0 1 1 12 6a5.9 5.9 0 0 1 4.2 1.8L13 11h7V4l-2.3 2.3z" />
  ),
  stop: <path d="M6 6h12v12H6z" fillRule="evenodd" />,
  home: <path d="M12 3 3 11h2v9h6v-6h2v6h6v-9h2z" />,
  plus: <path d="M11 5h2v6h6v2h-6v6h-2v-6H5v-2h6z" />,
  globe: (
    <path d="M12 2a10 10 0 1 0 0 20 10 10 0 0 0 0-20zm7.9 9h-3.4a15.5 15.5 0 0 0-1-4.8A8 8 0 0 1 19.9 11zM12 4.2c.9 1.2 1.7 3.5 1.9 6.8h-3.8c.2-3.3 1-5.6 1.9-6.8zM4.1 13h3.4c.1 1.8.5 3.4 1 4.8A8 8 0 0 1 4.1 13zm3.4-2H4.1a8 8 0 0 1 4.4-4.8 15.5 15.5 0 0 0-1 4.8zm1.9 2h5.2c-.2 3.3-1 5.6-1.9 6.8-.9-1.2-1.7-3.5-1.9-6.8h-3.3zm7-2a15.5 15.5 0 0 0-1-4.8 8 8 0 0 1 4.4 4.8h-3.4zM9.5 17.8a15.5 15.5 0 0 1-1-4.8h3.4a15.5 15.5 0 0 1-1 4.8 8 8 0 0 1-1.4 0zm3.4 3.2z" />
  ),
  search: (
    <path d="M10 3a7 7 0 1 0 4.3 12.5l4.6 4.6 1.4-1.4-4.6-4.6A7 7 0 0 0 10 3zm0 2a5 5 0 1 1 0 10 5 5 0 0 1 0-10z" />
  ),
  shield: (
    <path d="M12 2 4 5v6c0 5 3.4 9.7 8 11 4.6-1.3 8-6 8-11V5l-8-3zm-1 13-3-3 1.4-1.4L11 12.2l4.6-4.6L17 9l-6 6z" />
  ),
  star: (
    <path d="M12 2.5l2.9 5.9 6.5.9-4.7 4.6 1.1 6.5-5.8-3-5.8 3 1.1-6.5L2.6 9.3l6.5-.9L12 2.5z" />
  ),
  starFilled: (
    <path d="M12 2.5l2.9 5.9 6.5.9-4.7 4.6 1.1 6.5-5.8-3-5.8 3 1.1-6.5L2.6 9.3l6.5-.9L12 2.5zm0 3.4L9.9 10.2l-4.4.6 3.2 3.1-.8 4.4L12 16.1l4.1 2.2-.8-4.4 3.2-3.1-4.4-.6L12 5.9z" fill="currentColor" fillRule="evenodd" />
  ),
  shieldOff: (
    <path d="M12 2 4 5v6c0 5 3.4 9.7 8 11 2-.6 3.7-1.8 5.1-3.4L4.4 5.9 5.8 4.5l1.2 1.2V5l5-1.9L16.2 5H21v7.7L14.6 6.3l1.4-1.4z" />
  ),
}

function NavButton(props: {
  title: string
  disabled?: boolean
  onClick: () => void
  children: React.ReactNode
}) {
  return (
    <button
      className="nav-btn"
      title={props.title}
      aria-label={props.title}
      disabled={props.disabled}
      onClick={props.onClick}
    >
      <svg viewBox="0 0 24 24">{props.children}</svg>
    </button>
  )
}

function ToolbarTab(props: { tab: TabState; active: boolean; onActivate: () => void; onClose: () => void }) {
  const title = props.tab.internal && !props.tab.title ? 'تبويب جديد' : props.tab.title || props.tab.url || '...'
  return (
    <div
      className={`tab${props.active ? ' active' : ''}`}
      role="tab"
      aria-selected={props.active}
      tabIndex={0}
      onClick={props.onActivate}
      onKeyDown={(e) => {
        if (e.key === 'Enter' || e.key === ' ') props.onActivate()
      }}
    >
      {props.tab.loading && (
        <span className="tab-spinner" aria-hidden="true" />
      )}
      <span className="tab-title">{title}</span>
      <button
        className="tab-close"
        title="إغلاق التبويب"
        aria-label="إغلاق التبويب"
        onClick={(e) => {
          e.stopPropagation()
          props.onClose()
        }}
      >
        ×
      </button>
    </div>
  )
}

function App() {
  const [state, setState] = useState<AppState | null>(null)
  const [address, setAddress] = useState('')
  const [addressFocused, setAddressFocused] = useState(false)
  const addressRef = useRef<HTMLInputElement>(null)

  useEffect(
    () =>
      subscribe((s) => {
        setState(s)
        if (s.focusAddress) {
          addressRef.current?.focus()
          addressRef.current?.select()
        }
      }),
    [],
  )

  const active = state?.tabs.find((t) => t.id === state.activeTabId)
  const blockedHere = state?.privacy
    ? state.privacy.tabBlockedCounts[String(state.activeTabId)] ?? 0
    : 0
  const bookmarked = !!active && !!state?.bookmarks.some((b) => b.url === active.url)
  const badgeText =
    blockedHere > 999
      ? '999+'
      : blockedHere > 99 && blockedHere < 1000
        ? '99+'
        : String(blockedHere)

  // Keep the address bar in sync with the active tab while the user is not
  // typing in it. Internal UI pages (new tab, settings) show a blank address
  // bar like real browsers do for chrome://newtab.
  useEffect(() => {
    if (addressFocused) return
    if (!active || active.internal) {
      setAddress('')
    } else {
      setAddress(active.url)
    }
  }, [active?.id, active?.url, active?.internal, addressFocused]) // eslint-disable-line react-hooks/exhaustive-deps

  const go = useCallback((req: Parameters<typeof send>[0]) => {
    void send(req).catch((err) => console.error(err))
  }, [])

  const submitAddress = () => {
    const text = address.trim()
    if (!text) return
    go({ type: 'navigate', url: text })
    addressRef.current?.blur()
  }

  if (!state) return <div className="toolbar-root" />

  const loading = !!active?.loading

  return (
    <div className="toolbar-root">
      <div className="tab-strip" role="tablist">
        {state.tabs.map((tab) => (
          <ToolbarTab
            key={tab.id}
            tab={tab}
            active={tab.id === state.activeTabId}
            onActivate={() => go({ type: 'activateTab', tabId: tab.id })}
            onClose={() => go({ type: 'closeTab', tabId: tab.id })}
          />
        ))}
        <button className="tab-new" title="تبويب جديد (Ctrl+T)" aria-label="تبويب جديد" onClick={() => go({ type: 'createTab' })}>
          <svg viewBox="0 0 24 24" width="14" height="14" fill="currentColor">
            {icons.plus}
          </svg>
        </button>
      </div>

      <div className="nav-bar">
        <NavButton title="رجوع (Alt+السهم الأيسر)" disabled={!active?.url || active.url.startsWith('dask://')} onClick={() => go({ type: 'back' })}>
          {icons.back}
        </NavButton>
        <NavButton title="تقدّم" onClick={() => go({ type: 'forward' })}>
          {icons.forward}
        </NavButton>
        {loading ? (
          <NavButton title="إيقاف" onClick={() => go({ type: 'stop' })}>
            {icons.stop}
          </NavButton>
        ) : (
          <NavButton title="تحديث (Ctrl+R)" onClick={() => go({ type: 'reload' })}>
            {icons.reload}
          </NavButton>
        )}
        <NavButton title="الرئيسية" onClick={() => go({ type: 'home' })}>
          {icons.home}
        </NavButton>

        {active && !active.internal && !active.private && (
          <button
            className={'nav-btn star' + (bookmarked ? ' on' : '')}
            title={bookmarked ? 'إزالة من المفضلة (Ctrl+D)' : 'إضافة إلى المفضلة (Ctrl+D)'}
            aria-label="إشارة مرجعية"
            onClick={() => go({ type: 'toggleBookmark', title: active.title || active.url, url: active.url })}
          >
            <svg viewBox="0 0 24 24">{bookmarked ? icons.starFilled : icons.star}</svg>
          </button>
        )}

        {state?.privacy && active && !active.internal && (
          <button
            className={`nav-btn shield${state.privacy.activeSiteAllowed ? ' off' : ''}`}
            title={
              state.privacy.activeSiteAllowed
                ? `الحجب معطّل لهذا الموقع (${state.privacy.activeSite}) — انقر للتفعيل`
                : `الحجب مفعّل (${state.privacy.activeSite}) — انقر للتعطيل لهذا الموقع`
            }
            aria-label="حجب الإعلانات لهذا الموقع"
            onClick={() =>
              go({
                type: 'setSiteAllowed',
                host: state.privacy!.activeSite,
                allowed: !state.privacy!.activeSiteAllowed,
              })
            }
          >
            <svg viewBox="0 0 24 24">
              {state.privacy.activeSiteAllowed ? icons.shieldOff : icons.shield}
            </svg>
            {blockedHere > 0 && <span className="shield-badge">{badgeText}</span>}
          </button>
        )}

        <div className={`address-box${addressFocused ? ' focused' : ''}`}>
          <svg className="address-icon" viewBox="0 0 24 24" aria-hidden="true">
            {active?.internal ? icons.search : icons.globe}
          </svg>
          <input
            ref={addressRef}
            id="address"
            type="text"
            autoComplete="off"
            spellCheck={false}
            placeholder="ابحث أو أدخل عنواناً"
            aria-label="شريط العنوان"
            value={address}
            onChange={(e) => setAddress(e.target.value)}
            onFocus={(e) => {
              setAddressFocused(true)
              e.currentTarget.select()
            }}
            onBlur={() => setAddressFocused(false)}
            onKeyDown={(e) => {
              if (e.key === 'Enter') submitAddress()
              if (e.key === 'Escape') e.currentTarget.blur()
            }}
          />
        </div>

        {loading && <div className="loading-bar" aria-hidden="true" />}
      </div>

      {state?.permissionPrompt ? (
        (() => {
          const prompt = state.permissionPrompt
          return (
        <div className="perm-bubble" role="dialog" aria-label="طلب إذن">
          <div className="perm-text">
            يطلب <strong>{hostOf(prompt.origin)}</strong>{' '}
            {prompt.types.join(' و')}:
          </div>
          <div className="perm-actions">
            <button
              className="perm-btn allow"
              onClick={() => go({ type: 'respondPermission', id: prompt.id, action: 'allow' })}
            >
              سماح دائماً
            </button>
            <button
              className="perm-btn"
              onClick={() => go({ type: 'respondPermission', id: prompt.id, action: 'allowOnce' })}
            >
              سماح مرة واحدة
            </button>
            <button
              className="perm-btn"
              onClick={() => go({ type: 'respondPermission', id: prompt.id, action: 'denyOnce' })}
            >
              رفض
            </button>
            <button
              className="perm-btn deny"
              onClick={() => go({ type: 'respondPermission', id: prompt.id, action: 'deny' })}
            >
              رفض دائماً
            </button>
          </div>
        </div>
          )
        })()
      ) : null}
    </div>
  )
}

function hostOf(origin: string): string {
  try {
    const url = new URL(origin)
    return url.host || origin
  } catch {
    return origin
  }
}

createRoot(document.getElementById('root')!).render(<App />)
