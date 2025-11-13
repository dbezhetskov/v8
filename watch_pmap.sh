#!/usr/bin/env bash
#
# watch-pmap.sh – repeatedly dump pmap content into LOG output every 3 seconds
#

process_name="cloudflare"
pid=$(
  ps -e | grep $process_name | awk '{print $1; exit}'
)

if [[ -z "$pid" ]]; then
  echo "Process '$process_name' is not running." >&2
  exit 1
fi

# --- main loop ---------------------------------------------------------------
while true; do
  pmap -X "$pid" &> LOG
  
  # Wait 3 seconds before the next snapshot
  sleep 3
done
