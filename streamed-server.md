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

## Tests and documentation

- Add tracked fixtures under `tests/fixtures/`, including HTML with inline SVG.
- Add `tests/windows/smoke-streamed.bat`.
- The batch script uses PowerShell for byte-accurate framing, sends PDF, PNG, JPEG, and quit packets through one process, then validates response IDs, file signatures, and image dimensions.
- Generated files go under the ignored `build/streamed-smoke/` directory.
- Update `README.md`, CLI help, and `CHANGELOG.md`.
- Build with `cmake --build . --config Release --target cef-pdf`, then run the batch smoke test.
