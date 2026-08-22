# Node streamed server sample

This dependency-free development server keeps one `cef-pdf --streamed` worker alive across renders. It starts the worker on the first conversion and restarts it after an idle shutdown or unexpected exit.

## Run it

Use Node.js 18 or newer. Build a `cef-pdf` executable that supports `--streamed`, then run:

```text
node server.js --cef-pdf ../../build/vs17-x64-7680/src/Release/cef-pdf.exe --port 3000 --idle-timeout 60
```

Open `http://127.0.0.1:3000`. The server binds only to `127.0.0.1` unless `--host` changes it.

Options:

- `--cef-pdf PATH` selects the executable. `CEF_PDF_PATH` is the fallback when the option is absent.
- `--host HOST` selects the bind address. The default is `127.0.0.1`.
- `--port PORT` selects the port. Use `0` to let the operating system choose one.
- `--idle-timeout SECONDS` sets worker idle time. The default is 60. Set it to `0` to keep the worker running until the Node server exits.

Paths are resolved relative to this directory. When `--cef-pdf` is omitted on Windows, the example checks the current CEF 7680 x64 build first, then other local Release build locations. The executable must remain beside the matching CEF DLLs and resource files. The example passes `--streamed --javascript` and a unique `--profile` path to the executable.

## Lifecycle and files

The idle clock starts after a conversion finishes. It never stops the worker while requests are active. At expiry, the server sends a framed `quit` request and gives the process three seconds to exit before terminating it. The next conversion starts a new worker.

Generated PDFs and images go under a per-process directory below `build/node-streamed-server/`; the API never accepts an output path from the browser. The cef-pdf worker also receives a unique temporary Chromium profile, so it does not conflict with another running cef-pdf process. Download URLs contain random tokens, work once, and expire after ten minutes. The server deletes downloaded, expired, failed, and shutdown outputs. A requested DOM snapshot also uses a generated path and follows the output's cleanup.

The server clears stale files from its dedicated output directory when it starts. It accepts at most four active conversions and returns HTTP 429 when that limit is full.

`GET /api/status` reports `state`, `pid`, and `generation` so smoke tests can verify reuse and restart behavior. It also reports the idle timeout, active request count, and last activity time.

POST endpoints require `application/json`, a same-origin request, and the CSRF token returned by `GET /api/status` in the `X-CSRF-Token` header. The server does not send permissive CORS headers. The JSON request body limit is 2 MiB. This is a local development example, not an internet-facing service.
