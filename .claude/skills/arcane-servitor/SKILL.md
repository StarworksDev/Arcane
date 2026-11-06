---
name: arcane-servitor
description: Use when a change must be checked in a RUNNING Arcane host (editor, runtime or server) rather than only in unit tests. Covers seeing what a scene renders now, stepping the simulation N ticks and inspecting entities or ECS state, reading or setting cvars, watching logs and Problems live, confirming a hot reload landed, and driving play/pause/stop. Also use it when a user asks what the engine is doing right now.
---

# Driving live hosts with Servitor

## Overview

`servitor` is a loopback CLI that attaches to running Arcane hosts and prints one JSON envelope per call. It is not an MCP server: run it with your shell tool, like `git`. Each host with remote armed listens on `127.0.0.1` and writes a descriptor (port + token) under `<project>\Saved\Remote\`. Servitor finds hosts through those descriptors.

A Servitor check is evidence for THIS change, not a regression net. If the behavior must stay fixed, write an ArcaneTests case or a golden lane (`arcane-tests`, `arcane-verify`).

**REQUIRED BACKGROUND:** `arcane-build` (hosts and the game module must be built for the config you launch).

## Setup

- **Binary:** `Binaries\<Cfg>-windows-x86_64-md\Servitor\servitor.exe`. It is built with `Arcane.slnx` and compiled out of Dist (there it exits 2 with a one-line refusal on stderr, whatever the format).
- **Project:** `--project <dir|.arcproj>`, otherwise the nearest `.arcproj` above the working directory.
- **Engine root:** `--engine <root>` or `SERVITOR_ENGINE` when servitor cannot find the host exes beside itself.
- **Name yourself, uniquely:** pick a client name no other agent uses: your agent name plus a session or run id (`b7-impl`, `claude-<session-id>`, `my-agent-$PID`), never a shared name like `claude-code`. Pass `--client NAME` on every call, `launch`, `status` and `quit` included, so the host's audit log (`Saved\Remote\audit.ndjson`) names you.
  - `launch --client NAME` records NAME in the host's descriptor. A different `--client` after `--` is refused (exit 2).
  - `status --client NAME` lists ONLY NAME's hosts and hides every other host, the user's editor included. To see what is running, use `status` without `--client`.
  - `quit --client NAME` picks its host in this order. `--pid N` wins: that process, with `--client` only the audit name. Otherwise NAME's hosts, narrowed by `--host`: one is quit (`--force` works, one envelope answers); several need `--yes` and refuse `--force`. With none of NAME's hosts, `--host KIND` quits the one KIND host Servitor launched WITHOUT `--client` (how hosts were launched before descriptors carried a client; the descriptor's `explicitRemote` marks a `--remote` launch), but never a host another client launched (exit 4 naming that client) and never a host Servitor did not launch (exit 4 naming it). With none and no `--host`: exit 4. That fallback never selects an editor the user opened by hand, even with `--force`.
  - A shared name is a hazard: two agents with one name match each other's hosts, and `--yes` would quit the other agent's host.
- **Read the output:** `--format json` (or `--json`) gives the full envelope, `--format ndjson` frames it. Under json and ndjson everything, usage refusals and `--verbose` notes included, is on stdout and stderr stays empty (a refusal is one `arg.invalid` envelope with `client.exitCode` 2; `--verbose` adds a `servitor.verbose` warning). Human format writes diagnostics to stderr. Branch on the exit code, never on HTTP status.
- **Launched hosts end themselves.** A host started with an explicit `--remote` (`servitor launch` always adds it) on an open-ended run exits after `--remote-idle-exit` seconds with no request (default 600, 0 = never). `--owner-pid <pid>` (after `--` on `launch`) also ends it when that process is gone, even while another agent is driving it. It is opt-in: Servitor never fills it in, and an omitted pid watches nothing. An editor the user opens by hand (armed by `editor.remote.enable`, no `--remote`) never idle-exits and ignores `--owner-pid`. Still `quit` what you launched.

How each host gets remote:

| Host | How it arms remote | Startup |
|---|---|---|
| ArcaneEditor | cvar `editor.remote.enable` (default ON) or `--remote` | opens in Edit |
| ArcaneRuntime | `--remote` (`servitor launch` adds it) | open-ended run starts HELD at frame 0 (log line, `[held]` title); `play`/`resume` releases it; `--frames N` runs |
| ArcaneServer | `--remote` | same hold rule as the runtime |

## Recipes

```bat
set S=Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe
set L=--project ReferenceProject
:: a client name no other agent uses (agent name + session or run id)
set P=--project ReferenceProject --client my-agent-1234

