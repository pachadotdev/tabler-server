# Minimal Makefile for tabler-server (Linux, C++17).

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
LDFLAGS  ?= -pthread

BUILDDIR := build
SRC := $(wildcard src/*.cpp)
OBJ := $(SRC:src/%.cpp=$(BUILDDIR)/%.o)
BIN := $(BUILDDIR)/tabler-server

PREFIX     ?= /opt/tabler-server
SYSCONFDIR ?= /etc/tabler-server
UNITDIR    ?= /etc/systemd/system
APPSDIR    ?= /srv/tabler-server/apps
LOGDIR     ?= /var/log/tabler-server
SERVICE_USER ?= tabler

.PHONY: all clean install uninstall

all: $(BIN)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(BUILDDIR)/%.o: src/%.cpp | $(BUILDDIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

clean:
	rm -rf $(BUILDDIR)

install: all
	# --- program files ---
	install -d $(DESTDIR)$(PREFIX)/bin
	install -d $(DESTDIR)$(PREFIX)/share
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/$(BIN)
	install -m 0644 R/worker.R $(DESTDIR)$(PREFIX)/share/worker.R
	# --- config (kept if it already exists) ---
	install -d $(DESTDIR)$(SYSCONFDIR)
	@if [ ! -f $(DESTDIR)$(SYSCONFDIR)/tabler-server.conf ]; then \
		install -m 0644 config/tabler-server.conf $(DESTDIR)$(SYSCONFDIR)/tabler-server.conf; \
	else \
		echo "keeping existing $(DESTDIR)$(SYSCONFDIR)/tabler-server.conf"; \
	fi
	# --- systemd unit ---
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 config/systemd/tabler-server.service $(DESTDIR)$(UNITDIR)/tabler-server.service
	# --- runtime dirs + example apps ---
	install -d $(DESTDIR)$(APPSDIR)
	install -d $(DESTDIR)$(LOGDIR)
	@if [ -z "$$(ls -A $(DESTDIR)$(APPSDIR) 2>/dev/null)" ]; then \
		cp -r apps/. $(DESTDIR)$(APPSDIR)/; \
	fi
	# --- post-install: only on a real system install (DESTDIR unset) ---
	@if [ -z "$(DESTDIR)" ]; then \
		if ! id -u $(SERVICE_USER) >/dev/null 2>&1; then \
			echo "creating service user '$(SERVICE_USER)'"; \
			useradd --system --home /srv/tabler-server --shell /usr/sbin/nologin $(SERVICE_USER) || true; \
		fi; \
		chown -R $(SERVICE_USER):$(SERVICE_USER) /srv/tabler-server $(LOGDIR) 2>/dev/null || true; \
		if command -v systemctl >/dev/null 2>&1; then \
			systemctl daemon-reload; \
			echo ""; \
			echo "Installed. Start with:  sudo systemctl enable --now tabler-server"; \
		fi; \
	fi

uninstall:
	-@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then \
		systemctl disable --now tabler-server 2>/dev/null || true; \
	fi
	rm -f $(DESTDIR)$(UNITDIR)/tabler-server.service
	rm -f $(DESTDIR)$(PREFIX)/bin/$(BIN)
	rm -f $(DESTDIR)$(PREFIX)/share/worker.R
	@if [ -z "$(DESTDIR)" ] && command -v systemctl >/dev/null 2>&1; then systemctl daemon-reload; fi
	@echo "Left $(SYSCONFDIR), $(APPSDIR) and $(LOGDIR) in place (remove manually if desired)."
