# Image Output Implementation Plan

## Goal

Add PNG, JPEG, and BMP output without changing the existing PDF behavior. The first release should render local HTML files, local SVG files, and URLs through both the command-line tool and HTTP server, preserve the current load/delay/JavaScript-ready workflow, and produce either a full-page or viewport image.

## Recommended Approach

Use Chromium's DevTools Protocol through CEF to call `Page.getLayoutMetrics` and `Page.captureScreenshot`. Chromium can encode PNG and JPEG directly. For BMP, capture lossless PNG, decode it with `CefImage`, and write a BMP from the resulting BGRA pixels.

This is preferable to the alternatives:

- `CefBrowserHost::PrintToPDF` only produces PDF. It has no raster image output mode.
- Rendering the page into an HTML canvas requires a library such as `html2canvas`; it is not native print output and has cross-origin image, font, CSS, canvas-tainting, and fidelity limitations.
- `CefRenderHandler::OnPaint` can capture the off-screen BGRA viewport and encode it with `CefImage::AddBitmap`/`GetAsPNG`, but reliably capturing a full page requires resizing or stitching and careful synchronization with the final paint.
- Converting the generated PDF to PNG is the best choice only when PNGs must exactly represent individual printed pages. It requires adding a rasterizer such as PDFium and produces one PNG per PDF page.

The DevTools route uses APIs already present in the bundled CEF headers, avoids a new image dependency, supports capture beyond the viewport, and returns encoded PNG or JPEG data directly. The bundled `CefImage` API can decode the intermediate PNG needed for BMP output.

## Output Semantics

The initial feature should be defined as a browser screenshot, not a rasterized PDF:

- `full` capture: one potentially tall image containing the full document content.
- `viewport` capture: one image containing the configured viewport.
- PNG preserves transparency and is the default lossless image format.
- JPEG is lossy, accepts a quality setting from 0 to 100, and does not preserve transparency. Composite transparent content onto an explicit background, white by default.
- BMP should initially be emitted as an uncompressed 24-bit BMP with a white background and no alpha channel. Write headers and padded pixel rows explicitly so behavior is cross-platform.
- Screen CSS is used by default. A later `--media=print` option can call `Emulation.setEmulatedMedia`, but this still will not add PDF pagination, margins, headers, or footers.
- PDF-only settings (`--size`, `--margin`, `--landscape`, `--backgrounds`, `--scale`, and header/footer settings) should be rejected or clearly ignored for image output. Rejecting them is safer because it avoids implying print-equivalent results.

If the required product behavior is instead "one PNG for every printed PDF page," implement PDF-to-image conversion as a separate second feature rather than trying to reproduce print pagination with screenshots.

## Supported Inputs

Image jobs must support the existing Chromium loading paths rather than introducing a canvas-only input path:

- Local HTML: `--file=page.html` or `--file=page.htm`.
- Local SVG: `--file=graphic.svg`, loaded as a standalone `image/svg+xml` document.
- Remote URL: `--url=https://example.com/page`, including URLs that return either `text/html` or `image/svg+xml`.
- HTTP request body: accept both `Content-Type: text/html` and `Content-Type: image/svg+xml`.
- `Content-Location`: continue loading the remote URL and honor the response's media type.

For local files and URLs, let Chromium load the original document directly so relative assets, linked stylesheets, web fonts, scripts, and SVG references resolve using the document's real base URL. Do not read a local SVG and inject it into an HTML canvas or wrapper page.

The internal `cefpdf://` scheme currently always responds with `text/html`. Extend each body-backed job with an input media type and have `SchemeHandlerFactory` return that value. This is required for posted or stdin SVG source to be parsed as an SVG document. Default to `text/html` for backward compatibility; use `image/svg+xml` when supplied by the HTTP `Content-Type` header or an explicit future stdin option.

Standalone SVG sizing rules must be documented and tested:

- SVGs with intrinsic `width` and `height` should retain their natural aspect ratio.
- SVGs with only a `viewBox` should scale to the requested viewport width and height.
- SVGs without intrinsic dimensions or a `viewBox` should use the configured image viewport defaults.
- Transparent SVG regions should remain transparent in PNG output and use the configured background color for JPEG/BMP output.

## Proposed Interface

### Command line

Add:

```text
--format=<pdf|png|jpg|bmp> Explicit output format (`jpeg` is an alias for `jpg`).
--capture=<full|viewport>  Image capture area. Default: full.
--viewwidth=<px>           Layout viewport width for image output.
--viewheight=<px>          Layout viewport height for viewport capture.
--quality=<0-100>          JPEG quality. Default: 90.
--image-background=<color> Background for JPEG/BMP. Default: white.
```

Selection rules:

1. `--format` wins when supplied.
2. Otherwise infer the format from `.png`, `.jpg`, `.jpeg`, `.bmp`, or `.pdf`.
3. Output to stdout without `--format` remains PDF for backward compatibility.
4. Reject an explicit format that conflicts with the output extension.
5. Use image-specific defaults such as 1280x720 without changing the existing PDF viewport defaults.

Examples:

