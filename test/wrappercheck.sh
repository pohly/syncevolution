#! /bin/bash
#
# wrappercheck.sh background command args ... -- command args ...
#
# Wrapper script which runs one command in the background and the
# other in the foreground. Once that second command completes, the
# first one is send a SIGINT+SIGTERM, then a SIGKILL until it terminates.
#
# Overall return code of this script is the return code of the
# foreground command or, if that is 0, the background command.

set -e
set -x
PS4='wrappercheck-$$ line ${LINENO}: '

PIDS=

trap "[ \"$PIDS\" ] && kill -TERM $PIDS" TERM
trap "[ \"$PIDS\" ] && kill -INT $PIDS" INT

DAEMON_LOG=
WAIT_FOR_DAEMON_OUTPUT=
WAIT_FOR_DBUS_DAEMON=

declare -a BACKGROUND
declare -a ENV
# parse parameters
while [ $# -gt 1 ] && [ "$1" != "--" ] ; do
    case "$1" in
        --daemon-log)
            shift
            DAEMON_LOG="$1"
            ;;
        --wait-for-dbus-daemon)
            shift
            WAIT_FOR_DBUS_DAEMON="$1"
            ;;
        --wait-for-daemon-output)
            shift
            WAIT_FOR_DAEMON_OUTPUT="$1"
            ;;
        --daemon-sleep)
            shift
            DAEMON_SLEEP="$1"
            ;;
        *=*)
            ENV[${#ENV[*]}]="$1"
            ;;
        *)
            break
            ;;
    esac
    shift
done
# gather command and its parameters
while [ $# -gt 1 ] && [ "$1" != "--" ] ; do
    BACKGROUND[${#BACKGROUND[*]}]="$1"
    shift
done
shift

if [ "$DAEMON_LOG" ] && [ "$WAIT_FOR_DAEMON_OUTPUT" ]; then
    daemonmatches=$(grep -e "$WAIT_FOR_DAEMON_OUTPUT" "$DAEMON_LOG" | wc -l)
fi

( set +x; echo >&2 "*** starting ${BACKGROUND[0]} as background daemon, output to ${DAEMON_LOG:-stderr}" )
# We need to create a process group so that we can kill all processes started by the sub-shell.
# ${BACKGROUND[*]} is used instead of ${BACKGROUND[@]} because although the later should have
# avoided expansion of words (good!) somehow the quoting got messed up in practice (bad!).
( set -x; exec >>${DAEMON_LOG:-&2} 2>&1; exec env "${ENV[@]}" setsid /bin/bash -c "set -x -o pipefail; ${BACKGROUND[*]} 2>&1 | $(dirname $0)/logger.py" ) &

BACKGROUND_PID=$!
PIDS+="$BACKGROUND_PID"

if [ "$DAEMON_LOG" ] && [ "$WAIT_FOR_DAEMON_OUTPUT" ]; then
    ( set +x; echo >&2 "*** waiting for daemon to write '$WAIT_FOR_DAEMON_OUTPUT' into $DAEMON_LOG"
        while [ $daemonmatches -eq $(grep -e "$WAIT_FOR_DAEMON_OUTPUT" "$DAEMON_LOG" | wc -l) ]; do
            if ! kill -0 $BACKGROUND_PID 2>/dev/null; then
                break
            fi
            sleep 1
        done
    )
fi

if [ "$WAIT_FOR_DBUS_DAEMON" ]; then
    ( set +x; echo >&2 "*** waiting for daemon to connect to D-Bus as '$WAIT_FOR_DBUS_DAEMON'"
      while ! (dbus-send --session --print-reply --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.ListNames | grep -q "$WAIT_FOR_DBUS_DAEMON"); do
          sleep 1
      done
    )
fi

if kill -0 $BACKGROUND_PID 2>/dev/null; then
    set +e
    if [ "$DAEMON_SLEEP" ]; then
        ( set +x; echo >&2 "*** 'sleep $DAEMON_SLEEP' for daemon to settle down" )
        sleep $DAEMON_SLEEP
    fi
    (set -x; "$@")
    RET=$?
    set -e
else
    echo >&2 "*** ${BACKGROUND[0]} terminated prematurely"
    RET=1
fi

( set +x; echo >&2 "*** killing and waiting for ${BACKGROUND[0]}" )
if kill -INT -$BACKGROUND_PID >&2 && kill -TERM -$BACKGROUND_PID >&2 || kill -INT $BACKGROUND_PID >&2 && kill -TERM $BACKGROUND_PID >&2; then
    perl -e "sleep(60); kill(9, -$BACKGROUND_PID);" &
    KILL_PID=$!
else
    ps x --forest >&2
    KILL_PID=
fi
set +e
wait $BACKGROUND_PID
SUBRET=$?
case $SUBRET in 0|130|137|139|143) SUBRET=0;; # 130 and 143 indicate that it was killed, probably by us, which is okay
esac
SUBRET=0 # TODO: don't ignore daemon results
if [ "$KILL_PID" ]; then
    msg=$(LC_ALL=C kill -KILL $KILL_PID 2>&1)
    if echo "$msg" | grep -q 'No such process'; then
        # Consider this a success.
        SUBRET=0
    else
        echo "$msg"
    fi
    wait $KILL_PID
fi
set -e
if [ $RET = 0 ]; then
    RET=$SUBRET
fi

exit $RET
