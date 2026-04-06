# cef-pdf

`cef-pdf` is a command line utility (with embedded HTTP server as an optional mode) for creating PDF documents from HTML content. It uses Google Chrome browser's [Chromium Embedded Framework (CEF)](https://bitbucket.org/chromiumembedded/cef/overview) library for all it's internal work; loading urls, rendering HTML & CSS pages and PDF printing, therefore, it produces perfect, accurate, excellent quality PDF documents.

### Usage:

    cef-pdf [options] --url=<url>|--file=<path> [output]

    Options:
      --help -h        This help screen.
      --url=<url>      URL to load, may be http, file, data, anything supported by Chromium.
      --file=<path>    File path to load using file:// scheme. May be relative to current directory.
      --stdin          Read content from standard input until EOF (Unix: Ctrl+D, Windows: Ctrl+Z).
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
      --viewwidth=<px> Width of viewport. Default is 128.
      --viewheight=<px> Height of viewport. Default is 128.
      --dump-file-prefix=<path_prefix> (Windows only) Enable unhandled exception dumps.
                       Prefix includes directory and file name prefix.
      --max-dump-files=<n> (Windows only) Max number of dump files to keep. Default is 5.

    Server options:
      --server         Start HTTP server
      --host=<host>    If starting server, specify ip address to bind to.
                       Default is 127.0.0.1
      --port=<port>    Specify server port number. Default is 9288

    Output:
      PDF file name to create. Default is to write binary data to standard output.

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

To receive a PDF, just make POST request to `localhost:9288/foo.pdf`with some HTML content as the request body. `foo` may be any name you choose, `.pdf` suffix is always required. The response will contain the PDF data, with `application/pdf` as the content type.

In addition to POSTing content inside the request body, special HTTP header `Content-Location` is supported, which should be an URL to some external content. `cef-pdf` will try to grab the content from this URL and use it just like it was the request's body.

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
