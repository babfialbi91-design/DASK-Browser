// Phase 7: entry point of the popup menu page (menu.html inside the native
// popup window). Renders the menu permanently open; the window itself
// closes on outside click (WM_ACTIVATE) or Escape.
import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import './menu.css'
import { send, subscribe, type AppState } from '../common/bridge'
import { buildMenuRows } from './menuItems'
import { menuIcons } from './menuIcons'
import type { MenuItem } from './menuModel'

// Icon pack artwork (DASK_Browser_Icon_Pack) for the items that map to it:
// incognito -> private window, app logo -> about, private shield -> tab strip.
import incognitoUrl from '../../public/icons/incognito.png'
import appUrl from '../../public/icons/app.png'

// Menu item icon overrides from the icon pack, matched by label.
const packIcons: Record<string, string> = {
  'نافذة خاصة': incognitoUrl,
  'حول DASK Browser': appUrl,
}

function MenuItemRow({ item, close }: { item: MenuItem; close: () => void }) {
  const [submenuOpen, setSubmenuOpen] = useState(false)
  const packSrc = packIcons[item.label]
  const icon = packSrc ? (
    <img src={packSrc} width={17} height={17} alt="" />
  ) : (
    <item.icon size={16} />
  )

  if (item.disabled) {
    return (
      <div className="menu-item disabled" title={item.disabledNote} aria-disabled="true">
        <span className="menu-icon">{icon}</span>
        <span className="menu-label">{item.label}</span>
      </div>
    )
  }

  if (item.items) {
    return (
      <div
        className={'menu-item has-sub' + (submenuOpen ? ' open' : '')}
        onMouseEnter={() => setSubmenuOpen(true)}
        onMouseLeave={() => setSubmenuOpen(false)}
        onClick={() => setSubmenuOpen((v) => !v)}
        role="menuitem"
        aria-haspopup="true"
        aria-expanded={submenuOpen}
      >
        <span className="menu-icon">{icon}</span>
        <span className="menu-label">{item.label}</span>
        <span className="menu-arrow"><menuIcons.chevronRight size={14} /></span>
        {submenuOpen && (
          <div className="menu-submenu" role="menu">
            {item.items.map((sub, i) => (
              <MenuItemRow key={i} item={sub} close={close} />
            ))}
          </div>
        )}
      </div>
    )
  }

  return (
    <button
      className="menu-item"
      role="menuitem"
      onClick={() => {
        item.run?.()
        close()
      }}
    >
      <span className="menu-icon">{icon}</span>
      <span className="menu-label">{item.label}</span>
      {item.shortcut && <span className="menu-shortcut">{item.shortcut}</span>}
    </button>
  )
}

function ZoomRow({ state, close }: { state: AppState; close: () => void }) {
  const zoom = state.zoomPercent ?? 100
  const step = (delta: number) => {
    void send({ type: 'adjustZoom', step: delta }).catch(() => {})
  }
  return (
    <div className="menu-zoom" role="group" aria-label="التكبير">
      <button className="menu-zoom-btn" title="تصغير (Ctrl+-)" onClick={() => step(-0.35)} aria-label="تصغير">
        <menuIcons.minus size={15} />
      </button>
      <span className="menu-zoom-value">{zoom}%</span>
      <button className="menu-zoom-btn" title="تكبير (Ctrl++)" onClick={() => step(0.35)} aria-label="تكبير">
        <menuIcons.plus size={15} />
      </button>
      <button
        className="menu-zoom-btn"
        title={state.fullscreen ? 'إنهاء ملء الشاشة (F11)' : 'ملء الشاشة (F11)'}
        aria-label="ملء الشاشة"
        onClick={() => {
          void send({ type: 'toggleFullscreen' }).catch(() => {})
          close()
        }}
      >
        <menuIcons.fullscreen size={15} />
      </button>
    </div>
  )
}

function MenuPage() {
  const [state, setState] = useState<AppState | null>(null)
  useEffect(() => subscribe(setState), [])
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') void send({ type: 'closeAppMenu' }).catch(() => {})
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  const close = () => {
    void send({ type: 'closeAppMenu' }).catch(() => {})
  }

  if (!state) return <div className="menu-page" />
  const rows = buildMenuRows({ state, go: send, close })

  return (
    <div className="menu-page" role="menu" aria-label="القائمة الرئيسية">
      {rows.map((row, i) => {
        if (row.kind === 'divider') return <div key={i} className="menu-divider" />
        if (row.kind === 'zoomRow') return <ZoomRow key={i} state={state} close={close} />
        return <MenuItemRow key={i} item={row} close={close} />
      })}
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<MenuPage />)
