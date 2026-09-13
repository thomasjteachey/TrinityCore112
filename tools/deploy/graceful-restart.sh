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
#
# EXCEPT when an operator has taken hold of the timer. `.server restart force
# 6000` sets the countdown to a hundred minutes on purpose - somebody is in the
# middle of something and does not want the realm to go down yet - and the old
# fixed margin here hard stopped it anyway about four minutes later, which made
# the hold useless. The realm now publishes who set the timer (see
# custom_graceful_shutdown.cpp), and a timer this deploy did not set is waited
# out on the operator's schedule instead of being overridden on ours.
#
# Waited out rather than aborted, deliberately. The pipeline stage that calls
# this reads
#
#     graceful-restart.sh ... || sudo systemctl stop "$SERVICE" || true
#
# so ANY non-zero exit here is turned straight back into the kill this is trying
# to avoid - an "I am held, please stop" exit code would reach the realm as a
# stop four minutes EARLIER than today, not later. Until that stage learns about
# a hold exit code, holding the line means staying in the wait loop.
#
# HOLD_MAX_SECONDS is the backstop so a forgotten hold cannot wedge a deploy for
# ever; it only ever applies while somebody else owns the timer.
set -Eeuo pipefail

HOLD_MAX_SECONDS="${GRACEFUL_HOLD_MAX_SECONDS:-7200}"   # 2h
# Validated, because an unusable value here disables the very backstop that stops
# a forgotten hold from wedging every future deploy: a non-numeric one makes the
# deadline test fail silently and the wait loop runs for ever.
case "$HOLD_MAX_SECONDS" in
  ''|*[!0-9]*)
    echo "[graceful] GRACEFUL_HOLD_MAX_SECONDS='$HOLD_MAX_SECONDS' is not a whole number of seconds; using 7200."
    HOLD_MAX_SECONDS=7200 ;;
esac

SERVICE="${1:?usage: graceful-restart.sh <service> <request-file> [seconds] [message...]}"
REQUEST_FILE="${2:?missing request file path}"
SECONDS_TO_GO="${3:-60}"
shift 3 || shift $# || true
MESSAGE="${*:-A new patch is on the way. The realm restarts in 1 minute - log out somewhere safe.}"

STATUS_FILE="${REQUEST_FILE}.status"

# Read one key out of the status file. Absent, vanishing mid-read, unreadable, or
# no sed on the box all yield the empty string, which every caller treats as "no
# information" - never as a hold and never as permission to stop.
#
# The '|| true' inside the brace group is load-bearing, not defensive noise. Under
# 'set -e' with 'pipefail' a failing sed makes the whole pipeline non-zero, and
# these are consumed as plain assignments (STATE="$(status_get state)"), so the
# script would EXIT - and the caller turns any non-zero exit into
# 'sudo systemctl stop', killing a live realm mid-countdown. The realm really can
# make sed fail: WriteStatus replaces this file, so there is a window in which it
# does not exist even though the -f test just passed.
status_get() {
  [ -f "$STATUS_FILE" ] || return 0
  { sed -n "s/^$1=//p" "$STATUS_FILE" 2>/dev/null || true; } | tail -n 1
}

