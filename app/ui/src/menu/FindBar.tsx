// Phase 7: find-in-page bar. Wired to the CEF Find() API through the
// findInPage/stopFind bridge actions; match counts come from the state
// channel (findCount / findActiveMatch).
import { useEffect, useRef, useState } from 'react'
import { send, type AppState } from '../common/bridge'

export function FindBar({ state }: { state: AppState }) {
  const [text, setText] = useState('')
  const inputRef = useRef<HTMLInputElement>(null)
  const findNext = useRef(false)

  useEffect(() => {
    inputRef.current?.focus()
  }, [])

  const run = (forward: boolean) => {
    const t = text.trim()
    if (!t) return
    void send({ type: 'findInPage', text: t, forward, findNext: findNext.current })
      .catch(() => {})
    findNext.current = true
  }

  const stop = () => {
    void send({ type: 'stopFind' }).catch(() => {})
  }

  return (
    <div className="find-bar" role="search" aria-label="بحث في الصفحة">
      <input
        ref={inputRef}
        type="text"
        placeholder="بحث في الصفحة…"
        value={text}
        onChange={(e) => {
          setText(e.target.value)
          findNext.current = false
        }}
        onKeyDown={(e) => {
          if (e.key === 'Enter') run(!e.shiftKey)
          if (e.key === 'Escape') stop()
        }}
        aria-label="نص البحث"
      />
      <span className="find-count" aria-live="polite">
        {state.findCount > 0
          ? `${state.findActiveMatch} / ${state.findCount}`
          : state.findCount === 0 && findNext.current
            ? '0 / 0'
            : ''}
      </span>
      <button className="find-btn" title="السابق" onClick={() => run(false)} aria-label="النتيجة السابقة">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <path d="M18 15l-6-6-6 6" />
        </svg>
      </button>
      <button className="find-btn" title="التالي" onClick={() => run(true)} aria-label="النتيجة التالية">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <path d="M6 9l6 6 6-6" />
        </svg>
      </button>
      <button className="find-btn" title="إغلاق (Esc)" onClick={stop} aria-label="إغلاق البحث">
        <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" strokeWidth="2" strokeLinecap="round">
          <line x1="6" y1="6" x2="18" y2="18" />
          <line x1="18" y1="6" x2="6" y2="18" />
        </svg>
      </button>
    </div>
  )
}
