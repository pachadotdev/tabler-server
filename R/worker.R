# worker.R --------------------------------------------------------------------
# Bootstrap run by tabler-server for each app session.
#
# tabler-server sets two environment variables and then execs:
#     Rscript --vanilla worker.R
#
#   TABLER_WORKER_PORT  loopback port this worker must listen on
#   TABLER_APP_DIR      directory containing the app's app.R
#
# The app's app.R may either call tablerApp(ui, server) directly (the usual
# style) or simply define `ui` and `server`. In the first case we intercept the
# tablerApp() call so the app does not bind its own default port; we then launch
# it on the server-assigned port.
# -----------------------------------------------------------------------------

local({
  port <- suppressWarnings(as.integer(Sys.getenv("TABLER_WORKER_PORT", "0")))
  app_dir <- Sys.getenv("TABLER_APP_DIR", "")

  if (is.na(port) || port <= 0) {
    stop("TABLER_WORKER_PORT is not set to a valid port")
  }
  if (!nzchar(app_dir) || !dir.exists(app_dir)) {
    stop("TABLER_APP_DIR is not a valid directory: ", app_dir)
  }

  app_file <- file.path(app_dir, "app.R")
  if (!file.exists(app_file)) {
    stop("app.R not found in ", app_dir)
  }

  setwd(app_dir)
  suppressPackageStartupMessages(library(tabler))

  # Source the app in its own environment. Override tablerApp so that, if the
  # app calls it, we merely capture (ui, server) instead of starting a server.
  captured <- NULL
  app_env <- new.env(parent = globalenv())
  app_env$tablerApp <- function(ui, server, ...) {
    captured <<- list(ui = ui, server = server)
    invisible(NULL)
  }

  sys.source(app_file, envir = app_env, keep.source = FALSE)

  if (is.null(captured)) {
    if (!exists("ui", envir = app_env, inherits = FALSE) ||
        !exists("server", envir = app_env, inherits = FALSE)) {
      stop("app.R must either call tablerApp(ui, server) or define `ui` and `server`")
    }
    captured <- list(
      ui = get("ui", envir = app_env, inherits = FALSE),
      server = get("server", envir = app_env, inherits = FALSE)
    )
  }

  # Launch on the server-assigned loopback port. This call blocks and runs the
  # httpuv event loop until the process is terminated by tabler-server.
  tabler::tablerApp(
    ui = captured$ui,
    server = captured$server,
    host = "127.0.0.1",
    port = port,
    launch.browser = FALSE
  )
})
