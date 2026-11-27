// Phase 7: declarative definition of the main browser menu, following the
// required grouping. Real actions only — items without an implemented
// backend are marked disabled with the reason (never faked).
import type { MenuContext, MenuRow } from './menuModel'
import { menuIcons } from './menuIcons'

export function buildMenuRows(ctx: MenuContext): MenuRow[] {
  const { go, close } = ctx
  // Context-dependent items: "install page as app" exists only while a
  // real web page is open (not on internal/new-tab pages).
  const tab = ctx.state.tabs.find((t) => t.id === ctx.state.activeTabId)
  const siteOpen = !!tab && !tab.internal && /^https?:\/\//.test(tab.url)

  return [
    // ---- Group 1: windows and tabs ----
    { kind: 'item', label: 'تبويب جديد', shortcut: 'Ctrl+T', icon: menuIcons.newTab,
      run: () => go({ type: 'createTab' }) },
    { kind: 'item', label: 'نافذة جديدة', shortcut: 'Ctrl+N', icon: menuIcons.newWindow,
      run: () => go({ type: 'newWindow' }) },
    { kind: 'item', label: 'نافذة خاصة', shortcut: 'Ctrl+Shift+N', icon: menuIcons.private,
      run: () => go({ type: 'newPrivateWindow' }) },
    { kind: 'divider' },
    // ---- Group 2: browser data ----
    { kind: 'item', label: 'كلمات المرور والتعبئة التلقائية', icon: menuIcons.key,
      disabled: true, disabledNote: 'لا يخزّن DASK كلمات مرور عمداً' },
    { kind: 'item', label: 'السجل', icon: menuIcons.history,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#history' }) },
    { kind: 'item', label: 'المفضلة والقوائم', icon: menuIcons.bookmark,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#bookmarks' }) },
    { kind: 'item', label: 'التنزيلات', shortcut: 'Ctrl+J', icon: menuIcons.download,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#downloads' }) },
    { kind: 'item', label: 'الإضافات', icon: menuIcons.puzzle,
      disabled: true, disabledNote: 'بنية CEF الحالية بلا محرك إضافات' },
    { kind: 'item', label: 'حذف بيانات التصفح', shortcut: 'Ctrl+Shift+Del', icon: menuIcons.trash,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#privacy' }) },
    { kind: 'divider' },
    // ---- Group 3: zoom + fullscreen ----
    { kind: 'zoomRow' },
    { kind: 'divider' },
    // ---- Group 4: page actions ----
    { kind: 'item', label: 'طباعة', shortcut: 'Ctrl+P', icon: menuIcons.print,
      run: () => go({ type: 'printPage' }) },
    { kind: 'item', label: 'بحث وتحرير', icon: menuIcons.search, items: [
      { kind: 'header', label: 'بحث' },
      { kind: 'item', label: 'بحث في الصفحة', shortcut: 'Ctrl+F', icon: menuIcons.search,
        run: () => close() },
      { kind: 'header', label: 'تحرير' },
      { kind: 'item', label: 'قص', shortcut: 'Ctrl+X', icon: menuIcons.scissors,
        run: () => go({ type: 'editCommand', cmd: 'cut' }) },
      { kind: 'item', label: 'نسخ', shortcut: 'Ctrl+C', icon: menuIcons.copy,
        run: () => go({ type: 'editCommand', cmd: 'copy' }) },
      { kind: 'item', label: 'لصق', shortcut: 'Ctrl+V', icon: menuIcons.clipboard,
        run: () => go({ type: 'editCommand', cmd: 'paste' }) },
      { kind: 'item', label: 'عرض المصدر', shortcut: 'Ctrl+U', icon: menuIcons.fileText,
        run: () => go({ type: 'viewSource' }) },
      { kind: 'item', label: 'نسخ عنوان الصفحة', icon: menuIcons.link,
        run: () => go({ type: 'copyPageLink' }) },
    ] },
    { kind: 'item', label: 'حفظ ومشاركة', icon: menuIcons.share, items: [
      { kind: 'header', label: 'حفظ' },
      { kind: 'item', label: 'حفظ الصفحة باسم… (PDF)', icon: menuIcons.fileText,
        run: () => go({ type: 'savePagePdf' }) },
      ...(siteOpen
        ? [{ kind: 'item' as const, label: 'تثبيت الصفحة كتطبيق…',
            icon: menuIcons.monitor,
            run: () => go({ type: 'installPageApp' }) }]
        : []),
      { kind: 'item', label: 'إنشاء اختصار…', icon: menuIcons.monitor,
        run: () => go({ type: 'createShortcut' }) },
      { kind: 'header', label: 'مشاركة' },
      { kind: 'item', label: 'نسخ الرابط', icon: menuIcons.link,
        run: () => go({ type: 'copyPageLink' }) },
      { kind: 'item', label: 'إرسال إلى جهاز آخر', icon: menuIcons.sync,
        disabled: true, disabledNote: 'يتطلب مزامنة الأجهزة — غير متاح' },
      { kind: 'item', label: 'إنشاء رمز QR', icon: menuIcons.qrCode,
        disabled: true, disabledNote: 'غير متاح بعد في هذا الإصدار' },
    ] },
    { kind: 'item', label: 'أدوات إضافية', icon: menuIcons.tools, items: [
      { kind: 'item', label: 'ملف شخصي جديد', icon: menuIcons.userPlus,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#profiles' }) },
      { kind: 'item', label: 'فتح ملف الضيف', icon: menuIcons.guest,
        run: () => go({ type: 'newPrivateWindow' }) },
      { kind: 'item', label: 'المزامنة', icon: menuIcons.sync,
        disabled: true, disabledNote: 'يتطلب حساب مزامنة — غير متاح' },
      { kind: 'divider' },
      { kind: 'item', label: 'بحث في التبويبات', shortcut: 'Ctrl+Shift+A', icon: menuIcons.tabSearch,
        disabled: true, disabledNote: 'غير متاح بعد في هذا الإصدار' },
      { kind: 'item', label: 'أدوات المطور', shortcut: 'Ctrl+Shift+I', icon: menuIcons.tools,
        run: () => go({ type: 'showDevTools' }) },
      { kind: 'item', label: 'عرض المصدر', shortcut: 'Ctrl+U', icon: menuIcons.fileText,
        run: () => go({ type: 'viewSource' }) },
      { kind: 'item', label: 'مدير المهام', shortcut: 'Shift+Esc', icon: menuIcons.taskManager,
        disabled: true, disabledNote: 'غير متاح في بنية CEF الحالية' },
    ] },
    { kind: 'divider' },
    // ---- Group 5: help and app ----
    { kind: 'item', label: 'مساعدة', icon: menuIcons.help, items: [
      { kind: 'item', label: 'حول DASK Browser', icon: menuIcons.info,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#about' }) },
      { kind: 'item', label: 'مركز المساعدة', icon: menuIcons.lifeBuoy,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/help.html' }) },
      { kind: 'item', label: 'اختصارات لوحة المفاتيح', icon: menuIcons.info,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/help.html#shortcuts' }) },
      { kind: 'item', label: 'الإبلاغ عن موقع معطّل', icon: menuIcons.alertTriangle,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#privacy' }) },
    ] },
    { kind: 'item', label: 'الإعدادات', icon: menuIcons.settings,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html' }) },
    { kind: 'item', label: 'خروج', icon: menuIcons.exit,
      run: () => go({ type: 'exitApp' }) },
  ]
}
