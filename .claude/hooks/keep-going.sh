#!/usr/bin/env bash
# Stop hook: keep the session on the campaign until a STOP file exists. There is no deadline.
# It also watches the context size: past FRESH_AT tokens it asks for a fresh context
# (LOOP.md, "Fresh contexts"), and it lets the turn end once .claude/fresh exists so that
# overnight.sh's watchdog can /clear the session and start it again from PLAN.md.
dir="${CLAUDE_PROJECT_DIR:-$(pwd)}"
input=$(cat)
if [ -f "$dir/STOP" ]; then
  # Allow one last turn to leave STATUS.md current, then let it stop.
  if [ ! -f "$dir/.claude/status-asked" ]; then
    touch "$dir/.claude/status-asked"
    printf '{"decision":"block","reason":"STOP requested. Bring STATUS.md and the Now line of PLAN.md up to date, commit and push, then stop."}\n'
  fi
  exit 0
fi
[ -f "$dir/.claude/fresh" ] && exit 0   # the watchdog takes it from here

# Context size: input + cache tokens of the last assistant message in the transcript.
transcript=$(printf '%s' "$input" | python3 -c 'import json,sys; print(json.load(sys.stdin).get("transcript_path",""))' 2>/dev/null)
ctx=$(tail -c 2000000 "$transcript" 2>/dev/null | python3 -c '
import json,sys
n=0
for l in sys.stdin:
    try: d=json.loads(l)
    except Exception: continue
    u=(d.get("message") or {}).get("usage") if d.get("type")=="assistant" else None
    if u and d["message"].get("model")!="<synthetic>":
        n=(u.get("input_tokens") or 0)+(u.get("cache_read_input_tokens") or 0)+(u.get("cache_creation_input_tokens") or 0)
print(n)' 2>/dev/null)
fresh_at=${FRESH_AT:-250000}
extra=""
if [ "${ctx:-0}" -ge "$fresh_at" ]; then
  extra=" Your context is ${ctx} tokens, past ${fresh_at}: every turn re-reads all of it. At the next clean point take a fresh context (LOOP.md, Fresh contexts): wait for or stop your subagents, commit and push, make the Now line exact, touch .claude/fresh, and end your turn."
fi
now=$(grep -m1 '^Now:' "$dir/PLAN.md" 2>/dev/null | cut -c6- | tr -d '"\\' | tr '\n' ' ' | cut -c1-400)
printf '{"decision":"block","reason":"Keep going. There is no deadline and no session end to plan around. Do not ask the user anything. Continue the campaign in PLAN.md (LOOP.md is the procedure). Now: %s%s"}\n' "$now" "$extra"
