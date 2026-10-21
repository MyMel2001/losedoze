#!/bin/sh
# loseshell session — native DRM/KMS compositor hosting a Wayland socket for
# the homemade Win32 subsystem (w32host). No X anywhere; init respawns this script.
. /etc/profile

export XDG_RUNTIME_DIR=/run
export WAYLAND_DISPLAY=losedoze0      # socket: /run/losedoze0 (created by loseshell)

# dev-time self-test (never shipped in release images); wait for the socket
if [ -x /etc/losedoze/autostart.sh ]; then
    (
        i=0
        while [ ! -S /run/losedoze0 ] && [ $i -lt 150 ]; do sleep 0.2; i=$((i + 1)); done
        if [ -S /run/losedoze0 ]; then
            echo "LOSEDOZE WAYLAND OK" > /dev/console
            /etc/losedoze/autostart.sh >> /var/log/autostart.log 2>&1
        else
            echo "LOSEDOZE WAYLAND FAIL" > /dev/console
        fi
    ) &
fi

while true; do
    /usr/bin/loseshell
    echo "loseshell exited; respawning" >> /var/log/session.log
    sleep 1
done