:: what is running (every host; --client would hide everyone else's)
%S% %L% status

:: headless runtime, deterministic picture
%S% %P% launch runtime --headless -- --fixed-time 0
%S% %P% --host runtime wait settled
%S% %P% --host runtime screenshot window --out %TEMP%\shot.png
%S% %P% --host runtime compare runtime-scene --source window

:: step the sim exactly N ticks, then inspect state
%S% %P% --host runtime step 10
%S% %P% --host runtime entity get name:Player --component Transform
%S% %P% --host runtime ecs query --with Transform,RigidBody2D --fields Transform --limit 20

:: live diagnostics while reproducing something
%S% %P% --host editor problems --severity error
%S% %P% --host editor logs --level warn --follow --timeout 30
%S% %P% --host editor cvar explain render.vsync

:: after rebuilding the game module, confirm the editor reloaded it
%S% %P% --host editor wait reload

:: clean up what you launched (your tagged runtime; --pid <n> names one exactly)
%S% %P% --host runtime quit
```

### Wrappers that always clean up

Both launch with a client name unique to the run and read the host's pid from launch's json envelope (`host.pid`). They drive that pid and `quit --pid` it even when a step fails, so they never quit a host they did not launch. A `launch` that exits 8 (`launch.timeout`) left the host RUNNING, possibly before it wrote its descriptor, so they `wait ready --pid` (which waits for the descriptor) and then quit that pid too. Exit 7 means it already exited. The script exits with Servitor's exit code.

PowerShell 5.1:

```powershell
$s = 'Binaries\Debug-windows-x86_64-md\Servitor\servitor.exe'
$c = @('--project', 'ReferenceProject', '--client', "my-agent-$PID")   # unique to this run
$launch = (& $s @c --json launch runtime --headless -- --fixed-time 0) -join "`n" | ConvertFrom-Json
$code = $LASTEXITCODE
$hostPid = $launch.host.pid   # set on 0 and on 8: the process is running either way
if ($code -eq 8) {   # launch.timeout: still running, maybe with no descriptor yet
    & $s @c --pid $hostPid wait ready | Out-Null; & $s @c --pid $hostPid quit --force | Out-Null; exit 8
}
if ($code -ne 0) { exit $code }   # 7: it already exited; 2, 4: nothing started
$t = @('--pid', $hostPid)
$failed = 0
function Step([string]$what) {
    if ($LASTEXITCODE -eq 0) { return }
    $script:failed = $LASTEXITCODE
    throw "$what failed with exit $LASTEXITCODE (2 usage, 3 permission, 4 no host, 5 busy: retry, 6 failed, 7 host died, 8 check failed)"
}
try {
    & $s @c @t wait settled; Step 'wait settled'
    & $s @c @t screenshot window --out "$env:TEMP\shot.png"; Step 'screenshot'
}
catch { Write-Host $_ }
finally { & $s @c @t quit | Out-Null }
exit $failed
```

bash (Git Bash; the binary path is repo-relative):

```bash
S=Binaries/Debug-windows-x86_64-md/Servitor/servitor.exe
C=(--project ReferenceProject --client "my-agent-$$")   # unique to this run
out=$("$S" "${C[@]}" --json launch runtime --headless -- --fixed-time 0); code=$?
HOSTPID=$(printf '%s' "$out" | sed -n 's/.*"host":{[^}]*"pid":\([0-9]*\).*/\1/p')
case "$code" in
  0) ;;
  8) "$S" "${C[@]}" --pid "$HOSTPID" wait ready >/dev/null       # launch.timeout: still running,
     "$S" "${C[@]}" --pid "$HOSTPID" quit --force >/dev/null; exit 8 ;;   # maybe with no descriptor yet
  *) exit "$code" ;;                                                       # 7: it already exited
