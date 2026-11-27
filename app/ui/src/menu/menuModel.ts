// Phase 7: menu item model + CSS classes for the browser menu.
// Keeping the item definitions here separates the declarative menu
// structure from the BrowserMenu rendering component.
import type { AppState } from '../common/bridge'
import type { MenuIconComponent } from './menuIcons'

export type MenuItem = {
  kind: 'item'
  label: string
  shortcut?: string
  icon: MenuIconComponent
  /** Leaf action (mutually exclusive with items). */
  run?: () => void
  /** Opens a submenu (mutually exclusive with run). */
  items?: MenuItem[]
  disabled?: boolean
  disabledNote?: string
}

export type MenuDivider = { kind: 'divider' }

export type MenuRow =
  | MenuItem
  | MenuDivider
  | { kind: 'zoomRow' }

export type MenuContext = {
  state: AppState
  go: (req: Parameters<typeof import('../common/bridge').send>[0]) => void
  close: () => void
}
