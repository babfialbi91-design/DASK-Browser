# DASK Browser

متصفح ويب Windows أصلي مبني على محرك **Chromium** المضمّن عبر **CEF** (Chromium Embedded Framework). C++20 + CMake. الواجهة (React + TypeScript) تأتي في المرحلة الثانية.

**المرحلة الحالية (1 — Foundation):** أساس المشروع قابل للبناء والتشغيل، مع نافذة Windows أصلية تعرض صفحة اختبار حقيقية عبر Chromium.

## المتطلبات

| الأداة | الإصدار | ملاحظات |
|---|---|---|
| Windows 10/11 | x64 | |
| Visual Studio 2022 Build Tools | مع workload ‏"Desktop development with C++" (MSVC v143 + Windows 11 SDK) | مثبّت على هذا الجهاز في `D:\VS2022BuildTools` |
| CMake | 3.21+ | يأتي ضمن Build Tools، أو `winget install Kitware.CMake` |
| Git | أي إصدار حديث | |
| CEF | 158.0.6 (Chromium 158.0.8127.89، قناة stable) | يُنزَّل تلقائيًا عبر السكربت أدناه — لا يُحفظ في Git |

> **ملاحظة Python:** مشروع CEF نفسه لا يحتاج Python لهذه المرحلة (لا نستخدم سكربتات cefclient). إن أردته لاحقًا: `winget install Python.Python.3.12`.

## الإعداد والبناء خطوة بخطوة

```bat
:: 0) إن لم يكن cmake على PATH، أضف أولاً:
set PATH=D:\VS2022BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%PATH%

:: 1) نزّل توزيعة CEF (~180 MB) إلى third_party/cef واكتب scripts/cef_root.txt
powershell -ExecutionPolicy Bypass -File scripts/download_cef.ps1

:: 2) Configure (استخدم Developer Command Prompt لـ VS أو أضف cmake إلى PATH)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

:: 2) بناء الواجهة (يتطلب Node.js 18+؛ مرة واحدة ثم عند تعديل الواجهة)
cd app\ui && npm install && npm run build && cd ..\..

:: 3) Configure (استخدم Developer Command Prompt لـ VS أو أضف cmake إلى PATH)
cmake -S . -B build -G "Visual Studio 17 2022" -A x64

:: 4) Build (Debug أو Release)
cmake --build build --config Release --parallel

:: 5) تشغيل
build\bin\dask_browser.exe
```

- مسارات CEF البديلة: `cmake -DCEF_ROOT="C:/path/to/cef_binary_..."` أو متغير البيئة `CEF_ROOT` أو ملف `scripts/cef_root.txt`.
- الـ CMake ينسخ ملفات CEF (libcef.dll وملفات .pak و locales) وصفحة الاختبار وناتج الواجهة (ui/) بجانب الملفات التنفيذية تلقائياً.

## المزايا الحالية (المرحلة 2)

- **تبويبات متعددة:** زر `+` أو `Ctrl+T` لفتح تبويب، `Ctrl+W` لإغلاقه، `Ctrl+Tab` للتنقل بينها. كل تبويب نافذة مضيفة Win32 أصلية تحت شريط الأدوات، وعملية renderer منفصلة لكل تبويب.
- **شريط أدوات RTL داكن:** تبويبات مع زر إغلاق، شريط عنوان ذكي (يُدخل URL أو يبحث في Google/ويكيبيديا/MDN… قابلة للتغيير من صفحة New Tab)، أزرار للخلف/للأمام/إعادة التحميل/الرئيسية.
- **صفحة New Tab:** شعار DASK، مربع بحث، اختصارات قابلة للتخصيص (تُحفظ مع الجلسة).
- **اختصارات لوحة المفاتيح:** `Ctrl+L` تركيز شريط العنوان، `Ctrl+T` تبويب جديد، `Ctrl+W` إغلاق، `Ctrl+R` إعادة تحميل، `Ctrl+Tab` تبويب التالي، `Alt+←/→` للخلف/للأمام.
- **جسر IPC آمن:** الواجهة تشتغل داخل CEF على `http://dask.ui/index.html` وتتواصل مع المحرك عبر `CefMessageRouter` (`window.cefQuery`). المحتوى الويب لا يمكنه الوصول إلى الجسر (403)؛ فتح التبويبات والروابط مقيّد بـ http(s).
- **جلسة خفيف:** روابط التبويبات المفتوحة + المحرك + الاختصارات تُحفظ في `%LOCALAPPDATA%\DaskBrowser\session.json`. لا كلمات مرور ولا محفوظات دائم (`cache_path` فارغ، in-memory).

## بنية المشروع

```
CMakeLists.txt          نظام البناء
app/native/             عملية المتصفح: نافذة Win32 + تهيئة CEF + التبويبات
  main.cpp              نقطة الدخول (browser process)
  browser_window.*      النافذة الأصلية، دورة الحياة، اختصارات لوحة المفاتيح
  tab_manager.*         التبويبات، جسر CefMessageRouter، الجلسة
  dask_app.*            CefApp المشترك بين كل العمليات
  dask_scheme.*         سكيما UI: تقديم http://dask.ui/* من مجلد ui/ محلياً
app/subprocess/         عملية CEF الفرعية (renderer / gpu / utility)
  subprocess_main.cpp   CefExecuteProcess
resources/              صفحة الاختبار المحلية (file://)
scripts/download_cef.ps1  تنزيل + تحقق SHA1 + فك ضغط CEF
app/ui/                 واجهة React + TypeScript (Vite): شريط الأدوات + New Tab + bridge.ts
resources/              صفحة الاختبار المحلية (file://)
scripts/download_cef.ps1  تنزيل + تحقق SHA1 + فك ضغط CEF
tests/smoke_test.ps1    اختبار: أدوات + CEF + الملفات + حزمة الواجهة
third_party/cef/        توزيعة CEF (متجاهلة في Git)
```

## ما الذي ستراه عند التشغيل؟

نافذة `DASK Browser` داكنة بحجم 1280×800: شريط تبويبات أعلى (تبويب New Tab جاهز)، تحته شريط عنوان وأزرار تنقل RTL، ثم منطقة محتوى الويب. العمليات الفرعية (renderer/gpu) تعمل عبر `dask_browser_subprocess.exe` وChromium sandbox **مفعّل**.

- الإغلاق: زر النافذة `X` أو إغلاق آخر تبويب → `CloseBrowser(true)` لكل تبويب → `OnBeforeClose` → `CefShutdown` بالترتيب الصحيح.
- إذا نقصت ملفات CEF أو حزمة الواجهة بجانب الـ exe تظهر رسالة خطأ واضحة بدل انهيار صامت.

## الاختبار

```bat
powershell -ExecutionPolicy Bypass -File tests\smoke_test.ps1
powershell -ExecutionPolicy Bypass -File tests\smoke_test.ps1 -SkipBuild   :: فحص الملفات فقط
```

## ملاحظات الترخيص

CEF وChromium مرخصان BSD؛ انظر `THIRD_PARTY_NOTICES.md`.
