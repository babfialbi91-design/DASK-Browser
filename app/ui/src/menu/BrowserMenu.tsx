// Phase 7: the main browser menu (hamburger) — rendering component.
// Split from menuItems.tsx (definitions) and menuIcons.tsx (icons) on
// purpose: this file only renders and handles interaction.
import { useEffect, useRef, useState } from 'react'
import type { AppState } from '../common/bridge'
import { send } from '../common/bridge'
import { buildMenuRows } from './menuItems'
import { menuIcons } from './menuIcons'
import type { MenuItem } from './menuModel'

function MenuItemRow({ item, onDone }: { item: MenuItem; onDone: () => void }) {
  const [submenuOpen, setSubmenuOpen] = useState(false)
  const Icon = item.icon

  if (item.disabled) {
    return (
      <div
        className="menu-item disabled"
        title={item.disabledNote}
        aria-disabled="true"
      >
        <span className="menu-icon"><Icon size={16} /></span>
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
        tabIndex={0}
      >
        <span className="menu-icon"><Icon size={16} /></span>
        <span className="menu-label">{item.label}</span>
        <span className="menu-arrow"><menuIcons.chevronRight size={14} /></span>
        {submenuOpen && (
          <div className="menu-submenu" role="menu">
            {item.items.map((sub, i) => (
              <MenuItemRow key={i} item={sub} onDone={onDone} />
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
        onDone()
      }}
    >
      <span className="menu-icon"><Icon size={16} /></span>
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
      <button className="menu-zoom-btn" title="تصغير (Ctrl+-)"
        onClick={() => step(-0.35)} aria-label="تصغير">
        <menuIcons.minus size={15} />
      </button>
      <span className="menu-zoom-value">{zoom}%</span>
      <button className="menu-zoom-btn" title="تكبير (Ctrl++)"
        onClick={() => step(0.35)} aria-label="تكبير">
        <menuIcons.plus size={15} />
      </button>
      <button
        className="menu-zoom-btn"
        title="ملء الشاشة (F11)"
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

export function BrowserMenu({ state }: { state: AppState | null }) {
  const [open, setOpen] = useState(false)
  const rootRef = useRef<HTMLDivElement>(null)
  const buttonRef = useRef<HTMLButtonElement>(null)

  // Close on outside click / Escape.
  useEffect(() => {
    if (!open) return
    const onDown = (e: MouseEvent) => {
      if (rootRef.current && !rootRef.current.contains(e.target as Node)) {
        setOpen(false)
      }
    }
    const onKey = (e: KeyboardEvent) => {
      if (e.key === 'Escape') setOpen(false)
    }
    window.addEventListener('mousedown', onDown)
    window.addEventListener('keydown', onKey)
    return () => {
      window.removeEventListener('mousedown', onDown)
      window.removeEventListener('keydown', onKey)
    }
  }, [open])

  if (!state) return null

  const close = () => setOpen(false)
  const rows = buildMenuRows({ state, go: send, close })

  return (
    <div className="menu-root" ref={rootRef}>
      <button
        ref={buttonRef}
        className={'nav-btn menu-btn' + (open ? ' on' : '')}
        title="القائمة"
        aria-label="القائمة الرئيسية"
        aria-haspopup="menu"
        aria-expanded={open}
        onClick={() => setOpen((v) => !v)}
      >
        <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <line x1="4" y1="7" x2="20" y2="7" />
          <line x1="4" y1="12" x2="20" y2="12" />
          <line x1="4" y1="17" x2="20" y2="17" />
        </svg>
      </button>

      {open && (
        <div className="browser-menu" role="menu" aria-label="القائمة الرئيسية">
          {rows.map((row, i) => {
            if (row.kind === 'divider') {
              return <div key={i} className="menu-divider" />
            }
            if (row.kind === 'zoomRow') {
              return <ZoomRow key={i} state={state} close={close} />
            }
            return <MenuItemRow key={i} item={row} onDone={close} />
          })}
        </div>
      )}
    </div>
  )
}
