#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
#scp -r ../manager jar:/home/pi/jar

rsync -av --delete manager pi@jar:/home/pi/jar/
ssh pi@jar 'sudo systemctl restart scp-manager && systemctl is-active scp-manager'