esac
T=(--pid "$HOSTPID")
trap '"$S" "${C[@]}" "${T[@]}" quit >/dev/null' EXIT
step() {  # step <exit code> <what>
  [ "$1" -eq 0 ] && return
  echo "$2 failed with exit $1 (2 usage, 3 permission, 4 no host, 5 busy: retry, 6 failed, 7 host died, 8 check failed)" >&2
  exit "$1"
}
"$S" "${C[@]}" "${T[@]}" wait settled; step $? "wait settled"
"$S" "${C[@]}" "${T[@]}" screenshot window --out "${TMPDIR:-/tmp}/shot.png"; step $? screenshot
```

- `describe <command>` and `schema component <Name>` explain a command's or a component's arguments.
- `commands` prints the whole manifest and the exit codes with no host running.
- `call <command> name=value ...` reaches any registered command.

## Exit codes (branch on these)

| Code | Meaning | What to do |
|---|---|---|
| 0 | ok | |
| 2 | usage/refusal: unknown verb or flag, ambiguous target, a flag that does not apply (never silently ignored) | fix the command line |
| 3 | permission: read-only token, or the command's audience refused | use the drive path, or a different cvar |
| 4 | no host / stale descriptor / protocol mismatch | `launch`, or pass `--project` |
| 5 | busy or not ready; provably NOT run | safe to retry (servitor already retries) |
| 6 | the host ran it and it failed (`errors[]`) | read `errors[0].code` and the remediation |
| 7 | host died or unreachable mid-call | check for an `.arcdiag` path; relaunch |
| 8 | a check did not pass: `compare.failed`, a wait false at timeout, `call.timeout`, `launch.timeout` | the result is the finding |

## Rules

- **Determinism.**
  - Captures of time-driven materials need a `--headless` host launched with `--fixed-time`; `--fixed-time` is refused without `--headless`.
  - `wait settled` (two byte-equal captures, shaders idle, clock frozen while it captures) comes before any screenshot or compare that you judge.
- **Drive vs observe.**
  - `play`, `stop`, `pause`, `resume`, `step`, `cvar set`, `console`, `quit` and `compare --bless` change the host and are audited.
  - Use `--read-only` when you only mean to look.
  - `--all` fans out to every host, and a drive command under `--all` needs `--yes`.
  - Never `--bless` from servitor to "make a diff go away". Blessing follows `arcane-verify`'s re-bless procedure and needs the user's intent.
- **Do not fight the user's session.**
  - An editor the user has open is THEIR session. Do not `play`, `stop`, `quit` or `cvar set` it without asking.
  - Prefer launching your own `--headless` host.
  - Never `quit --force` or `--discard-changes` a host you did not launch.
  - `quit --client NAME --host editor` with no editor of NAME's falls back only to an editor Servitor launched without `--client`; an editor the user opened (no `--remote`) is refused with exit 4, `--force` included. Still quit an editor you launched by your `--client` or its `--pid`.
- **Long steps.**
  - `step N` returns when `host.frame` moved N (a fixed tick each, or one loop pass each on a running windowed runtime).
  - Overlapping steps share one budget.
  - Give long steps a `--timeout`. A live host that is slow answers `call.timeout` (exit 8), not "relaunch".
- **Retry.** `--retry-on-death` resends only to a NEW replacement host of the same kind and project. It is refused with `--all` and with `--pid`.
- **Clean up.** `quit` every host you launched, and leave `Saved\Remote\audit.ndjson` alone. It is the audit trail.

## Common mistakes

- **Sandboxed agent CLIs.** Some block loopback by default (Codex does), so every call fails with exit 4 or 7. Allow localhost in that tool's sandbox settings.
- **Missing `wait`.** Calling a freshly launched host before `wait ready` gets exit 4/5. `launch` already waits for the descriptor; other spawns do not.
- **Held runtime.** A runtime launched open-ended is held at frame 0: `stats frame` stays put until `play`/`resume`/`step`.
- **Stale game module.** Judging pixels from a host whose game module is stale or the wrong config: see `arcane-build`'s single-slot trap.