```powershell
cef-pdf --file=page.html page.png
cef-pdf --file=graphic.svg graphic.png
cef-pdf --url=https://example.com/page page.png
cef-pdf --url=https://example.com/graphic.svg graphic.png
cef-pdf --quality=90 --url=https://example.com/page page.jpg
cef-pdf --file=graphic.svg graphic.bmp
cef-pdf --format=png --capture=viewport --viewwidth=1280 --viewheight=720 --url=https://example.com shot.png
cef-pdf --format=png --stdin > page.png
```

### HTTP server

Accept `.png`, `.jpg`, `.jpeg`, and `.bmp` routes alongside `.pdf` routes:

```text
POST /page.png
Content-Type: text/html
Image-Capture: full
Image-Viewport: 1280x720
```

An SVG body uses the same endpoint with `Content-Type: image/svg+xml`.

Return `image/png`, `image/jpeg`, or `image/bmp` and the requested inline filename. Add an `Image-Quality` header for JPEG and an `Image-Background` header for JPEG/BMP. Keep the existing PDF headers limited to PDF jobs. Validate image dimensions and reject unsupported image options with a 4xx response rather than silently falling back.

## Design Changes

### 1. Make output format job-specific

Add an `OutputFormat { PDF, PNG, JPEG, BMP }` field to `Job`, an input media type, and image settings for capture mode, viewport dimensions, JPEG quality, and opaque background color. Job-specific settings are important because server mode can process multiple requests concurrently; the current viewport and readiness settings on `Client` are global.

Also generalize status names where practical:

- `PRINTING` to `RENDERING`
- `PRINT_ERROR` to `OUTPUT_ERROR`

Temporary files must receive the correct extension. Change `reserveTempFile()` to accept an extension or add `reserveTempFile(OutputFormat)`.

### 2. Introduce an output renderer boundary

Keep loading and readiness in `Client`, then have `Manager::Process` choose the output implementation:

- PDF: retain the existing `Printer` and `PrintToPDF` callback.
- PNG/JPEG/BMP: start a new `Screenshotter` object.

`Screenshotter` should be ref-counted and retain:

- the browser and manager references;
- the `CefRegistration` returned by `AddDevToolsMessageObserver`;
- request IDs for each DevTools operation;
- the destination path and capture settings;
- a completed flag so timeout, callback, and browser-close paths cannot resolve a job twice.

### 3. Capture and save the image

For full-page capture:

1. Register a `CefDevToolsMessageObserver` before sending commands.
2. Call `Page.getLayoutMetrics` with `ExecuteDevToolsMethod`.
3. Read `cssContentSize.width` and `cssContentSize.height` from the result.
4. Validate dimensions and total pixel count against configured limits.
5. For JPEG/BMP, set the requested opaque background with `Emulation.setDefaultBackgroundColorOverride`. Call `Page.captureScreenshot` with `format: "png"` or `format: "jpeg"`, `fromSurface: true`, `captureBeyondViewport: true`, and a clip covering the content size. Include JPEG quality when applicable. Use PNG as the intermediate format for BMP.
6. Parse the returned JSON with `CefParseJSON`.
7. Decode the `data` field with `CefBase64Decode`.
8. For PNG/JPEG, write the decoded binary value directly. For BMP, pass the PNG bytes to `CefImage::AddPNG`, request BGRA pixels with `GetAsBitmap`, composite alpha onto the configured background, convert to bottom-up BGR rows with 4-byte padding, and write valid `BITMAPFILEHEADER`/`BITMAPINFOHEADER` fields in little-endian order.
9. Call `Manager::Finish` and release the DevTools observer registration.

For viewport capture, skip layout metrics and capture the current viewport. Ensure the browser has received the requested size and a paint/layout cycle before capture; call `WasResized()` after applying job-specific dimensions if necessary.

All CEF browser and DevTools calls must remain on `TID_UI`. File writing may be moved to a worker thread for large captures, but completion must be posted back to the UI thread before closing the browser.

### 4. Preserve readiness behavior

Both formats should pass through the same sequence:

```text
load -> optional delay -> optional JavaScript signal -> optional DOM snapshot -> render output
```

Route every successful path through `Client::Process`; the current immediate-load path calls `Manager::Process` directly and bypasses `--savehtml`. Fixing that inconsistency while adding image output prevents format-specific readiness bugs.

Log messages should say "rendering output" rather than "generating PDF" when the format is not known at that layer.

### 5. Extend server routing and responses

In `Session`:

- replace the PDF-only route regex with parsing that accepts `.pdf`, `.png`, `.jpg`, `.jpeg`, and `.bmp`;
- rename `HandlePDF` to a format-neutral handler;
- set the job format from the route extension;
- parse and validate image request headers;
- choose `application/pdf`, `image/png`, `image/jpeg`, or `image/bmp` in `OnResolve`;
- preserve the requested filename in `Content-Disposition`.

Avoid storing request-specific image settings on `Client`, because concurrent sessions would overwrite one another.

## Files Expected to Change

