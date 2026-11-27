// Phase 7: declarative definition of the main browser menu, following the
// required grouping. Real actions only — items without an implemented
// backend are marked disabled with the reason (never faked).
import type { MenuContext, MenuRow } from './menuModel'
import { menuIcons } from './menuIcons'

export function buildMenuRows(ctx: MenuContext): MenuRow[] {
  const { go, close } = ctx

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
      { kind: 'item', label: 'عرض المصدر', shortcut: 'Ctrl+U', icon: menuIcons.fileText,
        run: () => go({ type: 'viewSource' }) },
      { kind: 'item', label: 'نسخ عنوان الصفحة', icon: menuIcons.link,
        run: () => go({ type: 'copyPageLink' }) },
    ] },
    { kind: 'item', label: 'حفظ ومشاركة', icon: menuIcons.share, items: [
      { kind: 'header', label: 'حفظ' },
      { kind: 'item', label: 'حفظ الصفحة كـ PDF', icon: menuIcons.fileText,
        run: () => go({ type: 'savePagePdf' }) },
      { kind: 'header', label: 'مشاركة' },
      { kind: 'item', label: 'نسخ الرابط', icon: menuIcons.link,
        run: () => go({ type: 'copyPageLink' }) },
    ] },
    { kind: 'item', label: 'أدوات إضافية', icon: menuIcons.tools, items: [
      { kind: 'item', label: 'أدوات المطور', shortcut: 'Ctrl+Shift+I', icon: menuIcons.tools,
        run: () => go({ type: 'showDevTools' }) },
      { kind: 'item', label: 'عرض المصدر', shortcut: 'Ctrl+U', icon: menuIcons.fileText,
        run: () => go({ type: 'viewSource' }) },
      { kind: 'item', label: 'طباعة', shortcut: 'Ctrl+P', icon: menuIcons.print,
        run: () => go({ type: 'printPage' }) },
      { kind: 'item', label: 'بحث في الصفحة', shortcut: 'Ctrl+F', icon: menuIcons.search,
        run: () => close() },
    ] },
    { kind: 'divider' },
    // ---- Group 5: help and app ----
    { kind: 'item', label: 'مساعدة', icon: menuIcons.help, items: [
      { kind: 'item', label: 'حول DASK Browser', icon: menuIcons.info,
        run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html#about' }) },
    ] },
    { kind: 'item', label: 'الإعدادات', icon: menuIcons.settings,
      run: () => go({ type: 'createTab', url: 'http://dask.ui/settings.html' }) },
    { kind: 'item', label: 'خروج', icon: menuIcons.exit,
      run: () => go({ type: 'exitApp' }) },
  ]
}
