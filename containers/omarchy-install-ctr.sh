#!/bin/bash
#
# Curated, staged Omarchy install for the HypXRland container. Runs as the `dev`
# user inside a LIVE systemd+logind session (entered via `machinectl shell` by
# scripts/xr-container.sh build) — NOT in a `podman build` RUN, because the
# config stage wants a real per-user session bus (theme-set → gsettings, etc.).
#
# It sources the SAFE SUBSET of the pinned Omarchy v3.8.2 clone at
# ~/.local/share/omarchy (baked into the base image). We deliberately do NOT run
# Omarchy's install.sh / install/*/all.sh, because those pull in host-tie and
# boot-time surfaces that make no sense (and fail) in a rootless container:
# preflight sudoers/first-run, login/ (sddm, plymouth, limine, hibernation),
# post-install/, and the /etc-mutating + hardware config stages.
#
# Two stages (machinectl shell ALWAYS exits 0, so we write the real exit code to
# a sentinel file /tmp/omarchy-install.exit that the host wrapper reads):
#   packages  — pacman-install the curated desktop package subset (dev + sudo).
#   config    — seed ~/.config from Omarchy defaults, set the theme, wire
#               walker/elephant, user-dirs, toggles, branding, mimetypes.
#   all       — packages then config (default).
#
# Usage (inside the container, as dev):  bash omarchy-install-ctr.sh <stage>

set -uo pipefail

STAGE="${1:-all}"

export OMARCHY_PATH="$HOME/.local/share/omarchy"
export OMARCHY_INSTALL="$OMARCHY_PATH/install"
export OMARCHY_INSTALL_LOG_FILE="/var/log/omarchy-install.log"
export PATH="$OMARCHY_PATH/bin:$PATH"

SENTINEL=/tmp/omarchy-install.exit
rm -f "$SENTINEL"
FINAL_RC=0
finish() { echo "${FINAL_RC}" >"$SENTINEL"; }
trap finish EXIT

log() { printf '\n\033[1;34m[omarchy-ctr] %s\033[0m\n' "$*" | tee -a "$OMARCHY_INSTALL_LOG_FILE"; }
warn() { printf '\033[1;33m[omarchy-ctr] WARN: %s\033[0m\n' "$*" | tee -a "$OMARCHY_INSTALL_LOG_FILE"; }

[[ -d $OMARCHY_PATH ]] || { echo "Omarchy tree missing at $OMARCHY_PATH" >&2; FINAL_RC=1; exit 1; }

# =============================================================================
# PACKAGES stage — curated desktop subset of install/omarchy-base.packages.
# =============================================================================
#
# KEEP = a representative, functional Omarchy session (compositor session bits,
# launcher, notifications, themed terminal, portals, fonts, theme system) plus
# the CLI/tools Omarchy's own bin/ + config scripts call. Everything is either
# Arch core/extra or the prebuilt [omarchy] repo (added in the base image), so a
# plain `pacman -S` resolves it — no per-package yay builds needed.
#
# EXCLUSIONS from omarchy-base.packages (one reason each):
#   1password-beta 1password-cli   proprietary, heavy, host-tie
#   docker docker-buildx docker-compose lazydocker   container daemon, host-tie
#   sddm            display manager — we enter via machinectl, no DM/greeter
#   plymouth        boot splash — a container has no boot to splash
#   limine/snapper (login/) grub    bootloader — not our concern (login/ skipped)
#   signal-desktop spotify obsidian typora libreoffice-fresh obs-studio kdenlive
#                   heavy GUI apps irrelevant to an XR compositor session
#   cups* system-config-printer     printing stack tied to host hardware
#   bluetui impala iwd              bluetooth/wifi radios — no radios in container
#   avahi nss-mdns  mDNS — unneeded, wants host/system integration
#   power-profiles-daemon brightnessctl asdcontrol hyprsunset  hardware power/backlight/gamma
#   kernel-modules-hook             host kernel modules
#   plocate tzupdate ufw ufw-docker wireless-regdb   host indexing/net/firewall/radio
#   mariadb-libs postgresql-libs dotnet-runtime-9.0  db/runtime libs unused here
#   ruby rust clang llvm luarocks mise tree-sitter-cli  heavy dev toolchains (base-devel covers our build)
#   fcitx5 fcitx5-gtk fcitx5-qt     CJK input-method framework, not needed
#   claude-code aether              heavy AUR/omarchy extras, not session-critical
#   kvantum-qt5 qt5-wayland         Qt theming — session is GTK/wlroots-style
#   gpu-screen-recorder localsend   screen-recording / network file share extras
#   nautilus nautilus-python sushi gnome-disk-utility evince gnome-calculator
#                   heavy GNOME app stack — mimetype defaults set tolerantly
#   noto-fonts-cjk                  huge CJK font set
#   gnome-keyring libsecret         secret daemon — no session secrets needed
#   pinta xournalpp imagemagick ffmpegthumbnailer tesseract* inxi  image/OCR/thumb tools
#   alsa-utils wiremix wireplumber pamixer playerctl cliamp  audio stack — XR test has no audio
#   1password/webapp/spotify done above
CURATED_PKGS=(
  # --- compositor session (visible desktop) ---
  hyprland hyprland-guiutils         # stock compositor + dialogs; provides hyprctl (omarchy scripts call it). Dev binary from /build supersedes at run time.
  waybar mako swaybg
  walker omarchy-walker              # [omarchy] launcher + its config integration
  uwsm                               # session/env manager the omarchy autostart uses (uwsm-app scopes)
  swayosd                            # volume/brightness OSD session component
  hypridle hyprlock hyprpicker
  xdg-desktop-portal-hyprland xdg-desktop-portal-gtk
  polkit-gnome                       # polkit auth agent for the session
  # --- browser (Omarchy default) ---
  chromium                           # Omarchy's default browser: omarchy-launch-browser / SUPER+SHIFT+RETURN, the webapp helpers (omarchy-webapp-*), and the theme.sh chromium policy/appearance step all target it. Arch's /usr/bin/chromium wrapper sources ~/.config/chromium-flags.conf (seeded below).
  # --- terminal, fonts, theme ---
  alacritty                          # Omarchy default terminal
  ttf-jetbrains-mono-nerd noto-fonts noto-fonts-emoji woff2-font-awesome ttf-ia-writer
  fontconfig gnome-themes-extra yaru-icon-theme
  # --- screenshot / clipboard (omarchy keybinds) ---
  grim slurp satty wl-clipboard
  # --- CLI/tools omarchy bin + config scripts use, + shell UX ---
  gum jq fzf ripgrep fd bat eza zoxide starship btop fastfetch
  xdg-user-dirs xdg-terminal-exec socat less imv
)

