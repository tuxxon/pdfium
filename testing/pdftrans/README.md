# pdftrans source-stream preservation checks (host only)

Independent host checks for `CPDF_RemoveOwnedTextPreservingStreams()`
(`core/fpdfapi/edit/cpdf_sourcecontentremover.*`). Nothing here is shipped or
linked into an Android package.

- `source_preservation_audit.cc`: `pdftrans_source_preservation_audit`.
  `--list` prints every top-level page object (raw text from char codes, mode,
  font, bounds, matrix, content stream). The audit mode removes the selected
  top-level TEXT objects from a fresh load, saves a full copy
  (`FPDF_SaveAsCopy(..., 0)`, no `GenerateContent`), closes, reopens and
  compares against oracles computed from fresh loads of the original:
  - page count and each page's effective MediaBox/CropBox/Rotate/UserUnit;
  - every page's top-level object signature sequence, minus the selection;
  - every non-target page raster, exactly;
  - the target raster outside the selected objects' **actual paint mask**: the
    original page rendered with only the selected objects active on a
    transparent bitmap (alpha != 0). No bounding boxes, padding or tolerance;
  - the target raster against the original rendered with the selected objects
    deactivated, exactly;
  - decoded content streams: same count and length, and every changed byte is
    a blank.
  A rejected removal must leave `/Contents` unchanged and save/reopen to the
  same rasters and objects. The input file's SHA-256 is rechecked on exit.
  `changedOutsideBounds*` are diagnostics for the historical bbox audit only.
- `make_synthetic_matrix.py`: deterministic stdlib-only synthetic PDFs plus
  `manifest.json` (Tj/TJ, kerning, Tc/Tw, moves, escapes, multiple/shared
  streams, rotation/CropBox/UserUnit/inheritance, Form, clip, CMYK, images,
  numbers, vertical text, and the expected rejections).
- `run_preservation_matrix.py`: runs the manifest and real fixtures and writes
  `ledger.json`. Real-fixture selection policy: upright mode-0 non-Type3 TEXT
  with letters and no digits is selectable; numeric-only and rotated TEXT is
  protected; TEXT mixing letters and digits stays unsupported (kept). Every
  fixture remains in the denominator.

## Candidate build recipe

The app's `native/pdfium/build_android.py` applies patches 0001/0002 to the
pinned upstream commit and verifies its source lock. This fork branch already
contains 0001-0003 plus task-28 changes, so it is built directly instead:

```
git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git
git clone -b feature/task-28-source-stream-preservation \
    https://github.com/tuxxon/pdfium pdfium
cat > .gclient <<'EOF'
solutions = [{"name": "pdfium",
  "url": "https://pdfium.googlesource.com/pdfium.git", "managed": False,
  "custom_deps": {},
  "custom_vars": {"checkout_configuration": "minimal",
                  "checkout_android": True}}]
target_os = ["android"]
EOF
# vpython may be unable to fetch wheels behind a proxy; gclient.py runs under
# the system python3 with VPYTHON_BYPASS set.
export PATH=$PWD/depot_tools:$PATH DEPOT_TOOLS_UPDATE=0 \
    VPYTHON_BYPASS="manually managed python not supported by chrome operations"
python3 depot_tools/gclient.py sync --no-history --shallow -j4 --nohooks
python3 depot_tools/gclient.py runhooks
```

Host (unit tests and audit), `out/Host/args.gn`:

```
is_debug = false
is_component_build = false
dcheck_always_on = true
pdf_is_standalone = true
pdf_enable_v8 = false
pdf_enable_xfa = false
pdf_use_skia = false
pdf_enable_fontations = false
use_remoteexec = false
use_siso = false
treat_warnings_as_errors = true
symbol_level = 0
```

```
buildtools/linux64/gn gen out/Host
ninja -C out/Host pdfium_unittests pdftrans_source_preservation_audit
out/Host/pdfium_unittests
python3 -I testing/pdftrans/make_synthetic_matrix.py --out-dir /tmp/synth
python3 -I testing/pdftrans/run_preservation_matrix.py \
    --tool out/Host/pdftrans_source_preservation_audit \
    --manifest /tmp/synth/manifest.json \
    --fixture spike/resnet.pdf ... --singles --out-dir /tmp/run
```

Android engine archives (same GN args as `build_android.py`), for
`target_cpu` in `arm64` and `x64`:

```
buildtools/linux64/gn gen out/pdftrans-android-$cpu --args='is_debug=false
  is_component_build=false pdf_is_complete_lib=true pdf_is_standalone=false
  pdf_enable_v8=false pdf_enable_xfa=false pdf_use_skia=false
  pdf_enable_fontations=false clang_use_chrome_plugins=false
  use_remoteexec=false use_siso=false target_os="android"
  target_cpu="'$cpu'" default_min_sdk_version=28 android_ndk_api_level=28'
ninja -C out/pdftrans-android-$cpu pdfium   # obj/libpdfium.a
```

These archives are candidates only. They do not replace the app's release
SDK and say nothing about JNI packaging, APK or device behaviour.
