#!/usr/bin/env bash
# Minimal installer for tabler-server (Linux only).
# Installs the binary + worker script, a config, a systemd unit, and creates
# the apps directory. Run as root.
set -euo pipefail

PREFIX=/opt/tabler-server
CONF_DIR=/etc/tabler-server
APPS_DIR=/srv/tabler-server/apps
LOG_DIR=/var/log/tabler-server
SERVICE_USER=tabler

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

if [[ $EUID -ne 0 ]]; then
  echo "This installer must be run as root." >&2
  exit 1
fi

echo "==> Building tabler-server"
make -C "$SCRIPT_DIR"
BIN="$SCRIPT_DIR/build/tabler-server"

echo "==> Creating service user '$SERVICE_USER' (if missing)"
if ! id -u "$SERVICE_USER" >/dev/null 2>&1; then
  useradd --system --home /srv/tabler-server --shell /usr/sbin/nologin "$SERVICE_USER"
fi

echo "==> Installing files"
install -d "$PREFIX/bin" "$PREFIX/share" "$PREFIX/share/docs" "$PREFIX/share/docs-r" "$CONF_DIR" "$APPS_DIR" "$LOG_DIR"
install -m 0755 "$BIN" "$PREFIX/bin/tabler-server"
install -m 0644 "$SCRIPT_DIR/R/worker.R" "$PREFIX/share/worker.R"
cp -r "$SCRIPT_DIR/docs/." "$PREFIX/share/docs/"
cp -r "$SCRIPT_DIR/docs-r/." "$PREFIX/share/docs-r/"

if [[ ! -f "$CONF_DIR/tabler-server.conf" ]]; then
  install -m 0644 "$SCRIPT_DIR/config/tabler-server.conf" "$CONF_DIR/tabler-server.conf"
else
  echo "    keeping existing $CONF_DIR/tabler-server.conf"
fi

install -m 0644 "$SCRIPT_DIR/config/systemd/tabler-server.service" \
  /etc/systemd/system/tabler-server.service

echo "==> Installing example apps (updating shipped examples, keeping other apps)"
cp -r "$SCRIPT_DIR/apps/." "$APPS_DIR/"

chown -R "$SERVICE_USER:$SERVICE_USER" /srv/tabler-server "$LOG_DIR"

echo "==> Reloading systemd"
systemctl daemon-reload

cat <<EOF

Done. Next steps:

  1. Ensure R and the 'tabler' package are installed system-wide:
       sudo R -e 'install.packages("tabler")'
  2. Start the service:
       sudo systemctl enable --now tabler-server
  3. Visit http://localhost:3000/

Apps live in: $APPS_DIR/<name>/app.R
EOF
