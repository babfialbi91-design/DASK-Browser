# Third-Party Notices

## CEF (Chromium Embedded Framework)

- **الإصدار المستخدم:** 158.0.6+g23f1736+chromium-158.0.8127.89 (قناة stable)
- **المنصة:** windows64
- **نوع التوزيعة:** minimal (بدون تطبيقات الأمثلة)
- **مصدر التنزيل:** https://cef-builds.spotifycdn.com/cef_binary_158.0.6%2Bg23f1736%2Bchromium-158.0.8127.89_windows64_minimal.tar.bz2
- **SHA1 للملف المنزَّل:** 2abb78fcd3b07ea0fb2014e3867a0902f19f1731
- **الترخيص:** BSD 3-Clause (يمتد إلى مكونات Chromium المضمّنة) — https://bitbucket.org/chromiumembedded/cef/src/master/LICENSE.txt
- **ملاحظة:** لا تُبنى أي ملفات CEF من المصدر؛ التوزيعة prebuilt تُنزَّل عبر `scripts/download_cef.ps1` ولا تُخزَّن داخل Git.

## EasyList / EasyPrivacy (قوائم حجب الإعلانات)

- **ما:** قوائم قواعد حجب الإعلانات والمتتبعات بصيغة Adblock Plus، تُنزّل عند طلب المستخدم وتُخزن محلياً في `%LOCALAPPDATA%\DaskBrowser\filters\` (لا تُخزَّن في Git).
- **المصادر:**
  - EasyList: https://easylist.to/easylist/easylist.txt
  - EasyPrivacy: https://easylist.to/easylist/easyprivacy.txt
- **الترخيص:** GPLv3 (وفق صفحة المصدر على easylist.to). تُستخدم القوائم كبيانات فقط — لا تُنفَّذ أي كود منها.
- **الاستخدام:** يفسّرها محرك `app/native/adblock/filter_engine` على مستوى الشبكة فقط؛ القواعد الجمالية وغير المدعومة تُتجاهل.

## React / Vite / TypeScript (أدوات بناء الواجهة)

- **ما:** تبعيات Node لتطوير وبناء واجهة المستخدم (`app/ui/package.json`).
- **الترخيص:** React (MIT)، Vite (MIT)، TypeScript (Apache-2.0). أدوات بناء وقت التطوير فقط؛ الناتج يُقدّم محلياً داخل CEF.
- **سجل كامل:** انظر `app/ui/package.json` و`app/ui/package-lock.json`.