install_packages() {
  log "Installing curated Omarchy desktop package subset (${#CURATED_PKGS[@]} packages)"
  # First try the whole set in one transaction (fast, shared download). If that
  # fails (an [omarchy]/AUR bit is transiently unavailable), fall back to a
  # per-package tolerant pass so one flaky package can't sink the desktop.
  if sudo pacman -Syu --noconfirm --needed "${CURATED_PKGS[@]}" 2>&1 | tee -a "$OMARCHY_INSTALL_LOG_FILE"; then
    log "Batch package install succeeded"
  else
    warn "Batch install failed — retrying per-package (tolerant)"
    local missing=()
    for p in "${CURATED_PKGS[@]}"; do
      if ! sudo pacman -S --noconfirm --needed "$p" >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1; then
        warn "package failed: $p"
        missing+=("$p")
      fi
    done
    if ((${#missing[@]})); then
      # Only the non-critical desktop extras are allowed to be missing; the
      # session-core set below must all be present or the stage fails.
      local core=(hyprland waybar mako walker uwsm alacritty xdg-desktop-portal-hyprland)
      for c in "${core[@]}"; do
        for m in "${missing[@]}"; do
          [[ $c == "$m" ]] && { warn "CRITICAL session package missing: $c"; FINAL_RC=1; }
        done
      done
      warn "packages that did not install: ${missing[*]}"
    fi
  fi

  # yay for optional future AUR use (tolerant — [omarchy] repo already covers the
  # desktop bits, so this is a convenience, never load-bearing).
  sudo pacman -S --noconfirm --needed yay >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 \
    || warn "yay not installed (non-fatal)"
}

# =============================================================================
# CONFIG stage — seed ~/.config + theme + walker/elephant + user-dirs, sourcing
# the SAFE Omarchy config scripts directly (not install/config/all.sh, which
# also runs hardware + /etc-mutating + docker/timezone stages).
# =============================================================================
config_session() {
  log "Seeding ~/.config from Omarchy defaults (config.sh)"
  # config/config.sh: copy config tree + default bashrc.
  mkdir -p ~/.config
  cp -R "$OMARCHY_PATH"/config/* ~/.config/ 2>>"$OMARCHY_INSTALL_LOG_FILE" || warn "config copy had issues"
  cp "$OMARCHY_PATH/default/bashrc" ~/.bashrc 2>>"$OMARCHY_INSTALL_LOG_FILE" || true

  # Container chromium sandbox: chromium's namespace sandbox spawns a NESTED
  # unprivileged user namespace. On this dev box that works (rootless podman +
  # kernel.unprivileged_userns_clone=1, verified: GUI chromium runs sandboxed with
  # no --no-sandbox), but hosts that lock nested userns down (unprivileged_userns_
  # clone=0 / max_user_namespaces=0 — common on hardened/Debian kernels) make
  # chromium abort with "Failed to move to new namespace" / "No usable sandbox!".
  # This is a CONTAINER-portability constraint, not an Omarchy one, so we append
  # --no-sandbox to the Omarchy chromium-flags.conf (seeded above) rather than
  # editing the tracked Omarchy default — the image then works on any host without
  # per-box kernel tuning. Arch's /usr/bin/chromium wrapper sources ~/.config/
  # chromium-flags.conf, so omarchy-launch-browser (and bare chromium) both pick
  # it up; --test-type drops the resulting "unsupported flag" infobar. Idempotent
  # (guarded so a config re-run doesn't duplicate the lines).
  local cflags="$HOME/.config/chromium-flags.conf"
  if [[ -f $cflags ]] && ! grep -qx -- '--no-sandbox' "$cflags"; then
    printf '%s\n' '--no-sandbox' '--test-type' >>"$cflags"
    log "chromium-flags.conf: appended --no-sandbox --test-type (rootless-podman nested userns)"
  fi

  log "Branding (branding.sh)"
  mkdir -p ~/.config/omarchy/branding
  cp "$OMARCHY_PATH/icon.txt" ~/.config/omarchy/branding/about.txt 2>/dev/null || true
  cp "$OMARCHY_PATH/logo.txt" ~/.config/omarchy/branding/screensaver.txt 2>/dev/null || true

  log "Fonts (fonts.sh)"
  mkdir -p ~/.local/share/fonts
  cp "$OMARCHY_PATH/config/omarchy.ttf" ~/.local/share/fonts/ 2>/dev/null || true
  fc-cache >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 || true

  log "Icons (icons.sh)"
  mkdir -p ~/.local/share/applications/icons
  cp "$OMARCHY_PATH"/applications/icons/*.png ~/.local/share/applications/icons/ 2>/dev/null || true

  log "Theme: Tokyo Night (trimmed theme.sh — chromium steps now RUN; nautilus system links skipped)"
  mkdir -p ~/.config/omarchy/themes ~/.config/btop/themes ~/.config/mako
  # Yaru action-icon links that theme.sh makes with sudo — tolerant (yaru present).
  sudo ln -snf /usr/share/icons/Adwaita/symbolic/actions/go-previous-symbolic.svg \
       /usr/share/icons/Yaru/scalable/actions/go-previous-symbolic.svg 2>/dev/null || true
  sudo ln -snf /usr/share/icons/Adwaita/symbolic/actions/go-next-symbolic.svg \
       /usr/share/icons/Yaru/scalable/actions/go-next-symbolic.svg 2>/dev/null || true
  # Chromium theme integration (theme.sh's chromium block) — now that chromium is
  # installed these should apply for real, not tolerantly: a managed-policy dir
  # (so omarchy-install-chromium-google-account / browser policies have a home)
  # and a chromium initial_preferences that follows system appearance ("device").
  sudo mkdir -p /etc/chromium/policies/managed 2>/dev/null || true
  sudo chmod a+rw /etc/chromium/policies/managed 2>/dev/null || true
  echo '{"browser":{"theme":{"color_scheme":0,"color_scheme2":0}}}' \
    | sudo tee /usr/lib/chromium/initial_preferences >/dev/null 2>&1 || true
  # Skip the wallpaper swap at build time (no compositor running); the session
  # launcher applies it. Restarts are pgrep-guarded and no-op with nothing running.
  OMARCHY_THEME_SKIP_BACKGROUND=1 omarchy-theme-set "Tokyo Night" \
    >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 || warn "theme-set had non-fatal issues (gsettings)"
  ln -snf ~/.config/omarchy/current/theme/btop.theme ~/.config/btop/themes/current.theme 2>/dev/null || true
  ln -snf ~/.config/omarchy/current/theme/mako.ini ~/.config/mako/config 2>/dev/null || true

  log "User dirs (user-dirs.sh)"
  mkdir -p ~/Downloads ~/Pictures ~/Videos ~/.config/gtk-3.0
  xdg-user-dirs-update --set TEMPLATES "$HOME" 2>/dev/null || true
  xdg-user-dirs-update --set PUBLICSHARE "$HOME" 2>/dev/null || true
  xdg-user-dirs-update --set DESKTOP "$HOME" 2>/dev/null || true
  rmdir ~/Templates ~/Public ~/Desktop 2>/dev/null || true

  log "Walker/elephant wiring (walker-elephant.sh)"
  bash "$OMARCHY_PATH/install/config/walker-elephant.sh" >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 \
    || warn "walker-elephant wiring had non-fatal issues"

  log "Omarchy toggles (omarchy-toggles.sh)"
  mkdir -p ~/.local/state/omarchy/toggles/hypr
  cp "$OMARCHY_PATH/default/hypr/toggles/flags.conf" ~/.local/state/omarchy/toggles/hypr/ 2>/dev/null || true

  log "Mimetypes (mimetypes.sh — tolerant; some referenced apps intentionally absent)"
  omarchy-refresh-applications >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 || true
  update-desktop-database ~/.local/share/applications >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 || true
  bash "$OMARCHY_PATH/install/config/mimetypes.sh" >>"$OMARCHY_INSTALL_LOG_FILE" 2>&1 || true

  log "Config stage complete"
}

case "$STAGE" in
  packages) install_packages ;;
  config)   config_session ;;
  all)      install_packages; config_session ;;
  *) echo "unknown stage: $STAGE (want packages|config|all)" >&2; FINAL_RC=2; exit 2 ;;
esac

log "omarchy-install-ctr.sh stage '$STAGE' done (rc=$FINAL_RC)"
exit "$FINAL_RC"