# Digits only, else the fallback. Everything read out of the status file is fed
# through this before it reaches arithmetic or a -gt test: a truncated or garbled
# file must not be able to abort the script or wedge the wait loop.
num_or() {
  case "$1" in
    ''|*[!0-9]*) printf '%s' "$2" ;;
    *)           printf '%s' "$1" ;;
  esac
}

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
BASE_DEADLINE=$(( SECONDS_TO_GO + 180 ))
DEADLINE=$BASE_DEADLINE
WAITED=0
SEEN_STATUS=0
LAST_SERIAL=""
ANNOUNCED_SERIAL=""
SET_AT=0        # value of WAITED when the current timer was set
while systemctl is-active --quiet "$SERVICE"; do
  STATE="$(status_get state)"
  OURS="$(status_get ours)"
  LEFT="$(num_or "$(status_get seconds)" 0)"
  SERIAL="$(status_get serial)"
  if [ -n "$STATE" ]; then
    SEEN_STATUS=1
  fi

  # `seconds` is the time left when the timer was SET, not a live countdown, so
  # anchor to the moment it changed. Recomputing against the current elapsed
  # time would push the deadline out by ten seconds on every pass and the hard
  # stop would never arrive.
  if [ -n "$SERIAL" ] && [ "$SERIAL" != "$LAST_SERIAL" ]; then
    LAST_SERIAL="$SERIAL"
    SET_AT=$WAITED
  fi

  # Somebody in the game owns the timer: `.server restart force 6000` to push the
  # restart out, or a cancel. Either way it is not this deploy's to override, so
  # wait on THEIR schedule - the countdown they set plus the same save margin -
  # instead of stopping the realm at ours. A cancel leaves no countdown to wait
  # for, so that one just runs to the backstop.
  # Recomputed from the CURRENT state on every pass, never accumulated. A
  # deadline that could only grow meant one glimpse of state=cancelled pinned the
  # fallback at the backstop for the rest of the deploy, even after the operator
  # re-armed a short countdown. The floor is the margin we started with, so this
  # can never stop a realm sooner than the original contract.
  HELD_NOW=0
  DEADLINE=$BASE_DEADLINE
  if [ "$STATE" = "cancelled" ]; then
    HELD_NOW=1
    DEADLINE=$HOLD_MAX_SECONDS
  elif [ "$STATE" = "scheduled" ] && [ "$OURS" = "0" ]; then
    HELD_NOW=1
    DEADLINE=$(( SET_AT + LEFT + 180 ))
    if [ "$DEADLINE" -gt "$HOLD_MAX_SECONDS" ]; then
      DEADLINE=$HOLD_MAX_SECONDS
    fi
  elif [ "$STATE" = "scheduled" ] && [ "$OURS" = "1" ]; then
    # Our own countdown, possibly longer than we asked for: track it rather than
    # cut it short.
    DEADLINE=$(( SET_AT + LEFT + 180 ))
  fi
  if [ "$DEADLINE" -lt "$BASE_DEADLINE" ]; then
    DEADLINE=$BASE_DEADLINE
  fi

  # Say it once, when it starts, and once more if they change their mind.
  if [ "$HELD_NOW" = "1" ] && [ "$ANNOUNCED_SERIAL" != "${SERIAL:-}" ]; then
    ANNOUNCED_SERIAL="${SERIAL:-}"
    echo "[graceful] the shutdown timer is held from inside the game (state=$STATE, ${LEFT:-0}s)."
    echo "[graceful] waiting for it on the operator's schedule rather than stopping $SERVICE; backstop ${HOLD_MAX_SECONDS}s."
  fi

  if [ "$WAITED" -ge "$DEADLINE" ]; then
    if [ "$HELD_NOW" = "1" ]; then
      echo "[graceful] WARNING: held from inside the game for ${WAITED}s, past the ${DEADLINE}s this deploy was willing to wait."
      echo "[graceful] falling back to a hard stop so the deploy cannot wedge. Raise GRACEFUL_HOLD_MAX_SECONDS to wait longer."
    elif [ "$SEEN_STATUS" -eq 1 ]; then
      echo "[graceful] WARNING: still running ${WAITED}s after a countdown this deploy owns; hard stop."
    else
      echo "[graceful] WARNING: still running after ${WAITED}s and this build publishes no shutdown state."
      echo "[graceful] falling back to a hard stop; the NEXT deploy will be able to see an operator hold."
    fi
    sudo systemctl stop "$SERVICE" || true
    rm -f "$REQUEST_FILE" || true
    exit 0
  fi

  # Quietly, and not too often: this loop can run for ten minutes.
  sleep 10
  WAITED=$(( WAITED + 10 ))
  if [ $(( WAITED % 60 )) -eq 0 ]; then
    echo "[graceful] still up after ${WAITED}s of ${DEADLINE}s (state=${STATE:-unknown} ours=${OURS:-?})"
  fi
done

echo "[graceful] $SERVICE stopped cleanly after ${WAITED}s."

# It should have removed this itself; clearing it means a request can never be
# left behind to fire against the NEXT boot.
rm -f "$REQUEST_FILE" || true
