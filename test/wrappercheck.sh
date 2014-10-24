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

trap "kill -TERM $PIDS" TERM
trap "kill -INT $PIDS" INT

DAEMON_LOG=
WAIT_FOR_DAEMON_OUTPUT=

declare -a BACKGROUND
declare -a ENV
# parse parameters
while [ $# -gt 1 ] && [ "$1" != "--" ] ; do
    case "$1" in
        --daemon-log)
            shift
            DAEMON_LOG="$1"
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
( set -x; exec >>${DAEMON_LOG:-&2} 2>&1; exec env "${ENV[@]}" "${BACKGROUND[@]}" ) &
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
kill -INT $BACKGROUND_PID && kill -TERM $BACKGROUND_PID || true
perl -e "sleep(60); kill(9, $BACKGROUND_PID);" &
KILL_PID=$!
set +e
wait $BACKGROUND_PID
msg=$(LC_ALL=C kill -KILL $KILL_PID 2>&1)
SUBRET=$?
if echo "$msg" | grep -q 'No such process'; then
    # Consider this a success.
    SUBRET=0
else
    echo "$msg"
fi
set -e
if [ $RET = 0 ]; then
    RET=$SUBRET
fi

exit $RET
