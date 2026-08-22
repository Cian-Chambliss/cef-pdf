# Streamed server implementation plan

## Implementation

1. Add `--streamed` as a third execution mode in `src/main.cpp`. Reject combinations with `--server`, `--stdin`, and one-shot input options.
2. Add a stream controller under `src/Stream/`. It will use separate reader and writer threads while CEF keeps its main-thread message loop.
3. Use LSP-style framing:

   ```text
   Content-Length: 142\r\n
   Content-Type: application/json\r\n
   \r\n
   {"id":"pdf-1",...}
   ```

4. Reserve stdout for protocol traffic. Duplicate its original handle, redirect normal stdout to stderr before CEF starts, and write frames only through the duplicate. Mark it non-inheritable so Chromium subprocesses cannot retain or write to it.
5. Parse and serialize JSON with CEF's existing `CefParseJSON` and `CefWriteJSON` APIs.

## Proposed requests

```json
{
  "id": "pdf-1",
  "command": "render",
  "input": {
    "type": "html",
    "content": "<html>...</html>"
  },
  "output": {
    "path": "build/streamed-smoke/page.pdf",
    "format": "pdf"
  },
  "options": {
    "size": "A4",
    "backgrounds": true
  }
}
```

- Input types: `html`, `svg`, `url`, and `file`.
- Output path is required. Binary output never shares stdout with protocol responses.
- Options map to existing PDF, image, viewport, delay, signal, and snapshot settings.
- Every request requires a string or numeric `id`.
- Multiple renders may run concurrently, so responses can arrive out of order.

Terminal response:

```json
{
  "id": "pdf-1",
  "status": "success",
  "output": {
    "path": "build/streamed-smoke/page.pdf",
    "format": "pdf",
    "mediaType": "application/pdf"
  }
}
```

Errors include stable codes and messages, such as `invalid_request`, `load_error`, `http_error`, `output_error`, and `aborted`.

Shutdown request:

```json
{"id":"quit-1","command":"quit"}
```

`quit` stops accepting requests, drains accepted jobs, emits its response last, flushes stdout, and shuts down CEF. EOF performs the same drain without a quit response. Invalid framing is fatal because the stream cannot be safely resynchronized.

## CEF integration

- Convert valid render packets into existing `job::Local` or `job::Remote` objects.
- Submit jobs using `CefPostTask(TID_UI, ...)`, matching the HTTP server at `src/Server/Session.cpp:539`.
- Keep callbacks short. They enqueue response data rather than performing stdout I/O while `Manager::Resolve` holds its mutex at `src/Job/Manager.cpp:132`.
- Add an idle shutdown path to `Client` so CEF exits only after queued jobs finish and browsers close.

## Sample Node.js application

Add a runnable example under `examples/node-streamed-server/`. Keep it dependency-free by using only Node.js built-ins: `http`, `child_process`, `fs`, `path`, `crypto`, and `url`.

Suggested files:

- `server.js` starts the local web server, owns the cef-pdf client, manages temporary output files, and implements the idle timer.
- `cef-pdf-client.js` starts `cef-pdf --streamed --javascript`, writes Content-Length frames, incrementally parses response frames, and matches out-of-order responses to request IDs.
- `public/index.html` contains the conversion form and result area.
- `public/app.js` submits conversions, downloads successful output, and polls or refreshes process status.
- `public/style.css` provides a usable desktop and mobile layout without a frontend framework.
- `README.md` documents `node server.js`, executable path selection, port selection, and the idle timeout behavior.

Run the example with a command such as:

```text
node server.js --cef-pdf ../../build/src/Release/cef-pdf.exe --port 3000 --idle-timeout 60
```

The browser page should provide:

- A large HTML textarea that accepts complete documents, fragments, inline CSS, and inline SVG.
- Output format controls for PDF, PNG, JPEG, and BMP.
- Common controls for delay, viewport width and height, JavaScript readiness signal, signal timeout, saved HTML path, and static-only snapshots.
- PDF controls for page size, custom dimensions, margins, orientation, backgrounds, scale, header/footer enablement, header title, and footer URL.
- Image controls for full or viewport capture, width, height, JPEG quality, and JPEG/BMP background color.
- An idle timeout control in seconds. A value of `0` keeps cef-pdf running until the Node.js server exits.
- A process-status display with `starting`, `running`, `stopping`, `stopped after inactivity`, and `failed` states.
- A conversion-status area and a download link or inline preview after a successful request.

The example server should expose a small local API:

- `GET /` and static asset routes serve the page.
- `GET /api/status` returns the cef-pdf process state, configured timeout, active request count, and last activity time.
- `POST /api/settings` updates the idle timeout and rearms or cancels the timer.
- `POST /api/convert` accepts the form as JSON, allocates a random output name under `build/node-streamed-server/`, starts cef-pdf if needed, sends one render packet, and waits for its matching response.
- `GET /api/output/<token>` returns a completed file once and then deletes it.

The Node.js wrapper should treat cef-pdf as a restartable worker:

1. Start it lazily for the first conversion rather than when the web server starts.
2. Keep one child process alive across conversions and allow multiple pending IDs.
3. Reset activity when a conversion request is accepted. Never apply the idle timeout while conversions are pending.
4. Once the configured idle period expires, send a framed `quit` request and wait for its response and process exit.
5. If graceful shutdown exceeds a short fixed grace period, terminate the child and reject any remaining requests.
6. Set the state to `stopped after inactivity`. The status response and web page should explain that the next conversion will start a new cef-pdf process.
7. On the next conversion, start a fresh child automatically, report `starting`, and continue once its pipes are ready.
8. Reject all pending promises if cef-pdf exits unexpectedly. A later conversion may start a new process.
9. On Node.js shutdown, send `quit`, wait briefly, and then terminate the child if needed.

The sample should generate output paths itself rather than accept browser-provided paths. Limit request body size, bind to `127.0.0.1` by default, escape errors shown in the page, and delete abandoned output files. This remains a development example rather than a production service.

## Tests and documentation

- Add tracked fixtures under `tests/fixtures/`, including HTML with inline SVG.
- Add `tests/windows/smoke-streamed.bat`.
- The batch script uses PowerShell for byte-accurate framing, sends PDF, PNG, JPEG, and quit packets through one process, then validates response IDs, file signatures, and image dimensions.
- Add a Node.js smoke test that starts the sample server with a short idle timeout, performs two conversions through one cef-pdf process, observes idle shutdown, and verifies that a third conversion starts a new process.
- Generated files go under the ignored `build/streamed-smoke/` directory.
- Update `README.md`, CLI help, and `CHANGELOG.md`.
- Build with `cmake --build . --config Release --target cef-pdf`, then run the batch smoke test.
