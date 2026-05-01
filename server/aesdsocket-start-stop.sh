#!/bin/sh

# Full path to the aesdsocket executable installed in the target rootfs.
DAEMON="/usr/bin/aesdsocket"

# PID file used by start-stop-daemon to track the running daemon process.
PIDFILE="/var/run/aesdsocket.pid"


# Start the aesdsocket daemon.
start() {
    # Print a status message without adding a newline yet.
    printf 'Starting aesdsocket: '

    # -S : start a new process
    # -q : quiet mode, suppress extra output
    # -m : create the PID file specified by -p
    # -b : run in background
    # -p : path to PID file
    # -x : executable to run
    # -- : everything after this is passed as arguments to the daemon
    # -d : run aesdsocket in daemon mode
    start-stop-daemon -S -q -m -b -p "$PIDFILE" -x "$DAEMON" -- -d

    # Save command return status.
    status=$?

    # Print OK or FAIL depending on command result.
    if [ "$status" -eq 0 ]; then
        echo "OK"
    else
        echo "FAIL"
    fi

    return "$status"
}


# Stop the aesdsocket daemon.
stop() {
    printf 'Stopping aesdsocket: '

    # -K : stop a running process
    # -q : quiet mode
    # -s TERM : send SIGTERM signal for graceful shutdown
    # -p : use PID from this PID file
    # -x : make sure it matches this executable
    #
    # This is the line that sends SIGTERM to aesdsocket.
    start-stop-daemon -K -q -s TERM -p "$PIDFILE" -x "$DAEMON"

    status=$?

    # If stop was successful, remove the stale PID file.
    if [ "$status" -eq 0 ]; then
        rm -f "$PIDFILE"
        echo "OK"
    else
        echo "FAIL"
    fi

    return "$status"
}


# Restart means stop first, then start again.
restart() {
    stop
    start
}


# Check the first command-line argument and call the matching function.
case "$1" in
    start)
        start
        ;;
    stop)
        stop
        ;;
    restart)
        restart
        ;;
    *)
        # Print help if the user gives an invalid argument.
        echo "Usage: $0 {start|stop|restart}"
        exit 1
        ;;
esac


# Exit successfully after handling the requested action.
exit 0