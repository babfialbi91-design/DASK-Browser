// Phase 7: entry point of the popup menu page (menu.html inside the native
// popup window). Renders the menu permanently open; the window itself
// closes on outside click (WM_ACTIVATE) or Escape.
import { useEffect, useRef, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import { applyAccent } from '../common/accent'
import './menu.css'
import { send, subscribe, type AppState } from '../common/bridge'
import { buildMenuRows } from './menuItems'
import { menuIcons } from './menuIcons'
import type { MenuItem, MenuRow } from './menuModel'

// Icon pack artwork (DASK_Browser_Icon_Pack) for the items that map to it:
// incognito -> private window, app logo -> about, private shield -> tab strip.
import incognitoUrl from '../../public/icons/incognito.png'
import appUrl from '../../public/icons/app.png'

// Menu item icon overrides from the icon pack, matched by label.
const packIcons: Record<string, string> = {
  'نافذة خاصة': incognitoUrl,
  'حول DASK Browser': appUrl,
}

function MenuItemRow({
  item,
  close,
  onSubmenu,
  onHoverOut,
}: {
  item: MenuItem
  close: () => void
  onSubmenu: (items: MenuRow[], top: number) => void
  onHoverOut?: () => void
}) {
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
        className="menu-item has-sub"
        onMouseEnter={(e) =>
          onSubmenu(item.items!, (e.currentTarget as HTMLElement).offsetTop)
        }
        onMouseLeave={() => onHoverOut?.()}
        role="menuitem"
        aria-haspopup="true"
      >
        <span className="menu-icon">{icon}</span>
        <span className="menu-label">{item.label}</span>
        <span className="menu-arrow"><menuIcons.chevronRight size={14} /></span>
      </div>
    )
  }

  return (
    <button
      className="menu-item"
      role="menuitem"
      onMouseEnter={() => onHoverOut?.()}
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
  // Phase 7.5: Chrome-style hover flyout. The popup window expands to the
  // left (resizeMenuPopup) while a submenu is open, and the panel renders
  // inside the expanded area next to the hovered row — never clipped.
  const [sub, setSub] = useState<{ title: string; items: MenuRow[]; top: number } | null>(null)
  const closeTimer = useRef<number | null>(null)
  useEffect(() => subscribe((s2) => { setState(s2); applyAccent(s2) }), [])
  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') void send({ type: 'closeAppMenu' }).catch(() => {})
    }
    window.addEventListener('keydown', onKey)
    return () => window.removeEventListener('keydown', onKey)
  }, [])

  const openSub = (title: string, items: MenuRow[], top: number) => {
    if (closeTimer.current) {
      window.clearTimeout(closeTimer.current)
      closeTimer.current = null
    }
    if (!sub) void send({ type: 'resizeMenuPopup', wide: true }).catch(() => {})
    setSub({ title, items, top })
  }
  const closeSub = () => {
    if (closeTimer.current) window.clearTimeout(closeTimer.current)
    closeTimer.current = window.setTimeout(() => {
      setSub(null)
      void send({ type: 'resizeMenuPopup', wide: false }).catch(() => {})
    }, 260)
  }
  const close = () => {
    void send({ type: 'closeAppMenu' }).catch(() => {})
  }

  // Report the measured flyout rect so the native side can clip the popup
  // window to exactly the menu column + this panel (nothing else shows).
  useEffect(() => {
    if (!sub) return
    const fly = document.querySelector('.menu-flyout') as HTMLElement | null
    if (!fly) return
    void send({
      type: 'resizeMenuPopup',
      wide: true,
      top: fly.offsetTop,
      height: fly.offsetHeight,
    }).catch(() => {})
  }, [sub])

  if (!state) return <div className="menu-page" />
  const rows = buildMenuRows({ state, go: send, close })

  return (
    <div className="menu-page" role="menu" aria-label="القائمة الرئيسية">
      {sub && (
        <div
          className="menu-flyout"
          role="menu"
          style={{ top: sub.top }}
          onMouseEnter={() => {
            if (closeTimer.current) {
              window.clearTimeout(closeTimer.current)
              closeTimer.current = null
            }
          }}
          onMouseLeave={closeSub}
        >
          {sub.items.map((item, i) => {
            if (item.kind === 'divider') return <div key={i} className="menu-divider" />
            if (item.kind === 'header') {
              return <div key={i} className="flyout-header">{item.label}</div>
            }
            const it = item as MenuItem
            const Icon = it.icon
            if (it.disabled) {
              return (
                <div
                  key={i}
                  className="menu-item disabled"
                  title={it.disabledNote}
                  aria-disabled="true"
                >
                  <span className="menu-icon"><Icon size={16} /></span>
                  <span className="menu-label">{it.label}</span>
                </div>
              )
            }
            return (
              <button
                key={i}
                className="menu-item"
                role="menuitem"
                onClick={() => {
                  it.run?.()
                  close()
                }}
              >
                <span className="menu-icon"><Icon size={16} /></span>
                <span className="menu-label">{it.label}</span>
                {it.shortcut && <span className="menu-shortcut">{it.shortcut}</span>}
              </button>
            )
          })}
        </div>
      )}
      {rows.map((row, i) => {
        if (row.kind === 'divider') return <div key={i} className="menu-divider" />
        if (row.kind === 'zoomRow') return <ZoomRow key={i} state={state} close={close} />
        if (row.kind !== 'item') return null
        return (
          <MenuItemRow
            key={i}
            item={row}
            close={close}
            onSubmenu={(items, top) => openSub(row.label, items, top)}
            onHoverOut={closeSub}
          />
        )
      })}
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<MenuPage />)
