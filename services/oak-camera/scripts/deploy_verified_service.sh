#!/usr/bin/env bash
set -euo pipefail
if [[ ${EUID} -ne 0 || $# -ne 2 ]]; then
  echo "Usage: sudo $0 VERIFIED_BINARY EXPECTED_SHA256" >&2
  exit 2
fi
binary="$(realpath -- "$1")"
expected="$2"
[[ "$expected" =~ ^[[:xdigit:]]{64}$ ]] || { echo "Invalid SHA256" >&2; exit 2; }
[[ -f "$binary" && -x "$binary" ]] || { echo "Binary missing/not executable" >&2; exit 2; }
actual="$(sha256sum -- "$binary")"; actual="${actual%% *}"
[[ "$actual" == "$expected" ]] || { echo "Binary SHA256 mismatch" >&2; exit 2; }
service=tenco-oak-camera.service
target=/opt/tenco/bin/oak_camera_service
systemctl show "$service" -p ExecStart --value | grep -Fq "path=$target ;" || {
  echo "Unexpected systemd executable; inspect before deployment" >&2; exit 2;
}
status="$(curl --noproxy '*' --silent --show-error --max-time 3 http://127.0.0.1:18080/camera/status)"
python3 -c 'import json,sys; s=json.load(sys.stdin); sys.exit(1 if s.get("recording",True) else 0)' <<< "$status" || {
  echo "Cannot replace while recording or without a known recording state" >&2; exit 2;
}
backup="$target.backup-$(date +%Y%m%d-%H%M%S)-$$"
cp -p -- "$target" "$backup"
echo "Backup: $backup"
sha256sum -- "$backup"
install -m 0755 -- "$binary" "$target.pending-$$"
mv -- "$target.pending-$$" "$target"
rollback() {
  echo "Readiness failed; restoring $backup" >&2
  cp -p -- "$backup" "$target.rollback-$$"
  mv -- "$target.rollback-$$" "$target"
  systemctl restart "$service"
}
if ! systemctl restart "$service"; then rollback; exit 1; fi
for ((attempt=0; attempt<15; attempt++)); do
  status="$(curl --noproxy '*' --silent --max-time 3 http://127.0.0.1:18080/camera/status || true)"
  if python3 -c 'import json,sys; s=json.load(sys.stdin); sys.exit(0 if s.get("success") and s.get("cameraConnected") and s.get("frameId",0)>0 else 1)' <<< "$status" 2>/dev/null; then
    echo "$status"
    echo "Installed $actual; verify continuous frames and device recovery next."
    exit 0
  fi
  sleep 2
done
rollback
exit 1
