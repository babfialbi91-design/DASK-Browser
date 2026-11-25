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

:: 3) Build (Debug أو Release)
cmake --build build --config Release --parallel
cmake --build build --config Debug --parallel

:: 4) تشغيل
build\bin\Release\dask_browser.exe
```

- مسارات CEF البديلة: `cmake -DCEF_ROOT="C:/path/to/cef_binary_..."` أو متغير البيئة `CEF_ROOT` أو ملف `scripts/cef_root.txt`.
- الـ CMake ينسخ ملفات CEF (libcef.dll وملفات .pak و locales) وصفحة الاختبار بجانب الملفات التنفيذية تلقائيًا — لا حاجة لنسخ يدوي.

## بنية المشروع

```
CMakeLists.txt          نظام البناء (Debug + Release)
app/native/             عملية المتصفح: نافذة Win32 + تهيئة CEF
  main.cpp              نقطة الدخول (browser process)
  browser_window.*      النافذة الأصلية، دورة الحياة، معالجة الأخطاء
  dask_app.*            CefApp المشترك بين كل العمليات
app/subprocess/         عملية CEF الفرعية (renderer / gpu / utility)
  subprocess_main.cpp   CefExecuteProcess
resources/              صفحة الاختبار المحلية (file://)
scripts/download_cef.ps1  تنزيل + تحقق SHA1 + فك ضغط CEF
scripts/run_smoke_test.ps1 (انظر tests/)
tests/smoke_test.ps1    اختبار: أدوات + CEF + configure + build + الملفات
third_party/cef/        توزيعة CEF (متجاهلة في Git)
```

## ما الذي ستراه عند التشغيل؟

نافذة `DASK Browser` بحجم 1280×800 تعرض `resources/test_page.html` (صفحة اختبار محلية آمنة) مرسومة عبر Chromium الحقيقي. العمليات الفرعية (renderer/gpu) تعمل عبر `dask_browser_subprocess.exe` وChromium sandbox **مفعّل**. عند فشل تحميل صفحة يظهر الخطأ في عنوان النافذة وسجل Debug.

- الإغلاق: زر النافذة `X` → `CloseBrowser` → `OnBeforeClose` → `CefShutdown` بالترتيب الصحيح.
- إذا نقصت ملفات CEF بجانب الـ exe تظهر رسالة خطأ واضحة تشرح الخطوات بدل انهيار صامت.

## الاختبار

```bat
powershell -ExecutionPolicy Bypass -File tests\smoke_test.ps1
powershell -ExecutionPolicy Bypass -File tests\smoke_test.ps1 -SkipBuild   :: فحص الملفات فقط
```

## ملاحظات الترخيص

CEF وChromium مرخصان BSD؛ انظر `THIRD_PARTY_NOTICES.md`.
