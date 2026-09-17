<#
.SYNOPSIS
Send every local commit of the watched repositories to the Linux machine, which ports it.

.DESCRIPTION
Installs a reference-transaction hook in each repository's common .git\hooks (so
every worktree is covered). When a transaction moves the remote-tracking ref of one
of -Branches (origin/dev by default: it moves when anyone on this PC pushes to dev,
or fetches a newer dev), the hook starts tpf2-port-push.sh detached and returns at
once; that script sends what GitHub has for those branches to
<remote>:tpf2-port/inbox/<repo>.git, retrying with backoff while the laptop is
unreachable. Local-only commits and other branches are never sent.
The inbox's post-receive hook starts port_worker.py.

  tools\linux_port\install_windows_hooks.ps1              install, then push once
  tools\linux_port\install_windows_hooks.ps1 -Uninstall   remove the hooks
  $env:TPF2_PORT_OFF = 1                                  skip for one shell session

An existing hook that this script did not write is never overwritten.
#>
param(
    [hashtable]$Repos = @{ 'tpf2-multiplayer' = 'C:\Users\james\tpf2-multiplayer'; 'tpf2-bigmap' = 'C:\Users\james\tpf2-bigmap' },
    [string]$Remote = 'topsnek@strelka',
    [string[]]$Branches = @('dev'),
    [string]$Upstream = 'origin',
    [switch]$Uninstall,
    [switch]$NoInitialPush
)
$ErrorActionPreference = 'Stop'
$Marker = '# tpf2-port hook (tools/linux_port/install_windows_hooks.ps1)'

function Write-Lf([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, ($Text -replace "`r`n", "`n"), (New-Object Text.UTF8Encoding $false))
}

$hookText = @'
#!/bin/sh
__MARKER__
# A watched GitHub branch moved: send it to the Linux machine in the background. Never blocks git.
[ "$1" = committed ] || exit 0
grep -qE ' refs/remotes/__UPSTREAM__/(__BRANCHES__)$' || exit 0
[ -n "$TPF2_PORT_OFF" ] && exit 0
nohup sh "$(dirname "$0")/tpf2-port-push.sh" </dev/null >/dev/null 2>&1 &
exit 0
'@

$pushText = @'
#!/bin/sh
__MARKER__
# Push the watched branches to the Linux machine's inbox; retry with backoff while it is unreachable.
url='__URL__'
common=$(cd "$(dirname "$0")/.." && pwd)
# Leave the directory git was run in: a fetch from a temporary clone or a worktree
# that is removed afterwards left this pusher with no working directory, and git
# then fails every retry ("Unable to read current working directory") while the
# lock below holds newer pushes back (2026-09-16, twice, ~90 min each).
cd "$common" || exit 0
state="$common/tpf2-port"
mkdir -p "$state"
log="$state/push.log"
: > "$state/dirty"
lock="$state/lock"
if ! mkdir "$lock" 2>/dev/null; then
  # a pusher is running and will see the dirty flag; a lock older than three hours is stale
  [ -n "$(find "$lock" -maxdepth 0 -mmin +180 2>/dev/null)" ] || exit 0
  rmdir "$lock" 2>/dev/null
  mkdir "$lock" 2>/dev/null || exit 0
fi
trap 'rmdir "$lock" 2>/dev/null' EXIT
if [ -f "$log" ] && [ "$(wc -c < "$log")" -gt 1000000 ]; then tail -n 2000 "$log" > "$log.tmp" && mv "$log.tmp" "$log"; fi
delay=60
tries=0
while [ -e "$state/dirty" ]; do
  rm -f "$state/dirty"
  if GIT_SSH_COMMAND='ssh -o BatchMode=yes -o ConnectTimeout=15' \
     git --git-dir="$common" push --quiet --force "$url" __REFSPECS__ >>"$log" 2>&1; then
    echo "$(date '+%F %T') pushed" >>"$log"
    tries=0
    delay=60
  else
    : > "$state/dirty"
    tries=$((tries + 1))
    if [ "$tries" -ge 8 ]; then
      # the dirty flag stays: the next hook run pushes what is pending
      echo "$(date '+%F %T') push failed $tries times; giving up until the next commit" >>"$log"
      exit 0
    fi
    echo "$(date '+%F %T') push failed (try $tries); retrying in ${delay}s" >>"$log"
    sleep "$delay"
    delay=$((delay * 2)); [ "$delay" -gt 1800 ] && delay=1800
  fi
done
'@

foreach ($name in $Repos.Keys) {
    $path = $Repos[$name]
    $common = (git -C $path rev-parse --path-format=absolute --git-common-dir).Trim()
    if ($LASTEXITCODE -ne 0) { throw "$path is not a git repository" }
    $hooksPath = (git -C $path config --get core.hooksPath)
    if ($hooksPath) { throw "$name sets core.hooksPath=$hooksPath; install the hook there by hand" }
    $hooks = Join-Path $common 'hooks'
    New-Item -ItemType Directory -Force $hooks | Out-Null
    $hook = Join-Path $hooks 'reference-transaction'
    $push = Join-Path $hooks 'tpf2-port-push.sh'
    $ours = (Test-Path $hook) -and ((Get-Content $hook -Raw) -match [regex]::Escape($Marker))

    if ($Uninstall) {
        if ($ours) { Remove-Item $hook; Write-Host "[$name] hook removed" }
        if (Test-Path $push) { Remove-Item $push }
        continue
    }
    if ((Test-Path $hook) -and -not $ours) { throw "[$name] $hook exists and was not written by tpf2-port; not touching it" }
    $url = "${Remote}:tpf2-port/inbox/$name.git"
    $alternation = ($Branches | ForEach-Object { [regex]::Escape($_) }) -join '|'
    $refspecs = ($Branches | ForEach-Object { "'+refs/remotes/$Upstream/$($_):refs/heads/$($_)'" }) -join ' '
    Write-Lf $hook ($hookText.Replace('__MARKER__', $Marker).Replace('__UPSTREAM__', $Upstream).Replace('__BRANCHES__', $alternation))
    Write-Lf $push ($pushText.Replace('__MARKER__', $Marker).Replace('__URL__', $url).Replace('__REFSPECS__', $refspecs))
    Write-Host "[$name] hook installed in $hooks -> $url"

    if (-not $NoInitialPush) {
        $env:GIT_SSH_COMMAND = 'ssh -o BatchMode=yes -o ConnectTimeout=15'
        $have = @($Branches | Where-Object { git --git-dir="$common" rev-parse -q --verify "refs/remotes/$Upstream/$_" })
        if ($have.Count -gt 0) {
            git --git-dir="$common" push --quiet --force $url ($have | ForEach-Object { "+refs/remotes/$Upstream/$($_):refs/heads/$($_)" })
            if ($LASTEXITCODE -ne 0) { throw "[$name] initial push to $url failed" }
        }
        Remove-Item Env:GIT_SSH_COMMAND
        Write-Host "[$name] initial push done (the Linux machine records it as the baseline)"
    }
}
