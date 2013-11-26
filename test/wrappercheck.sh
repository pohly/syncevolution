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

PIDS=

trap "kill -TERM $PIDS" TERM
trap "kill -INT $PIDS" INT

DAEMON_LOG=

declare -a BACKGROUND
declare -a ENV
# parse parameters
while [ $# -gt 1 ] && [ "$1" != "--" ] ; do
    case "$1" in
        --daemon-log)
            shift
            DAEMON_LOG="$1"
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

( set +x; echo >&2 "*** starting ${BACKGROUND[0]} as background daemon, output to ${DAEMON_LOG:-stderr}" )
( set -x; exec >>${DAEMON_LOG:-&2} 2>&1; exec env "${ENV[@]}" "${BACKGROUND[@]}" ) &
BACKGROUND_PID=$!
PIDS+="$BACKGROUND_PID"

set +e
(set -x; "$@")
RET=$?
set -e

( set +x; echo >&2 "*** killing and waiting for ${BACKGROUND[0]}" )
kill -INT $BACKGROUND_PID && kill -TERM $BACKGROUND_PID || true
( sleep 60; kill -KILL $BACKGROUND_PID ) &
KILL_PID=$!
set +e
wait $BACKGROUND_PID
msg=$(kill -KILL $KILL_PID 2>&1)
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
