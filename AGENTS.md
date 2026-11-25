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
app/native/       عملية المتصفح (Win32 + CEF)
app/subprocess/   عملية CEF الفرعية (يجب أن تبقى منفصلة عن الـ exe الرئيسي)
resources/        صفحات/ملفات محلية مشحونة مع التطبيق
scripts/          download_cef.ps1 وسكربتات أخرى
tests/            smoke_test.ps1
```

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
Phase 1 (Foundation). لا تبني مزايا مراحل لاحقة (UI، تابس، محفوظات...) إلا بطلب صريح من المستخدم.
