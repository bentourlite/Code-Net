#!/usr/bin/env bash
# Install or update Code Net on a Raspberry Pi. Run from anywhere inside the repository:
#   bash pi/install.sh
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY="$REPO/python"
VENV="$PY/.venv"
USER_NAME="${SUDO_USER:-$USER}"

echo "== System packages"
sudo apt-get update -qq
sudo apt-get install -y git python3-venv libportaudio2 libasound2-dev libopenblas0

echo "== Python environment in $VENV"
[ -x "$VENV/bin/python" ] || python3 -m venv "$VENV"
"$VENV/bin/pip" install -q --upgrade pip
"$VENV/bin/pip" install -q -e "$PY[pi]"

echo "== Settings in /etc/codenet"
sudo mkdir -p /etc/codenet
if [ ! -f /etc/codenet/receiver.json ]; then
  echo '{"name": "'"$(hostname | cut -c1-16)"'"}' | sudo tee /etc/codenet/receiver.json >/dev/null
fi
# The menu edits settings without sudo.
sudo chown -R "$USER_NAME" /etc/codenet

echo "== Service"
sed -e "s|@USER@|$USER_NAME|" -e "s|@VENV@|$VENV|" "$REPO/pi/codenet-receiver.service" \
  | sudo tee /etc/systemd/system/codenet-receiver.service >/dev/null
sudo usermod -aG audio "$USER_NAME"
sudo systemctl daemon-reload

echo "== Commands"
sudo ln -sf "$VENV/bin/codenet-menu" /usr/local/bin/codenet-menu
sudo ln -sf "$VENV/bin/codenet-receiver" /usr/local/bin/codenet-receiver

if ! grep -q "codenet-menu" "/home/$USER_NAME/.bashrc" 2>/dev/null; then
  read -r -p "Open the Code Net menu automatically when you log in over SSH? (y/n) " yn
  if [[ "$yn" == y* ]]; then
    cat >> "/home/$USER_NAME/.bashrc" <<'RC'

# Code Net: open the menu on SSH login (remove these lines to stop it).
if [ -n "$SSH_CONNECTION" ] && [ -z "$CODENET_MENU_SHOWN" ] && [ -t 0 ]; then
  export CODENET_MENU_SHOWN=1
  codenet-menu
fi
RC
  fi
fi

echo
echo "Done. Run 'codenet-menu' to set up the receiver, then start it from 'Start, stop and logs'."
