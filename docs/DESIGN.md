# Design notes

tabler-server is a byte-splicing HTTP/WebSocket reverse proxy plus an R process
supervisor. It is intentionally the smallest thing that can host multiple
tabler apps with isolated sessions.

## Why a byte-splicing proxy?

A tabler app (see `tabler::tablerApp`) runs its own `httpuv` server that:

- serves the page at `/`,
- opens a WebSocket at **`/ws`**,
- serves static assets at **absolute** paths (`/tabler-1.4.0/...`,
  `/tabler-icons-3.55.0/...`, `/js/...`),
- serves render artifacts at `/plots/<id>` and `/widgets/<id>`.

Because every URL except the page itself is absolute (rooted at `/`), we cannot
route by path prefix alone — a request for `/ws` or `/tabler-1.4.0/tabler.min.js`
carries no app name. Two mechanisms solve this:

1. **The page path carries the app name.** `GET /example1` (or `/example1/`) is
   the only request that names the app. On that request the proxy:
   - mints a session id,
   - sets a `tabler_sid` cookie (`Path=/`),
   - rewrites the request line to `GET /` before forwarding to the worker.

2. **Everything else routes by the `tabler_sid` cookie** to that session's
   worker.

After the request head is forwarded, the proxy simply splices bytes in both
directions with `poll(2)`. This is transport-agnostic: it forwards HTTP
responses and, after a `101 Switching Protocols`, the raw WebSocket frames —
no protocol-specific code required.

## Components

| File                 | Responsibility                                             |
| -------------------- | ---------------------------------------------------------- |
| `src/config.*`       | Parse the `key value` config file.                         |
| `src/http.*`         | Read + parse an HTTP message head from a socket.           |
| `src/worker.*`       | `Worker` (one R process) and `WorkerManager` (spawn/reap). |
| `src/proxy.*`        | Accept loop + per-connection routing and splicing.         |
| `src/main.cpp`       | Wire everything together, handle signals.                  |
| `R/worker.R`         | Load `tabler`, source the app's `app.R`, run on our port.  |

## Session lifecycle

```
GET /example1 -|-> new sid, Set-Cookie: tabler_sid=<sid>
               |-> spawn Worker(example1) on 127.0.0.1:<wport>
GET /tabler-1.4.0/... (cookie sid) -> same worker
GET /ws (Upgrade, cookie sid) -> same worker; marks session "connected"
... WebSocket frames splice both ways ...
WS closes -> session "disconnected", disconnect timer starts
reaper thread:
   kill worker if (disconnected AND idle > disconnect_grace)
                or (idle > idle_timeout)
```

## Privilege model

If started as root with `run_as <user>` set, each worker process calls
`setgid`/`setuid` to that user before `exec`ing `Rscript`. The proxy itself can
keep listening on a privileged port and supervise. Running behind nginx you can
instead run the whole service as an unprivileged user and leave `run_as` unset.

## Deliberate non-goals

- No TLS (use nginx).
- No auth (use nginx / an auth proxy).
- No Windows/macOS support.
- No Python apps.
- No multi-host clustering, no load balancing across machines.
- No shared multi-session single-process model (each session is its own
  process, which keeps app code trivially isolated).

## Future: true per-tab isolation

Cookies are per-browser, so two tabs of the same app share a session today. To
isolate per tab without server changes, add a token to the WebSocket URL in
`tabler/inst/js/tabler-reactive.js`:

```js
// per-tab token kept in sessionStorage (unique per tab)
var tok = sessionStorage.getItem("tabler-tab");
if (!tok) { tok = Math.random().toString(36).slice(2); sessionStorage.setItem("tabler-tab", tok); }
var wsUrl = ... + location.host + "/ws?tab=" + tok;
```

The proxy would then key sessions on `(sid, tab)` and spawn a worker per tab.
Plot/widget URLs would need the same token for correct routing.
