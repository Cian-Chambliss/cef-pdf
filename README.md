# cef-pdf

`cef-pdf` is a command line utility, with an optional embedded HTTP server, for creating PDF documents and PNG, JPEG, or BMP browser screenshots from HTML and SVG content. It uses the [Chromium Embedded Framework (CEF)](https://bitbucket.org/chromiumembedded/cef/overview) for loading, rendering, PDF printing, and image capture.

Image output is a browser screenshot, not a rasterized or paginated PDF. A `full` capture creates one potentially tall image of the document; a `viewport` capture creates one image at the configured viewport size.

### Usage:

    cef-pdf [options] --url=<url>|--file=<path> [output]

    Options:
      --help -h        This help screen.
      --url=<url>      URL to load, may be http, file, data, anything supported by Chromium.
      --file=<path>    File path to load using file:// scheme. May be relative to current directory.
      --stdin          Read content from standard input until EOF (Unix: Ctrl+D, Windows: Ctrl+Z).
      --format=<type>  Output format: pdf, png, jpg, jpeg, or bmp.
      --capture=<mode> Image capture mode: full (default) or viewport.
      --quality=<0-100> JPEG quality. Default is 90.
      --image-background=<color> JPEG/BMP background. Default is white.
      --size=<spec>    Size (format) of the paper: A3, B2.. or custom <width>x<height> in mm.
                       A4 is the default.
      --list-sizes     Show all defined page sizes.
      --landscape      Wheather to print with a landscape page orientation.
                       Default is portrait.
      --margin=<spec>  Paper margins in mm (much like CSS margin but without units)
                       If omitted some default margin is applied.
      --javascript     Enable JavaScript.
      --backgrounds    Print with backgrounds. Default is without.
      --scale=<%>      Scale the output. Default is 100.
      --delay=<ms>     Wait after page load before creating PDF. Default is 0.
      --wait-signal    Wait for JavaScript signal before creating PDF.
      --wait-signal-timeout=<ms> Timeout for wait-signal before printing. Default is 0 (no timeout).
      --savehtml=<path> Save generated DOM HTML before creating PDF.
      --staticonly     Remove <script> tags from saved HTML snapshot.
      --viewwidth=<px> Width of viewport. Image default is 1280.
      --viewheight=<px> Height of viewport. Image default is 720.
      --dump-file-prefix=<path_prefix> (Windows only) Enable unhandled exception dumps.
                       Prefix includes directory and file name prefix.
      --max-dump-files=<n> (Windows only) Max number of dump files to keep. Default is 5.
      --disable-gpu     Disable GPU acceleration and GPU compositing for headless servers.

    Server options:
      --server         Start HTTP server
      --host=<host>    If starting server, specify ip address to bind to.
                       Default is 127.0.0.1
      --port=<port>    Specify server port number. Default is 9288

    Output:
      Output file name. Format is inferred from .pdf, .png, .jpg, .jpeg, or .bmp.
      Standard output defaults to PDF unless --format is supplied.

### Image output

PNG preserves transparent page regions. JPEG and uncompressed 24-bit BMP composite transparency onto `--image-background`, which defaults to white. PDF-only print settings such as paper size, margin, landscape, backgrounds, scale, and headers/footers are rejected for image output.

```powershell
cef-pdf --file=page.html page.png
cef-pdf --file=graphic.svg graphic.png
cef-pdf --quality=85 --url=https://example.com page.jpg
cef-pdf --capture=viewport --viewwidth=1280 --viewheight=720 --url=https://example.com shot.bmp
cef-pdf --format=png --stdin > page.png
```

### Crash dumps (Windows)

On Windows, you can enable dump file generation for unhandled exceptions:

- `--dump-file-prefix=<path_prefix>` enables dump generation.
  The prefix contains both the directory and the file name prefix.
- `--max-dump-files=<n>` controls retention and defaults to `5`.

Dump files are written as `<prefix>_YYYYMMDD_HHMMSS_mmm_<pid>.dmp`.
Before writing a new dump, older matching dumps are deleted to stay within
the configured maximum.

Example:

```powershell
cef-pdf --dump-file-prefix=C:\temp\cef-pdf\crash --max-dump-files=10 --url=https://example.com out.pdf
```

### JavaScript wait signal

When running with `--wait-signal` (and `--javascript`), cef-pdf will wait until the
page calls `window.cefpdf.signalReady()` before printing the PDF. Optionally add
`--wait-signal-timeout=<ms>` to force printing after a timeout.

Use `--savehtml=<path>` to save a snapshot of the generated DOM HTML (for example,
after JavaScript modifies the page) right before the PDF is created.

Add `--staticonly` together with `--savehtml` to strip `<script>` tags from the
saved HTML snapshot. Using `--staticonly` without `--savehtml` returns an error.

When `--savehtml` is enabled, cef-pdf logs snapshot diagnostics to the console,
including request/response status, HTML byte count, and write success/failure.

JavaScript `console.log`, `console.warn`, and `console.error` messages from the page
are also forwarded to the terminal with a `js-console:` prefix.

Examples:

```bash
# Save final DOM snapshot and PDF
cef-pdf --javascript --wait-signal --savehtml=out.html --url=https://example.com out.pdf

# Save static-only snapshot (scripts removed) and PDF
cef-pdf --javascript --wait-signal --savehtml=out.html --staticonly --url=https://example.com out.pdf
```

Example:

```
<script>
  window.addEventListener("load", async () => {
    // Perform async rendering work here
    await fetch("/data").then(r => r.json());
    // Signal readiness for printing
    if (window.cefpdf && typeof window.cefpdf.signalReady === "function") {
      window.cefpdf.signalReady();
    }
  });
</script>
```

### HTTP server usage

Execute `cef-pdf` with `--server` option and visit `localhost:9288` with web browser. Default json response, with status and version number, should indicate the server is up and running on local machine:

    {
        "status": "ok",
        "version": "0.2.0"
    }

POST HTML or SVG to a route ending in `.pdf`, `.png`, `.jpg`, `.jpeg`, or `.bmp`. Responses use `application/pdf`, `image/png`, `image/jpeg`, or `image/bmp` as appropriate. Send SVG bodies with `Content-Type: image/svg+xml`; HTML defaults to `text/html` for backward compatibility.

Image requests accept `Image-Capture: full|viewport`, `Image-Viewport: 1280x720`, `Image-Quality: 0-100` for JPEG, and `Image-Background: #RRGGBB` for JPEG/BMP. PDF headers are rejected on image routes, and image headers are rejected on PDF routes.

In addition to POSTing content inside the request body, special HTTP header `Content-Location` is supported, which should be an URL to some external content. `cef-pdf` will try to grab the content from this URL and use it just like it was the request's body.

### Streamed server usage

Use `--streamed` to keep one cef-pdf process alive while a parent process sends render requests through stdin. Responses are written only to stdout. Normal application and Chromium diagnostics are redirected to stderr so they cannot corrupt the protocol stream.

Requests and responses use UTF-8 JSON with LSP-style framing:

```text
Content-Length: <UTF-8 byte count>\r\n
Content-Type: application/json\r\n
\r\n
{"id":"page-1","command":"render",...}
```

Each render request requires a string or numeric `id`, an input, and an output path and format:

```json
{
  "id": "page-1",
  "command": "render",
  "input": {
    "type": "html",
    "content": "<h1>Hello</h1><svg viewBox=\"0 0 10 10\">...</svg>"
  },
  "output": {
    "path": "output/page.pdf",
    "format": "pdf"
  },
  "options": {
    "size": "A4",
    "backgrounds": true,
    "delay": 100
  }
}
```

Input types are `html`, `svg`, `url`, and `file`. Formats are `pdf`, `png`, `jpeg`, and `bmp`. The `options` object accepts the CLI equivalents in camel case: `size`, `margin`, `landscape`, `backgrounds`, `scale`, `delay`, `waitSignal`, `waitSignalTimeout`, `saveHtml`, `staticOnly`, `viewWidth`, `viewHeight`, `headerFooter`, `headerTitle`, `footerUrl`, `capture`, `quality`, and `imageBackground`.

Requests may finish out of order. IDs and output paths must remain unique while requests are active. Send `{"id":"quit-1","command":"quit"}` to drain accepted renders and shut down cleanly. Closing stdin also drains accepted renders, without a final quit response.

The dependency-free Node.js example in `examples/node-streamed-server/` provides a browser form, worker reuse, idle shutdown, and automatic worker restart. See its README for usage.

### Building

`cef-pdf` should compile without problems with cmake/ninja on Windows (7, x64), Linux (tested on Debian 8.5.0, x64) and Mac OS X (10.11.6) using decent C++11 compiler. In order to build, [CEF build distribution files](http://opensource.spotify.com/cefbuilds/index.html) must be downloaded and placed in some directory, like `/path/to/cef/release` in the example below.

```
$ mkdir ~/build
$ cd ~/build
$ cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release -DCEF_ROOT=/path/to/cef/release /path/to/cef-pdf
$ ninja
```

Windows 64 bit

note: \dev\cef\x64 had the current 64 bit distro download whose folder structure should look similar to this:

```
+cmake
+Debug
+Doxyfile
+include
+libcef_dll
+Release
+Resources
+sample
+tests
cef_paths.gypi
cef_paths2.gypi
LICENSE.txt
CMakeLists.txt
README.md
README.txt
```

```
cmake . -G "Visual Studio 17 2022" -A x64  -DCEF_ROOT=/dev/3rdParty/libcef/cef3/7444.176/src -D_HAS_ITERATOR_LEVEL=0 D=_HAS_ITERATOR_DEBUGGING=0
```

```
cmake . -G "Visual Studio 17 2022" -A Win32  -DCEF_ROOT=/dev/3rdParty/libcef/cef3/7444.176/src -D_HAS_ITERATOR_LEVEL=0 D=_HAS_ITERATOR_DEBUGGING=0
```

### Running headless

libcef has dependencies on X11, and requires an X11 server, so when running headless where an X11 server is not available, you will want to run this under xvfb.

```
xvfb-run cef-pdf --server
```

### License

`cef-pdf` is licensed under the MIT license.
