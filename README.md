# tabler-server

## About

A **minimal**, Linux-only server for hosting [`tabler`](https://github.com/pachadotdev/tabler)
R applications - the tabler equivalent of Shiny Server, but deliberately tiny.

It is a small C++ reverse proxy + process supervisor. It does **one** thing:

- serves every app found under an apps directory (default `/srv/tabler-server/apps`)
  at a matching URL path, and
- gives every browser session its own private R worker process that is reaped
  after inactivity.

Everything else (TLS, gzip, auth, rate limiting, HTTP/2, ...) is intentionally
**out of scope** and is meant to be handled by a front proxy such as **nginx**
+ **Let's Encrypt** in front of tabler-server.

```
            +------------------- Linux host ----------------------+
Internet -> | nginx (TLS, :443) -> tabler-server (C++, :3000)     |
            |                       |                             |
            |                       |- R worker (app1, session A) |
            |                       |- R worker (app1, session B) |
            |                       |- R worker (app2, session C) |
            +-----------------------------------------------------+
```

## Concept

1. **Make / CMake based - plain C++17, no heavy dependencies**
2. **C++ does the proxying/supervision**
3. **R runs the apps**
4. **No Python support**
5. **Linux only - managed with `systemctl`**

## How apps are served

Copy each app into its own directory under the apps directory:

```
/srv/tabler-server/apps/
|- example1/
|   |- app.R
|- example2/
|   |- app.R
```

They become available at:

```
http://localhost:3000/example1
http://localhost:3000/example2
```

Visiting `http://localhost:3000/` lists the available apps.

### App layout

Each app is a directory containing an `app.R`. Two conventions are accepted:

```r
# The usual tabler app; the final tablerApp() call is intercepted
# by the server, which re-launches it on a server-assigned port.
library(tabler)

ui <- page(title = "Example 1", body = body("Hello from example1"))
server <- function(input, output, session) {}

tablerApp(ui, server)
```

```r
# Define `ui` and `server`; no tablerApp() call needed.
library(tabler)
ui <- page(...)
server <- function(input, output, session) { ... }
```

You do **not** choose the port in the app - tabler-server assigns a private
loopback port to each worker.

## Sessions

- **One R worker per browser session.** A session is created the first time a
  browser loads an app page and is tracked with a `tabler_sid` cookie.
- **Reload = fresh session.** Reloading the page mints a new session and worker;
  the old worker becomes idle and is reaped.
- **Idle reaping.** A worker with no active WebSocket connection is killed after
  `disconnect_grace` seconds (default 15s, covers a closed tab). Any worker is
  killed after `idle_timeout` seconds of inactivity (default 300s = 5 min).

## Per-tab isolation behaviour

Cookies are shared across tabs of the same browser, so two tabs of the *same*
app currently share one worker/session. The proxy is structured so true per-tab
isolation can be added later with a one-line change to `tabler-reactive.js`
(append a `sessionStorage` token to the `/ws` URL). See
[docs/DESIGN.md](docs/DESIGN.md).

## Requirements

- Linux, `g++` (C++17) or `clang++`, `make`, optionally `cmake`.
- `R` with the `tabler` package installed **system-wide** (so workers can
  `library(tabler)`).

## Build

Using Make:

```bash
make -j4
```

or CMake:

```bash
cmake -S . -B build
cmake --build build
```

The binary is produced at `build/tabler-server` (CMake) or `./tabler-server` (Make).

## Run (development)

```bash
./tabler-server --config config/tabler-server.conf
```

## Install as a service

Either

```bash
sudo ./scripts/install.sh
sudo systemctl enable --now tabler-server
sudo systemctl status tabler-server
```

or

```bash
sudo make install
sudo systemctl enable --now tabler-server
sudo systemctl status tabler-server
```

Then drop apps into `/srv/tabler-server/apps/<name>/app.R`.

## TLS with nginx + Let's Encrypt

tabler-server speaks plain HTTP on `127.0.0.1:3000`. Put nginx in front:

```bash
sudo cp config/nginx/tabler-server.conf /etc/nginx/sites-available/tabler-server
sudo ln -s /etc/nginx/sites-available/tabler-server /etc/nginx/sites-enabled/
sudo certbot --nginx -d apps.example.com
sudo systemctl reload nginx
```

The provided nginx config already forwards WebSocket upgrade headers, which the
tabler reactive bridge needs.

## Configuration

See [config/tabler-server.conf](config/tabler-server.conf). Key = value, one per
line, `#` for comments.

| Key                 | Default                     | Meaning                                        |
| ------------------- | --------------------------- | ---------------------------------------------- |
| `listen`            | `127.0.0.1`                 | Bind address                                   |
| `port`              | `3000`                      | Bind port                                      |
| `apps_dir`          | `/srv/tabler-server/apps`   | Where app directories live                     |
| `rscript`           | `Rscript`                   | Rscript executable used to launch workers      |
| `worker_script`     | `share/worker.R`            | R bootstrap that launches a tabler app         |
| `run_as`            | *(unset)*                   | Drop privileges to this user for workers       |
| `worker_port_base`  | `34000`                     | First loopback port handed to workers          |
| `worker_port_count` | `256`                       | Size of the worker port range                  |
| `idle_timeout`      | `300`                       | Kill a worker after N seconds idle             |
| `disconnect_grace`  | `15`                        | Kill a worker N seconds after its WS closes    |
| `worker_start_timeout` | `20`                     | Seconds to wait for a worker to accept traffic |
| `log_dir`           | `/var/log/tabler-server`    | Worker stderr logs                             |

## Differences with Shiny Server

Shiny Server Open Source lacks enterprise features like built-in user authentication, role-based access control, and automated scaling for heavy traffic. It can be expanded by using Shiny Server Pro (discontinued) or [Posit Connect](). However, its open source edition uses the AGPL software license, which makes it unsuitable for commercial applications.

Tabler Server, and the tabler R package, allow to create interactive dashboard with a similar syntax to Shiny apps in R and both are released under the Apache License. This means that Tabler Server is not a commercial product, it can be used for a wide range of projects including commercial uses, and it is released "as is".

The Apache License 2.0 permits commercial use, modification, distribution, and private use, while prohibiting holding authors liable and the unauthorized use of trademarks. It requires including the original copyright, patent notices, and license text in any distribution. See more on [tl;drLegal](https://www.tldrlegal.com/license/apache-license-2-0-apache-2-0).

## Contributing

See the [Code of conduct](https://github.com/pachadotdev/tabler-server/blob/main/.github/CODE_OF_CONDUCT.md)

## License

Apache-2.0 (matching the `tabler` R package).
