import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import {
  send,
  subscribe,
  type AppState,
  type SiteException,
} from '../common/bridge'

function App() {
  const [state, setState] = useState<AppState | null>(null)
  const [clearNote, setClearNote] = useState('')

  useEffect(
    () =>
      subscribe((s) => {
        setState(s)
      }),
    [],
  )

  const go = (req: Parameters<typeof send>[0]) => {
    void send(req).catch((err) => console.error(err))
  }

  if (!state || !state.privacy) return <div className="settings-root" />
  const p = state.privacy

  const exceptions: SiteException[] = p.siteExceptions

  return (
    <div className="settings-root">
      <h1>الخصوصية وحجب الإعلانات</h1>
      <p className="privacy-note">
        كل الإعدادات محلية على جهازك. لا تجمع DASK Browser أي بيانات استخدام،
        ولا ترسل بيانات قياس عن بُعد، ولا تخزّن محفوظات التصفح أو كلمات المرور
        (الذاكرة فقط، ومسحها بإغلاق النافذة).
      </p>

      <section>
        <h2>حجب الإعلانات والمتتبعات</h2>
        <label className="toggle-row">
          <input
            type="checkbox"
            checked={p.enabled}
            onChange={(e) => go({ type: 'setBlockingEnabled', enabled: e.target.checked })}
          />
          <span>
            حجب الإعلانات والمتتبعات (شبكةً) — مستوى الشبكة فقط: لا إزالة عناصر
            من الصفحة.
          </span>
        </label>

        <h3>القوائم</h3>
        <ul className="lists">
          {p.lists.map((list) => (
            <li key={list.name}>
              <strong>{list.name}</strong> — {list.rules.toLocaleString('ar')} قاعدة؛{' '}
              {list.updated ? `آخر تحديث: ${list.updated}` : 'لم يُحدَّث بعد'}؛
              الحالة: {list.status}
              <div className="list-url">{list.url}</div>
            </li>
          ))}
        </ul>
        <button
          className="btn"
          disabled={p.updating}
          onClick={() => go({ type: 'updateFilterLists' })}
        >
          {p.updating ? 'جارٍ التحديث…' : 'تحديث القوائم الآن'}
        </button>

        <h3>استثناءات المواقع (حجب معطّل)</h3>
        {exceptions.length === 0 && <p>لا استثناءات — الحجب مفعّل لكل المواقع.</p>}
        <ul className="exceptions">
          {exceptions.map((exc) => (
            <li key={exc.host}>
              <code>{exc.host}</code> — {exc.allowed ? 'الحجب معطّل' : 'مفعّل'}
              <button
                className="btn small"
                onClick={() => go({ type: 'removeSiteException', host: exc.host })}
              >
                إزالة الاستثناء
              </button>
            </li>
          ))}
        </ul>
      </section>

      <section>
        <h2>ملفات تعريف الارتباط</h2>
        <label className="toggle-row">
          <input
            type="checkbox"
            checked={p.thirdPartyCookies}
            onChange={(e) =>
              go({ type: 'setThirdPartyCookies', enabled: e.target.checked })
            }
          />
          <span>
            حجب ملفات تعريف الارتباط من طرف ثالث (يمنع تتبّعك بين المواقع).
          </span>
        </label>
      </section>

      <section>
        <h2>مسح بيانات المواقع</h2>
        <p className="privacy-note">
          التخزين في الذاكرة فقط: لا محفوظات دائمة ولا كلمات مرور محفوظة.
          ملفات تعريف الارتباط تبقى أثناء التشغيل ويمكن مسحها هنا.
        </p>
        <div className="btn-row">
          <button
            className="btn"
            onClick={() => {
              go({ type: 'clearBrowsingData' })
              setClearNote('تم مسح كل ملفات تعريف الارتباط.')
            }}
          >
            مسح كل بيانات التصفح
          </button>
          {p.activeSite && (
            <button
              className="btn"
              onClick={() => {
                go({ type: 'clearSiteData', host: p.activeSite })
                setClearNote(`تم مسح بيانات ${p.activeSite}.`)
              }}
            >
              مسح بيانات الموقع الحالي ({p.activeSite})
            </button>
          )}
        </div>
        {clearNote && <p className="note">{clearNote}</p>}
      </section>
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<App />)
