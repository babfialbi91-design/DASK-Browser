// Phase 7: shared menu building blocks (BrowserMenu) — one file per concern.
// menuIcons.tsx holds a consistent inline-SVG icon set (Lucide-style
// 24x24, stroke 2) so no icon library dependency is needed.
import type { ReactNode } from 'react'

type IconProps = { size?: number }

function Svg({ size = 16, children }: { size?: number; children: ReactNode }) {
  return (
    <svg
      width={size}
      height={size}
      viewBox="0 0 24 24"
      fill="none"
      stroke="currentColor"
      strokeWidth="2"
      strokeLinecap="round"
      strokeLinejoin="round"
      aria-hidden="true"
    >
      {children}
    </svg>
  )
}

export const menuIcons = {
  hamburger: ({ size }: IconProps) => (
    <Svg size={size}>
      <line x1="4" y1="7" x2="20" y2="7" />
      <line x1="4" y1="12" x2="20" y2="12" />
      <line x1="4" y1="17" x2="20" y2="17" />
    </Svg>
  ),
  newTab: ({ size }: IconProps) => (
    <Svg size={size}>
      <rect x="3" y="3" width="18" height="18" rx="2" />
      <line x1="12" y1="9" x2="12" y2="15" />
      <line x1="9" y1="12" x2="15" y2="12" />
    </Svg>
  ),
  newWindow: ({ size }: IconProps) => (
    <Svg size={size}>
      <rect x="3" y="5" width="14" height="12" rx="2" />
      <line x1="8" y1="21" x2="14" y2="21" />
      <path d="M19 8v8" />
      <path d="M15 12h8" transform="translate(0,-0)" />
    </Svg>
  ),
  private: ({ size }: IconProps) => (
    <Svg size={size}>
      <rect x="3" y="11" width="18" height="10" rx="2" />
      <path d="M7 11V7a5 5 0 0 1 10 0v4" />
    </Svg>
  ),
  key: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="8" cy="15" r="4" />
      <path d="M10.8 12.2 20 3" />
      <path d="M17 6l3 3" />
    </Svg>
  ),
  history: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="12" cy="12" r="9" />
      <path d="M12 7v5l3 3" />
    </Svg>
  ),
  bookmark: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M6 3h12v18l-6-4-6 4V3z" />
    </Svg>
  ),
  download: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M12 3v12" />
      <path d="M7 10l5 5 5-5" />
      <path d="M4 21h16" />
    </Svg>
  ),
  puzzle: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M10 3h4v3a2 2 0 1 0 4 0h3v4h-3a2 2 0 1 0 0 4h3v4h-4v3a2 2 0 1 1-4 0v-3H7v-4h3a2 2 0 1 0 0-4H7V7h3V3z" />
    </Svg>
  ),
  trash: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M4 7h16" />
      <path d="M9 7V4h6v3" />
      <path d="M6 7l1 13h10l1-13" />
    </Svg>
  ),
  minus: ({ size }: IconProps) => (
    <Svg size={size}>
      <line x1="5" y1="12" x2="19" y2="12" />
    </Svg>
  ),
  plus: ({ size }: IconProps) => (
    <Svg size={size}>
      <line x1="12" y1="5" x2="12" y2="19" />
      <line x1="5" y1="12" x2="19" y2="12" />
    </Svg>
  ),
  fullscreen: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M4 9V4h5" />
      <path d="M20 9V4h-5" />
      <path d="M4 15v5h5" />
      <path d="M20 15v5h-5" />
    </Svg>
  ),
  print: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M6 9V3h12v6" />
      <rect x="4" y="9" width="16" height="8" rx="1" />
      <path d="M7 17h10v4H7z" />
    </Svg>
  ),
  search: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="11" cy="11" r="7" />
      <line x1="16.5" y1="16.5" x2="21" y2="21" />
    </Svg>
  ),
  share: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="6" cy="12" r="2.5" />
      <circle cx="18" cy="5" r="2.5" />
      <circle cx="18" cy="19" r="2.5" />
      <line x1="8.2" y1="10.8" x2="15.8" y2="6.2" />
      <line x1="8.2" y1="13.2" x2="15.8" y2="17.8" />
    </Svg>
  ),
  tools: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M14.7 6.3a4 4 0 0 1 5.3-3.7l-2.9 2.9 1.4 1.4 2.9-2.9a4 4 0 0 1-3.7 5.3L9 15.9a3 3 0 1 1-4.4-4.4l8.6-8.2z" transform="translate(2,2) scale(0.85)" />
    </Svg>
  ),
  help: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="12" cy="12" r="9" />
      <path d="M9.5 9a2.5 2.5 0 1 1 3.4 2.3c-.7.3-.9.9-.9 1.7" />
      <circle cx="12" cy="17" r="0.6" fill="currentColor" />
    </Svg>
  ),
  settings: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="12" cy="12" r="3" />
      <path d="M19.4 15a7.9 7.9 0 0 0 .1-1l2-1.5-2-3.4-2.3 1a7.9 7.9 0 0 0-1.7-1L15 6.5h-4l-.4 2.6a7.9 7.9 0 0 0-1.7 1l-2.3-1-2 3.4L6.5 14a7.9 7.9 0 0 0 0 2l-2.9 1.5 2 3.4" transform="translate(1.5,0) scale(0.93)" />
    </Svg>
  ),
  exit: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M9 21H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h4" />
      <path d="M16 17l5-5-5-5" />
      <line x1="21" y1="12" x2="9" y2="12" />
    </Svg>
  ),
  chevronRight: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M9 6l6 6-6 6" />
    </Svg>
  ),
  link: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M10 13a5 5 0 0 0 7.5.5l3-3a5 5 0 0 0-7-7l-1.7 1.7" />
      <path d="M14 11a5 5 0 0 0-7.5-.5l-3 3a5 5 0 0 0 7 7l1.7-1.7" />
    </Svg>
  ),
  fileText: ({ size }: IconProps) => (
    <Svg size={size}>
      <path d="M14 3H6a2 2 0 0 0-2 2v14a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V9z" />
      <path d="M14 3v6h6" />
      <line x1="9" y1="13" x2="15" y2="13" />
      <line x1="9" y1="17" x2="15" y2="17" />
    </Svg>
  ),
  info: ({ size }: IconProps) => (
    <Svg size={size}>
      <circle cx="12" cy="12" r="9" />
      <line x1="12" y1="10" x2="12" y2="16" />
      <circle cx="12" cy="7.5" r="0.6" fill="currentColor" />
    </Svg>
  ),
}

export type MenuIconComponent = (props: IconProps) => ReactNode