- `src/main.cpp`: format inference, image options, validation, help text, and format-neutral result messages.
- `src/Job/Job.h` and `src/Job/Job.cpp`: output format and image capture/encoding settings.
- `src/Job/Manager.h` and `src/Job/Manager.cpp`: renderer selection, format-aware temporary paths, and generic completion statuses.
- `src/Job/Printer.h`: retain PDF implementation behind the renderer boundary.
- `src/Job/Screenshotter.h` and `src/Job/Screenshotter.cpp`: DevTools capture flow, PNG/JPEG results, BMP conversion, base64 decoding, and binary file output.
- `src/Client.h` and `src/Client.cpp`: shared readiness path and job-specific viewport application.
- `src/RenderHandler.h` and `src/RenderHandler.cpp`: expose or resolve per-browser viewport dimensions if required; `OnPaint` remains a fallback rather than the primary capture path.
- `src/SchemeHandlerFactory.h` and `src/SchemeHandlerFactory.cpp`: return each body-backed job's `text/html` or `image/svg+xml` media type instead of always returning `text/html`.
- `src/Common.h` and `src/Common.cpp`: format-aware temporary files and binary writing helper.
- `src/Server/Http.h`, `src/Server/Session.h`, and `src/Server/Session.cpp`: image routes, headers, validation, MIME types, and response filenames.
- `README.md` and `CHANGELOG.md`: document behavior, examples, and the screenshot-versus-print distinction.

`src/CMakeLists.txt` already discovers `.cpp` files recursively, so adding `Screenshotter.cpp` should not require a source-list update.

## Delivery Phases

### Phase 1: CEF capture spike

- Hard-code one post-load `Page.captureScreenshot` call against a simple local HTML page.
- Confirm result parsing and `CefBase64Decode` with the bundled CEF version.
- Verify viewport and full-page captures on Windows and determine practical maximum dimensions.
- Confirm that local fonts, remote images, transparency, and JavaScript-rendered content appear after the existing readiness controls.

### Phase 2: CLI MVP

- Add image output formats and job settings.
- Preserve direct Chromium loading for local HTML files, local SVG files, and URLs.
- Propagate body input media types through the internal scheme handler.
- Add `Screenshotter` and manager dispatch.
- Add format-aware output paths and stdout support.
- Add CLI format inference, options, validation, and help.
- Preserve all existing PDF command lines unchanged.

### Phase 3: HTTP support

- Accept PNG, JPEG, and BMP endpoints plus image-specific headers.
- Make viewport/capture settings job-local and concurrency-safe.
- Return the correct MIME type and disposition.
- Add request validation and useful HTTP errors.

### Phase 4: Hardening and documentation

- Add capture timeout and exactly-once completion handling.
- Enforce width, height, and total-pixel limits to prevent excessive memory use.
- Handle navigation, renderer termination, DevTools detach, malformed results, decode failures, and write failures.
- Update README examples and release notes.
- Test all supported operating systems before claiming cross-platform support.

## Test Plan

Add small local HTML and SVG fixtures covering:

- fixed-size viewport capture;
- a page taller than the viewport for full-page capture;
- CSS backgrounds, web fonts, SVG, transparency, and remote images;
- standalone SVG with explicit dimensions, `viewBox` only, external assets, and transparent regions;
- JPEG quality and white/custom background compositing;
- BMP signature, headers, dimensions, row order, row padding, colors, and file size;
- delayed JavaScript rendering with `--delay`;
- `window.cefpdf.signalReady()` and timeout behavior;
- stdin, local HTML file, local SVG file, HTML URL, SVG URL, stdout, and explicit output paths;
- HTML and SVG HTTP bodies plus HTML and SVG `Content-Location` URLs;
- simultaneous PDF, PNG, JPEG, and BMP HTTP jobs with different dimensions;
- invalid format, conflicting extension, invalid dimensions, oversized page, capture timeout, and output write failure.

Validation should include checking each image signature, decoded dimensions, expected representative pixels, JPEG quality behavior, BMP header/file consistency, MIME type, exit status, and cleanup of temporary files. Existing PDF smoke tests should run unchanged to catch regressions.

## Acceptance Criteria

- Existing PDF commands and HTTP `.pdf` requests behave as before.
- CLI and HTTP inputs can produce valid PNG, JPEG, and BMP files.
- Local HTML files, local SVG files, HTML URLs, and SVG URLs all render without conversion to an intermediate canvas.
- HTTP bodies labeled `text/html` or `image/svg+xml` are parsed using the correct document type.
- Full capture includes content below the initial viewport in every image format.
- Viewport capture has the requested dimensions.
- Delay, ready signal, and DOM snapshot behavior are shared by PDF and all image formats.
- Concurrent server jobs do not share format or viewport settings.
- Failures return a nonzero CLI status or HTTP error and do not leave unresolved browsers or temporary files.
- Documentation clearly states that image output is a screenshot and is not a paginated PDF rendering.

## Optional Follow-up: Printed Page Images

If exact print layout is required, retain `PrintToPDF`, then rasterize each PDF page with PDFium and encode it in the selected image format. Add an output naming rule such as `report-001.png`, `report-002.png`, and define how multi-page output works over stdout and HTTP (for example, ZIP or multipart). This is a separate feature because it changes the one-input/one-output contract and adds a substantial dependency.
