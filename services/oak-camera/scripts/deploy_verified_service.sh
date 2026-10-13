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
temporary_backup_sha="$(sha256sum -- "$backup")"; temporary_backup_sha="${temporary_backup_sha%% *}"
echo "Backup: $backup"
sha256sum -- "$backup"
install -m 0755 -- "$binary" "$target.pending-$$"
mv -- "$target.pending-$$" "$target"
rollback() {
  echo "Readiness failed; restoring $backup" >&2
  cp -p -- "$backup" "$target.rollback-$$"
  mv -- "$target.rollback-$$" "$target"
  systemctl restart "$service"
  rm -- "$backup"
}
if ! systemctl restart "$service"; then rollback; exit 1; fi
previous_frame=0
healthy_samples=0
for ((attempt=0; attempt<15; attempt++)); do
  status="$(curl --noproxy '*' --silent --max-time 3 http://127.0.0.1:18080/camera/status || true)"
  frame="$(python3 -c 'import json,sys; s=json.load(sys.stdin); f=s.get("frameId",0); sys.exit(1) if not(s.get("success") and s.get("cameraConnected") and f>0) else print(f)' <<< "$status" 2>/dev/null || true)"
  if [[ "$frame" =~ ^[0-9]+$ ]] && ((frame > previous_frame)); then
    ((healthy_samples+=1))
    previous_frame="$frame"
  else
    healthy_samples=0
    previous_frame=0
  fi
  if ((healthy_samples>=3)); then
    installed_sha="$(sha256sum -- "$target")"; installed_sha="${installed_sha%% *}"
    [[ "$installed_sha" == "$expected" ]] || { rollback; exit 1; }
    # A verified install keeps only the newest binary; the old copy is temporary.
    backup_sha="$(sha256sum -- "$backup")"; backup_sha="${backup_sha%% *}"
    [[ "$backup_sha" == "$temporary_backup_sha" ]] || { echo "Temporary backup changed; inspect before cleanup" >&2; exit 1; }
    rm -- "$backup"
    echo "$status"
    echo "Removed temporary backup; only the verified latest binary is retained."
    echo "Installed $actual; verify continuous frames and device recovery next."
    exit 0
  fi
  sleep 2
done
rollback
exit 1
