# AGENTS.md — تعليمات لكل وكلاء يعملون على DASK Browser

## السياق
متصفح Windows أصلي على محرك Chromium عبر **CEF**. C++20 + CMake. الواجهة React + TypeScript (المرحلة 2+).

## قرارات ملزمة
- **CEF فقط.** ممنوع WebView2 أو Electron أو محرك بديل.
- **لا تبنِ Chromium من المصدر.** استخدم توزيعة cef-builds الرسمية prebuilt فقط.
- **لا تعطّل Chromium sandbox** لتسهيل التشغيل إطلاقًا.
- لا تخلط ملفات CEF الضخمة داخل Git: `third_party/` و`*.tar.bz2` و`scripts/cef_root.txt` كلها في `.gitignore` — حافظ على ذلك.

## بنية المشروع
```
app/native/       عملية المتصفح (Win32 + CEF): النافذة، التبويبات، جسر IPC، سكيما UI
app/subprocess/   عملية CEF الفرعية (يجب أن تبقى منفصلة عن الـ exe الرئيسي)
app/ui/           واجهة React + TypeScript (شريط الأدوات + صفحة New Tab) — ناتج dist يُنسخ إلى ui/ بجانب exe
resources/        صفحات/ملفات محلية مشحونة مع التطبيق
scripts/          download_cef.ps1 وسكربتات أخرى
tests/            smoke_test.ps1
```

## واجهة المستخدم وجسر IPC (المرحلة 2+)
- الواجهة تشتغل داخل CEF نفسها على **`http://dask.ui/index.html`** (interception عبر `CefRegisterSchemeHandlerFactory`، الملفات تُقدَّم من مجلد `ui/` بجانب الـ exe فقط — ممنوع path traversal).
- جسر واحد عبر **CefMessageRouter** (`window.cefQuery`). أنواع الطلبات: `getState`/`wait` (قناة الحالة: كل صفحة تملك استعلام `wait` خاصاً بها، لا خانة مشتركة — لا تتراجع عن هذا التصميم)، `navigate`، `back`، `forward`، `reload`، `stop`، `home`، `createTab`، `closeTab`، `activateTab`، `setQuickLinks`، `setSearchEngine`.
- **الأمان:** الطلبات مقبولة فقط من متصفح شريط الأدوات (tab_id=0) أو صفحة داخلية تبدأ بـ `http://dask.ui/`. المحتوى الويب العادي يرفض بـ 403. `createTab` يقبل http(s) فقط، و`setQuickLinks` يرفض الروابط غير http.
- لا تخزين كلمات مرور ولا محفوظات دائم؛ الجلسة (روابط التبويبات + المحرك + الاختصارات) في `%LOCALAPPDATA%\DaskBrowser\session.json` فقط.

## قواعد العمل
1. **الإغلاق:** اتبع ترتيب CEF الإلزامي: `CloseBrowser(true)` → انتظار `OnBeforeClose` → إنهاء حلقة الرسائل → `CefShutdown()`. لا تستدعِ CefShutdown بينما المتصفح حي.
2. **العمليات الفرعية:** `browser_subprocess_path` يشير إلى `dask_browser_subprocess.exe`. أي عملية جديدة تضيفها لا تمس هذا.
3. **كل process type يحتاج معالجته** في `dask_app` إن أضفت سلوكًا خاصًا بنوع العملية.
4. **CefSettings.cache_path فارغ حاليًا (in-memory).** غيّره فقط عند الحاجة الفعلية للبيانات المستمرة، وضع المجلد تحت `%LOCALAPPDATA%\DaskBrowser`.
5. عند إضافة تبعية ثالثة سجّلها في `THIRD_PARTY_NOTICES.md` (اسم، إصدار، مصدر، ترخيص).
6. بعد أي تغيير في البنية: حدّث `README.md` و`tests/smoke_test.ps1`.
7. أصلح التحذيرات بدل كتمها؛ لا تستخدم `/W0` ولا تعطيل تحذيرات عالميًا.

## بناء وتشغيل
```bat
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
build\bin\Release\dask_browser.exe
powershell -ExecutionPolicy Bypass -File tests\smoke_test.ps1
```

## المرحلة الحالية فقط
Phase 2 (Browser UI) مكتملة: تبويبات، شريط أدوات RTL، صفحة New Tab، اختصارات لوحة مفاتيح، جسر IPC. المرحلة التالية (الخصوصية وحجب الإعلانات) لا تبدأ إلا بطلب صريح من المستخدم.
