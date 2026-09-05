#!/usr/bin/env bash
#
# Ask a realm to shut down politely, wait for it, and leave it stopped.
#
#   graceful-restart.sh <service> <request-file> [seconds] [message...]
#
# Replaces "systemctl stop" in a deploy. The worldserver watches <request-file>
# (Centurion.Shutdown.RequestFile) and, on seeing one, tells everybody online
# why the realm is going down and then runs TrinityCore's own countdown - which
# warns clients, blocks new logins near the end, and saves on the way out.
#
# It exits 0, and the units here are Restart=on-failure, so the realm stays
# down afterwards and the installer is free to write over the binary. That is
# the whole reason this does not use a signal: SIGTERM stops the process at
# once with no countdown and no explanation to anybody playing.
#
# Falls back to a hard stop if the realm does not go down in time, so a deploy
# can never be wedged by this. A realm that is already stopped is fine - the
# wait loop simply ends immediately.
set -Eeuo pipefail

SERVICE="${1:?usage: graceful-restart.sh <service> <request-file> [seconds] [message...]}"
REQUEST_FILE="${2:?missing request file path}"
SECONDS_TO_GO="${3:-60}"
shift 3 || shift $# || true
MESSAGE="${*:-A new patch is on the way. The realm restarts in 1 minute - log out somewhere safe.}"

# Nothing to be polite to.
if ! systemctl is-active --quiet "$SERVICE"; then
  echo "[graceful] $SERVICE is already stopped; nothing to announce."
  exit 0
fi

echo "[graceful] asking $SERVICE to shut down in ${SECONDS_TO_GO}s"
echo "[graceful] message: $MESSAGE"

# The worldserver runs as its own user and only needs to READ this, but it also
# deletes it once it has been honoured - so the directory has to be writable by
# that user, not just the file.
printf '%s\n%s\n' "$SECONDS_TO_GO" "$MESSAGE" > "$REQUEST_FILE"
chmod 0664 "$REQUEST_FILE" || true

# Is anything actually listening?
#
# The watcher deletes the request the moment it reads it, so the file still
# being there after a few poll intervals means this realm is running a build
# from before the watcher existed, or has the config key unset. Without this
# probe the first deploy after either of those waits out the entire countdown
# and the margin - thirteen minutes - before falling back to the hard stop it
# was always going to do.
PROBE=0
while [ -f "$REQUEST_FILE" ]; do
  if [ "$PROBE" -ge 15 ]; then
    echo "[graceful] request not picked up in ${PROBE}s - this build has no shutdown watcher."
    echo "[graceful] falling back to a hard stop; the NEXT deploy will be graceful."
    rm -f "$REQUEST_FILE" || true
    sudo systemctl stop "$SERVICE" || true
    exit 0
  fi
  sleep 3
  PROBE=$(( PROBE + 3 ))
done
echo "[graceful] request accepted after ${PROBE}s; the realm is counting down."

# Give it the countdown plus a margin for the save on the way out. The margin is
# generous on purpose: a realm with a large fleet online spends real time saving
# characters, and cutting that short is exactly what this script exists to stop.
DEADLINE=$(( SECONDS_TO_GO + 180 ))
WAITED=0
while systemctl is-active --quiet "$SERVICE"; do
  if [ "$WAITED" -ge "$DEADLINE" ]; then
    echo "[graceful] WARNING: still running after ${WAITED}s; falling back to a hard stop."
    sudo systemctl stop "$SERVICE" || true
    rm -f "$REQUEST_FILE" || true
    exit 0
  fi
  # Quietly, and not too often: this loop can run for ten minutes.
  sleep 10
  WAITED=$(( WAITED + 10 ))
  if [ $(( WAITED % 60 )) -eq 0 ]; then
    echo "[graceful] still up after ${WAITED}s of ${DEADLINE}s"
  fi
done

echo "[graceful] $SERVICE stopped cleanly after ${WAITED}s."

# It should have removed this itself; clearing it means a request can never be
# left behind to fire against the NEXT boot.
rm -f "$REQUEST_FILE" || true
