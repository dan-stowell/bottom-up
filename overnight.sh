#!/usr/bin/env bash
# Start a continuous run on the campaign in PLAN.md. Usage: ./overnight.sh [LIMIT]   (inside tmux)
# It runs until a file STOP exists in this directory: `touch STOP`. With LIMIT it creates STOP
# itself after a duration (`1h`, `90m`) or at a clock time (`"Sat 18:00"`, UTC unless TZ is set);
# the session then gets one turn to bring STATUS.md up to date. The agent is never told the limit.
# Sessions run on Fable; set MODEL to use another model (MODEL=opus ./overnight.sh).
#
# If claude exits, a fresh session starts on PLAN.md. A watchdog in the tmux pane:
# - types /clear and the start prompt when the session asks for a fresh context
#   (it touches .claude/fresh; LOOP.md, "Fresh contexts");
# - types "continue" a minute after a usage limit's printed reset time;
# - types "continue" after 30 quiet minutes for any other reason.
set -uo pipefail
cd "$(dirname "$0")"
model=${MODEL:-claude-fable-5-2}
rm -f STOP .claude/status-asked
if [ -n "${1:-}" ]; then
  if [[ $1 =~ ^[0-9]+[smhd]?$ ]]; then secs=$(( $(date -d "now + $(sed 's/s$/ sec/;s/m$/ min/;s/h$/ hour/;s/d$/ day/' <<<"$1")" +%s) - $(date +%s) ))
  else secs=$(( $(date -d "$1" +%s) - $(date +%s) )) || exit 1; fi
  [ "$secs" -gt 0 ] || { echo "overnight.sh: $1 is in the past" >&2; exit 1; }
  echo "overnight.sh: stopping at $(date -d "@$(( $(date +%s) + secs ))")"
  ( sleep "$secs"; touch STOP ) &
  limiter=$!
fi
prompt="Continuous run. Read CHARTER.md, PLAN.md and LOOP.md, then work the campaign in PLAN.md, starting from its Now line. There is no deadline; a session ending costs nothing because you commit often. Don't ask the user questions; decide and write down why."
nudge="Continue the campaign in PLAN.md from its Now line. (Watchdog: the session was idle, usually a usage limit that has now reset.)"
transcripts="$HOME/.claude/projects/$(pwd | tr '/.' '--')"

if [ -n "${TMUX_PANE:-}" ]; then
  pane=$TMUX_PANE
  say() { tmux send-keys -t "$pane" -l "$1"; tmux send-keys -t "$pane" Enter; }
  (
    while sleep 30; do
      [ -f STOP ] && exit 0
      newest=$(ls -t "$transcripts"/*.jsonl 2>/dev/null | head -1)
      [ -n "$newest" ] || continue
      mtime=$(stat -c %Y "$newest"); now=$(date +%s); quiet=$(( now - mtime ))
      # 1. The session asked for a fresh context and has gone quiet: /clear, then start again.
      if [ -f .claude/fresh ] && [ "$quiet" -ge 20 ]; then
        echo "watchdog: fresh context requested; clearing" >&2
        rm -f .claude/fresh
        say "/clear"; sleep 10; say "$prompt"
        continue
      fi
      # 2. A usage limit: nudge a minute after the reset time it printed.
      reset=$(tail -c 50000 "$newest" | grep -o 'resets [0-9:]*[ap]m (UTC)' | tail -1 | sed 's/resets //; s/ (UTC)//')
      if [ -n "$reset" ] && [ "$quiet" -ge 60 ]; then
        at=$(date -u -d "$(date -u -d @"$mtime" +%F) $reset" +%s 2>/dev/null || echo 0)
        [ "$at" -lt "$mtime" ] && at=$(( at + 86400 ))
        if [ "$now" -ge $(( at + 60 )) ] && [ "$at" -gt "$(( now - 6 * 3600 ))" ] && [ "$at" != "${handled:-}" ]; then
          echo "watchdog: usage limit reset at $reset; nudging" >&2
          handled=$at
          say "$nudge"; touch "$newest"
          continue
        fi
      fi
      # 3. Anything else that leaves the session idle for 30 minutes.
      if [ "$quiet" -ge 1800 ]; then
        echo "watchdog: transcript quiet for $((quiet / 60)) min, nudging" >&2
        say "$nudge"; touch "$newest"
      fi
    done
  ) &
  watchdog=$!
  trap 'kill $watchdog ${limiter:-} 2>/dev/null' EXIT
else
  echo "overnight.sh: not inside tmux, so no watchdog; a usage limit will leave the session idle" >&2
fi

while [ ! -f STOP ]; do
  git pull --ff-only || echo "overnight.sh: git pull failed; continuing with the local tree" >&2
  claude --model "$model" --dangerously-skip-permissions --remote-control -- "$prompt"
  [ -f STOP ] && break
  echo "overnight.sh: claude exited; starting a fresh session in 60 s (touch STOP to end)" >&2
  sleep 60
done
