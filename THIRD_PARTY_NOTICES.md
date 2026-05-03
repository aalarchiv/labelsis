# Third-Party Notices

This file lists third-party software bundled with the pt700 firmware
binary or with the source distribution, together with the license
text required by each upstream.

---

## ESP-IDF

- Upstream: https://github.com/espressif/esp-idf
- Pinned version: 5.5.2 (see `dependencies.lock`)
- License: Apache-2.0
- Copyright: (c) Espressif Systems (Shanghai) CO LTD and contributors
- Linked against the firmware; not redistributed in source form.

Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0

---

## espressif/mdns

- Upstream: https://components.espressif.com/components/espressif/mdns
- Pinned version: 1.11.1 (see `dependencies.lock`)
- License: Apache-2.0
- Copyright: (c) Espressif Systems (Shanghai) CO LTD
- Pulled by the IDF Component Manager into `managed_components/` at
  build time; full LICENSE retained at
  `managed_components/espressif__mdns/LICENSE`.

Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0

---

## Material Icons (filled)

- Upstream: https://github.com/marella/material-icons
  (repackaging of Google's Material Icons font + metadata)
- Pinned upstream version: 1.13.14
- Bundled files:
  - `components/pt_app/spa/fonts/material-icons.woff2` (font, filled
    variant; vendored verbatim from upstream `iconfont/material-icons.woff2`)
  - `components/pt_app/spa/fonts/material-icons-codepoints.json`
    (name -> hex codepoint table; vendored verbatim from upstream
    `_data/codepoints.json`. The build pipeline re-emits it as
    `material-icons.json` with hex strings converted to decimal
    integers so the SPA can call `String.fromCodePoint(cp)` directly.)
- License: Apache-2.0
- Copyright: (c) Google Inc.

Apache License 2.0: https://www.apache.org/licenses/LICENSE-2.0

---

## qrcodejs (davidshimjs/qrcodejs)

- Upstream: https://github.com/davidshimjs/qrcodejs
- Bundled file: `components/pt_app/spa/qrcode.min.js` (vendored unmodified)
- License: MIT
- Copyright: (c) 2012 davidshimjs

```
The MIT License (MIT)

Copyright (c) 2012 davidshimjs

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

---

## niimblue

- Upstream: https://github.com/MultiMote/niimblue
- Use: algorithm ports (no verbatim files vendored). Specifically the
  Atkinson / Floyd-Steinberg / threshold dithering, the canvas
  pre-process pipeline, and the EAN/Code128 barcode encoders. See
  `components/pt_app/spa/index.html` source comments at the ported
  function definitions for line-level attribution.
- License: MIT
- Copyright: (c) 2024 MultiMote

```
MIT License

Copyright (c) 2024 MultiMote

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
