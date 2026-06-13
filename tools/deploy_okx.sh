#!/usr/bin/env bash
set -euo pipefail

# =========================================================================
# spp-okx VPS deployment script
#
# Tested on: Ubuntu 22.04 / 24.04, Debian 12
# Assumptions: fresh OS, root or sudo-capable user
#
# Usage:
#   chmod +x deploy_okx.sh
#   ./deploy_okx.sh
#
# You'll be prompted for OKX API credentials; they get written into an
# env file readable only by the spp-okx service user.
# =========================================================================

REPO_URL="${REPO_URL:-https://github.com/ericyangbit/spp.git}"
REPO_DIR="${REPO_DIR:-/opt/spp}"
SERVICE_USER="${SERVICE_USER:-spp}"
ENV_FILE="${ENV_FILE:-/etc/spp/env}"
WAL_DIR="${WAL_DIR:-/var/lib/spp}"
LOG_DIR="${LOG_DIR:-/var/log/spp}"
SYMBOL="${SYMBOL:-BTC-USDT}"
CASH="${CASH:-100}"

# -- helpers ---------------------------------------------------------------
say()  { printf '\033[32m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[33mWARN: %s\033[0m\n' "$*"; }
die()  { printf '\033[31mFATAL: %s\033[0m\n' "$*" >&2; exit 1; }

# -- check root ------------------------------------------------------------
if [[ $EUID -ne 0 ]]; then die "Run as root (sudo ./deploy_okx.sh)"; fi

# -- 1. OS detection + install deps ----------------------------------------
say "installing system dependencies..."

if [[ -f /etc/os-release ]]; then
    . /etc/os-release
    case "$ID" in
        ubuntu|debian)
            apt-get update -qq
            apt-get install -y -qq \
                clang-18 libc++-18-dev libc++abi-18-dev \
                libmbedtls-dev \
                make git curl
            # Link clang-18 → clang++ if needed.
            if ! command -v clang++ &>/dev/null; then
                update-alternatives --install /usr/bin/clang++ clang++ \
                    /usr/bin/clang++-18 100
            fi
            ;;
        arch)
            pacman -Sy --noconfirm clang mbedtls make git curl 2>/dev/null || true
            ;;
        *)
            die "Unsupported OS: $ID.  PRs welcome."
            ;;
    esac
else
    die "Can't detect OS.  Manual install: clang++-18 + libmbedtls-dev + make + git"
fi

command -v clang++ >/dev/null 2>&1 || die "clang++ not found after install"
command -v make     >/dev/null 2>&1 || die "make not found"
say "dependencies ok"

# -- 2. Create service user ------------------------------------------------
if ! id -u "$SERVICE_USER" &>/dev/null; then
    useradd --system --no-create-home --home-dir "$REPO_DIR" \
        --shell /usr/sbin/nologin "$SERVICE_USER"
    say "created user $SERVICE_USER"
fi

# -- 3. Clone / update repo ------------------------------------------------
if [[ -d "$REPO_DIR/.git" ]]; then
    say "repo exists — pulling latest main..."
    (cd "$REPO_DIR" && git fetch origin && git checkout main && git pull origin main)
else
    say "cloning $REPO_URL..."
    mkdir -p "$(dirname "$REPO_DIR")"
    git clone "$REPO_URL" "$REPO_DIR"
fi
chown -R "$SERVICE_USER:$SERVICE_USER" "$REPO_DIR"

# -- 4. Build --------------------------------------------------------------
say "building SPP_TLS=1..."
(cd "$REPO_DIR" && SPP_TLS=1 make clean && SPP_TLS=1 make -j"$(nproc)" lib)

# Build the OKX demo binary.
SPP_TLS=1 make -C "$REPO_DIR" build/bin/app/examples/okx_demo \
    -j"$(nproc)" 2>&1 || die "demo build failed"
say "binaries built"

# -- 5. Create directories -------------------------------------------------
mkdir -p "$WAL_DIR" "$LOG_DIR"
chown "$SERVICE_USER:$SERVICE_USER" "$WAL_DIR" "$LOG_DIR"

# -- 6. Credentials --------------------------------------------------------
if [[ -f "$ENV_FILE" ]]; then
    warn "env file $ENV_FILE already exists — re-using"
else
    say "OKX API credentials needed (demo trading keys are fine for now)"
    read -rp "  API Key     [required]: " OKX_KEY
    read -rp "  API Secret  [required]: " OKX_SEC
    read -rp "  Passphrase  [required]: " OKX_PASS
    read -rp "  Simulated? (y/n) [y]:    " OKX_SIM
    OKX_SIM="${OKX_SIM:-y}"
    [[ "$OKX_KEY"  ]] || die "API Key is required"
    [[ "$OKX_SEC"  ]] || die "API Secret is required"
    [[ "$OKX_PASS" ]] || die "Passphrase is required"
    OKX_SIM_FLAG=0
    [[ "$OKX_SIM" =~ ^[yY1] ]] && OKX_SIM_FLAG=1

    mkdir -p "$(dirname "$ENV_FILE")"
    cat > "$ENV_FILE" <<EOF
# spp-okx runtime env — sourced by systemd.  Keep permissions 0600.
OKX_API_KEY=$OKX_KEY
OKX_SECRET=$OKX_SEC
OKX_PASSPHRASE=$OKX_PASS
OKX_SIMULATED=$OKX_SIM_FLAG
EOF
    chmod 0600 "$ENV_FILE"
    chown "$SERVICE_USER:$SERVICE_USER" "$ENV_FILE"
    say "env file written to $ENV_FILE"
fi

# -- 7. systemd unit -------------------------------------------------------
cat > /etc/systemd/system/spp-okx.service <<EOF
[Unit]
Description=spp-okx live trading demo (Chan on OKX Spot)
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=$SERVICE_USER
WorkingDirectory=$WAL_DIR
EnvironmentFile=$ENV_FILE
ExecStart=$REPO_DIR/build/bin/app/examples/okx_demo $SYMBOL $CASH
Restart=on-failure
RestartSec=10
StandardOutput=append:$LOG_DIR/stdout.log
StandardError=append:$LOG_DIR/stderr.log
# Hard limits — tweak after you've seen memory usage.
MemoryMax=512M
CPUQuota=200%

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload
systemctl enable spp-okx

# -- 8. Log rotation -------------------------------------------------------
cat > /etc/logrotate.d/spp-okx <<EOF
$LOG_DIR/*.log {
    daily
    rotate 30
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
EOF
say "logrotate configured"

# -- done ------------------------------------------------------------------
say ""
say "Deploy complete."
say ""
say "────────────────────────────────────────────────────────────"
say " Review config:"
say "   env file:  $ENV_FILE"
say "   symbol:    $SYMBOL"
say "   cash:      $CASH USDT"
say "   binary:    $REPO_DIR/build/bin/app/examples/okx_demo"
say ""
say " Start:       sudo systemctl start spp-okx"
say " Status:      sudo systemctl status spp-okx"
say " Follow log:  tail -f $LOG_DIR/stderr.log"
say " Stop:        sudo systemctl stop spp-okx"
say ""
say " Kill switch (remote):  sudo -u $SERVICE_USER touch /tmp/spp_kill"
say "────────────────────────────────────────────────────────────"
