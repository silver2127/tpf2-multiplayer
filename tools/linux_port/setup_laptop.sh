#!/usr/bin/env bash
# setup_laptop.sh -- install the tpf2-port worker on the Linux machine. Idempotent.
#
#   bash setup_laptop.sh            install or update ~/tpf2-port (config.json is kept if present)
#   bash setup_laptop.sh --config   also overwrite config.json with this copy
#
# Creates one bare inbox per repository in config.json. Windows pushes every local
# branch there; the inbox's post-receive hook starts port_worker.py detached. The
# first push of each repository only records a baseline (nothing is ported).
set -euo pipefail
home=${TPF2_PORT_HOME:-$HOME/tpf2-port}
src=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

mkdir -p "$home/inbox" "$home/work" "$home/state" "$home/jobs"
install -m 755 "$src/port_worker.py" "$home/port_worker.py"
install -m 644 "$src/prompt.md" "$home/prompt.md"
if [ ! -f "$home/config.json" ] || [ "${1:-}" = --config ]; then
  install -m 644 "$src/config.json" "$home/config.json"
fi

for repo in $(python3 -c 'import json, sys; print(" ".join(json.load(open(sys.argv[1]))["repos"]))' "$home/config.json"); do
  inbox=$home/inbox/$repo.git
  [ -d "$inbox" ] || git init --quiet --bare "$inbox"
  cat > "$inbox/hooks/post-receive" <<EOF
#!/bin/sh
# tpf2-port: start the worker detached; it exits at once if one is already running.
cat >/dev/null
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_OBJECT_DIRECTORY GIT_ALTERNATE_OBJECT_DIRECTORIES GIT_QUARANTINE_PATH
setsid nohup python3 "$home/port_worker.py" </dev/null >>"$home/state/worker.out" 2>&1 &
exit 0
EOF
  chmod 755 "$inbox/hooks/post-receive"
  echo "inbox ready: $inbox"
done

for tool in git python3 gh; do command -v "$tool" >/dev/null || echo "WARNING: $tool not found" >&2; done
for tool in codex claude; do [ -x "$HOME/.local/bin/$tool" ] || command -v "$tool" >/dev/null || echo "WARNING: $tool not found" >&2; done
gh auth status >/dev/null 2>&1 || echo "WARNING: gh is not logged in; pull requests cannot be opened" >&2
echo "tpf2-port installed in $home"
