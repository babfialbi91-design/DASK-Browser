import { useEffect, useState } from 'react'
import { createRoot } from 'react-dom/client'
import '../common/theme.css'
import {
  send,
  sendTyped,
  subscribe,
  type AppState,
  type BookmarkUi,
  type HistoryItemUi,
} from '../common/bridge'

type Section = 'general' | 'privacy' | 'profiles' | 'permissions' | 'bookmarks' | 'history' | 'downloads' | 'about'

const SECTIONS: { id: Section; label: string }[] = [
  { id: 'general', label: 'عام' },
  { id: 'profiles', label: 'الملفات الشخصية' },
  { id: 'permissions', label: 'الأذونات' },
  { id: 'privacy', label: 'الخصوصية' },
  { id: 'bookmarks', label: 'المفضلة' },
  { id: 'history', label: 'السجل' },
  { id: 'downloads', label: 'التنزيلات' },
  { id: 'about', label: 'حول' },
]

const ENGINES = [
  { id: 'google', label: 'Google' },
  { id: 'bing', label: 'Bing' },
  { id: 'duckduckgo', label: 'DuckDuckGo' },
  { id: 'custom', label: 'مخصص' },
]

function App() {
  const [state, setState] = useState<AppState | null>(null)
  const [section, setSection] = useState<Section>('general')
  const [clearNote, setClearNote] = useState('')
  const [historyQuery, setHistoryQuery] = useState('')
  const [historyRows, setHistoryRows] = useState<HistoryItemUi[]>([])
  const [bookmarkQuery, setBookmarkQuery] = useState('')
  const [newProfileName, setNewProfileName] = useState('')
  const [confirmOpen, setConfirmOpen] = useState<number | null>(null)

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

  const runHistorySearch = (query: string) => {
    void sendTyped<{ items: HistoryItemUi[] }>({
      type: 'searchHistory',
      query,
      limit: 100,
    })
      .then((res) => setHistoryRows(res.items ?? []))
      .catch((err) => console.error(err))
  }

  if (!state || !state.privacy) return <div className="settings-root" />
  const p = state.privacy

  const filteredBookmarks: BookmarkUi[] = state.bookmarks.filter(
    (b) =>
      !bookmarkQuery ||
      b.title.toLowerCase().includes(bookmarkQuery.toLowerCase()) ||
      b.url.toLowerCase().includes(bookmarkQuery.toLowerCase()),
  )

  return (
    <div className="settings-root">
      <h1>الإعدادات</h1>

      <div className="settings-tabs" role="tablist">
        {SECTIONS.map((sec) => (
          <button
            key={sec.id}
            className={'settings-tab' + (section === sec.id ? ' on' : '')}
            role="tab"
            aria-selected={section === sec.id}
            onClick={() => setSection(sec.id)}
          >
            {sec.label}
          </button>
        ))}
      </div>

      {state.dbNote && <p className="note warn">{state.dbNote}</p>}

      {section === 'general' && (
        <section>
          <h2>عام</h2>
          <label className="toggle-row">
            <input
              type="checkbox"
              checked={state.restoreSession}
              onChange={(e) =>
                go({ type: 'setRestoreSession', enabled: e.target.checked })
              }
            />
            <span>استعادة التبويبات المفتوحة عند بدء التشغيل.</span>
          </label>

          <h3>محرك البحث الافتراضي</h3>
          {ENGINES.map((engine) => (
            <label key={engine.id} className="toggle-row">
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
        </section>
      )}

      {section === 'profiles' && (
        <section>
          <h2>الملفات الشخصية</h2>
          <p className="privacy-note">
            كل ملف شخصي له مجلد منفصل تماماً: قاعدة بيانات خاصة (مفضلة، سجل،
            تنزيلات)، إعدادات، ملفات تعريف ارتباط وذاكرة CEF. التبديل يغلق
            تبويبات الجلسة الحالية ويستعيد جلسة الملف الشخصي المختار.
          </p>
          <ul className="exceptions">
            {state.profiles.map((prof) => (
              <li key={prof.id} className="bookmark-row">
                <strong>{prof.name}</strong>
                {prof.active && <span className="note"> (نشط)</span>}
                {!prof.active && (
                  <span style={{ display: 'flex', gap: 8 }}>
                    <button
                      className="btn small"
                      onClick={() => go({ type: 'switchProfile', id: prof.id })}
                    >
                      التبديل إليه
                    </button>
                    {state.profiles.length > 1 && (
                      <button
                        className="btn small"
                        onClick={() => go({ type: 'deleteProfile', id: prof.id })}
                      >
                        حذف
                      </button>
                    )}
                  </span>
                )}
              </li>
            ))}
          </ul>
          <div className="btn-row">
            <input
              className="search-input"
              type="text"
              placeholder="اسم الملف الشخصي الجديد…"
              aria-label="اسم الملف الشخصي"
              value={newProfileName}
              onChange={(e) => setNewProfileName(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter' && newProfileName.trim()) {
                  go({ type: 'addProfile', name: newProfileName.trim() })
                  setNewProfileName('')
                }
              }}
            />
            <button
              className="btn"
              disabled={!newProfileName.trim()}
              onClick={() => {
                go({ type: 'addProfile', name: newProfileName.trim() })
                setNewProfileName('')
              }}
            >
              إنشاء ملف شخصي
            </button>
          </div>
        </section>
      )}

      {section === 'permissions' && (
        <section>
          <h2>أذونات المواقع</h2>
          <p className="privacy-note">
            كل إذن يبدأ بحالة "اسأل" — تظهر فقاعة في شريط الأدوات عند الطلب.
            القرارات المحفوظة تخص الملف الشخصي الحالي فقط، ويمكن سحبها في أي
            وقت. التبويبات الخاصة لا تخزّن قرارات الأذونات.
          </p>
          {state.permissions.length === 0 && (
            <p className="privacy-note">لا قرارات محفوظة بعد.</p>
          )}
          <ul className="exceptions">
            {state.permissions.map((perm) => (
              <li key={perm.host + ":" + perm.perm} className="bookmark-row">
                <code>{perm.host}</code>
                <span>{perm.perm}</span>
                <span className="note">
                  {perm.decision === 1 ? 'مسموح' : 'مرفوض'}
                </span>
                <button
                  className="btn small"
                  onClick={() =>
                    go({
                      type: 'setPermission',
                      host: perm.host,
                      perm: perm.perm,
                      decision: perm.decision === 1 ? 2 : 1,
                    })
                  }
                >
                  {perm.decision === 1 ? 'تحويل إلى رفض' : 'تحويل إلى سماح'}
                </button>
                <button
                  className="btn small"
                  onClick={() =>
                    go({
                      type: 'resetPermission',
                      host: perm.host,
                      perm: perm.perm,
                    })
                  }
                >
                  سحب (إعادة السؤال)
                </button>
              </li>
            ))}
          </ul>
        </section>
      )}

      {section === 'privacy' && (
        <section>
          <h2>حجب الإعلانات والمتتبعات</h2>
          <label className="toggle-row">
            <input
              type="checkbox"
              checked={p.enabled}
              onChange={(e) =>
                go({ type: 'setBlockingEnabled', enabled: e.target.checked })
              }
            />
            <span>
              حجب الإعلانات والمتتبعات (شبكةً) — مستوى الشبكة فقط: لا إزالة
              عناصر من الصفحة.
            </span>
          </label>

          <h3>القوائم</h3>
          <ul className="lists">
            {p.lists.map((list) => (
              <li key={list.name}>
                <strong>{list.name}</strong> —{' '}
                {list.rules.toLocaleString('ar')} قاعدة؛{' '}
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
          {p.siteExceptions.length === 0 && (
            <p className="privacy-note">لا استثناءات — الحجب مفعّل لكل المواقع.</p>
          )}
          <ul className="exceptions">
            {p.siteExceptions.map((exc) => (
              <li key={exc.host}>
                <code>{exc.host}</code>
                <button
                  className="btn small"
                  onClick={() =>
                    go({ type: 'removeSiteException', host: exc.host })
                  }
                >
                  إزالة الاستثناء
                </button>
              </li>
            ))}
          </ul>

          <h2>ملفات تعريف الارتباط</h2>
          <label className="toggle-row">
            <input
              type="checkbox"
              checked={p.thirdPartyCookies}
              onChange={(e) =>
                go({
                  type: 'setThirdPartyCookies',
                  enabled: e.target.checked,
                })
              }
            />
            <span>
              حجب ملفات تعريف الارتباط من طرف ثالث (يمنع تتبّعك بين المواقع).
            </span>
          </label>

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
      )}

      {section === 'bookmarks' && (
        <section>
          <h2>المفضلة ({state.bookmarks.length})</h2>
          <input
            className="search-input"
            type="text"
            placeholder="ابحث في المفضلة…"
            aria-label="بحث في المفضلة"
            value={bookmarkQuery}
            onChange={(e) => setBookmarkQuery(e.target.value)}
          />
          {filteredBookmarks.length === 0 && (
            <p className="privacy-note">
              لا إشارات مرجعية. أضفها بزر النجمة في شريط الأدوات أو Ctrl+D.
            </p>
          )}
          <ul className="exceptions">
            {filteredBookmarks.map((bm) => (
              <li key={bm.id} className="bookmark-row">
                <input
                  className="title-input"
                  value={bm.title}
                  aria-label="اسم الإشارة"
                  onChange={(e) => {
                    // Local edit; saved on blur via updateBookmark below.
                    bm.title = e.target.value
                  }}
                  onBlur={() =>
                    go({
                      type: 'updateBookmark',
                      id: bm.id,
                      title: bm.title,
                      url: bm.url,
                    })
                  }
                />
                <span className="bm-url">{bm.url}</span>
                <button
                  className="btn small"
                  onClick={() => go({ type: 'removeBookmark', id: bm.id })}
                >
                  حذف
                </button>
              </li>
            ))}
          </ul>
        </section>
      )}

      {section === 'history' && (
        <section>
          <h2>السجل</h2>
          <label className="toggle-row">
            <input
              type="checkbox"
              checked={state.historyEnabled}
              onChange={(e) =>
                go({ type: 'setHistoryEnabled', enabled: e.target.checked })
              }
            />
            <span>
              تسجيل صفحات التصفح (العناوين فقط — لا نماذج ولا محتوى صفحات).
              التبويبات الخاصة لا تُسجَّل أبداً.
            </span>
          </label>
          <label className="toggle-row">
            <span>مدة الاحتفاظ:</span>
            <select
              value={state.retentionDays}
              onChange={(e) =>
                go({ type: 'setRetentionDays', days: Number(e.target.value) })
              }
            >
              <option value={7}>7 أيام</option>
              <option value={30}>30 يوماً</option>
              <option value={90}>90 يوماً</option>
              <option value={0}>دائماً (لا حذف تلقائي)</option>
            </select>
          </label>

          <div className="btn-row">
            <input
              className="search-input"
              type="text"
              placeholder="ابحث في السجل…"
              aria-label="بحث في السجل"
              value={historyQuery}
              onChange={(e) => setHistoryQuery(e.target.value)}
              onKeyDown={(e) => {
                if (e.key === 'Enter') runHistorySearch(historyQuery)
              }}
            />
            <button className="btn" onClick={() => runHistorySearch(historyQuery)}>
              بحث
            </button>
          </div>

          <ul className="exceptions">
            {historyRows.map((row) => (
              <li key={row.id}>
                <span>{row.title || row.url}</span>
                <div className="list-url">{row.url}</div>
              </li>
            ))}
          </ul>

          <h3>مسح السجل</h3>
          <div className="btn-row">
            <button
              className="btn"
              onClick={() => {
                go({ type: 'clearHistory', days: 0 })
                setHistoryRows([])
                setClearNote('تم مسح السجل بالكامل.')
              }}
            >
              مسح السجل بالكامل
            </button>
            <button
              className="btn"
              onClick={() => {
                go({ type: 'clearHistory', days: 7 })
                runHistorySearch(historyQuery)
                setClearNote('تم حذف سجل الأسبوع الأخير وما قبله.')
              }}
            >
              حذف آخر 7 أيام (وما قبله)
            </button>
            <button
              className="btn"
              onClick={() => {
                go({ type: 'clearHistory', days: 30 })
                runHistorySearch(historyQuery)
                setClearNote('تم حذف سجل آخر 30 يوماً (وما قبله).')
              }}
            >
              حذف آخر 30 يوماً (وما قبله)
            </button>
          </div>
        </section>
      )}

      {section === 'about' && (
        <section>
          <h2>حول DASK Browser</h2>
          <p className="privacy-note">
            DASK Browser الإصدار 1.0.0 — متصفح ويب Windows أصلي على محرك
            Chromium المضمن عبر CEF (Chromium 158.0.8127.89).
          </p>
          <p className="privacy-note">
            الترخيص: كود المشروع يتبع ملف LICENSE؛ CEF وChromium مرخصان BSD؛
            EasyList وEasyPrivacy (GPLv3) وPhishing Army (CC0) تُستخدم
            كبيانات فقط؛ التفاصيل الكاملة في THIRD_PARTY_NOTICES.md المرفق
            مع الحزمة.
          </p>
          <p className="privacy-note">
            الخصوصية: كل البيانات محلية — لا تتبّع، لا رفع، لا كلمات مرور
            محفوظة، وقاعدة البيانات في مجلد ملفك الشخصي فقط.
          </p>
        </section>
      )}

      {section === 'downloads' && (
        <section>
          <h2>التنزيلات</h2>
          <button className="btn" onClick={() => go({ type: 'openDownloadsFolder' })}>
            فتح مجلد التنزيلات
          </button>
          {state.downloads.length === 0 && (
            <p className="privacy-note">لا تنزيلات بعد.</p>
          )}
          <ul className="exceptions">
            {state.downloads.map((dl) => (
              <li key={dl.id || dl.filename}>
                <strong>{dl.filename}</strong>
                {dl.risky && (
                  <span className="note warn"> — ⚠ ملف تنفيذي/سكربت: قد يشغّل كوداً على جهازك</span>
                )}
                {confirmOpen === dl.id && (
                  <p className="note warn">
                    هل تريد فتح هذا الملف فعلاً؟ الملفات التنفيذية قد تكون خطرة.
                    <button
                      className="btn small"
                      style={{ marginRight: 8 }}
                      onClick={() => {
                        setConfirmOpen(null)
                        go({ type: 'openDownload', id: dl.id })
                      }}
                    >
                      نعم، افتح
                    </button>
                    <button
                      className="btn small"
                      onClick={() => setConfirmOpen(null)}
                    >
                      إلغاء
                    </button>
                  </p>
                )}
                {dl.total > 0 && dl.state === 0 && (
                  <span>
                    {' '}
                    —{' '}
                    {Math.round((dl.received / dl.total) * 100)}%
                  </span>
                )}
                {dl.state === 1 && <span> — مكتمل</span>}
                {dl.state === 2 && <span> — أُلغي</span>}
                {dl.state === 3 && <span> — فشل</span>}
                <div className="list-url">{dl.url}</div>
                <div className="btn-row">
                  {dl.state === 1 && (
                    <button
                      className="btn small"
                      onClick={() => {
                        // Executable/script files require an explicit
                        // confirmation before they are opened (Phase 5).
                        if (dl.risky && confirmOpen !== dl.id) {
                          setConfirmOpen(dl.id)
                          return
                        }
                        setConfirmOpen(null)
                        go({ type: 'openDownload', id: dl.id })
                      }}
                    >
                      فتح الملف
                    </button>
                  )}
                  {dl.state === 0 && (
                    <button
                      className="btn small"
                      onClick={() => go({ type: 'cancelDownload', id: dl.id })}
                    >
                      إلغاء
                    </button>
                  )}
                  <button
                    className="btn small"
                    onClick={() => go({ type: 'removeDownload', id: dl.id })}
                  >
                    إزالة من القائمة
                  </button>
                </div>
              </li>
            ))}
          </ul>
        </section>
      )}
    </div>
  )
}

createRoot(document.getElementById('root')!).render(<App />)
