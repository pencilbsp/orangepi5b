# Shared board plumbing for the measure-*.sh scripts. Source this; it expects
# $ROOT to point at the repo.
#
# Both measurements need a session that actually holds the GPU: over SSH there
# is no seat, so /dev/dri/renderD128 is root:render 0660 with an ACL only for
# whoever owns seat0. That is gdm-greeter at the login screen and the logged-in
# user afterwards, so resolve it at run time instead of assuming either.

read -r HOST PORT USER PASS < <(python3 -c "
import json
d = json.load(open('$ROOT/target.json'))
print(d['host'], d['port'], d['username'], d['password'])
")

ssh_board() { sshpass -p "$PASS" ssh -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=20 \
    -p "$PORT" "$USER@$HOST" "$@"; }

# Script on stdin. sudo -S eats the first line as the password, bash -s reads
# the rest, so the password never appears in a command line or in the log.
sudo_board() {
    { printf '%s\n' "$PASS"; cat; } | sshpass -p "$PASS" ssh \
        -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ConnectTimeout=20 -p "$PORT" "$USER@$HOST" \
        "sudo -S -p '' bash -s"
}

scp_board() { sshpass -p "$PASS" scp -o StrictHostKeyChecking=no \
    -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -P "$PORT" "$@"; }

# Sets SEAT_USER, SEAT_UID, SEAT_DISPLAY.
resolve_seat_session() {
    read -r SEAT_USER SEAT_UID SEAT_DISPLAY < <(sudo_board <<'EOF'
sess=$(loginctl list-sessions --no-legend | awk '$4 == "seat0" { print $1; exit }')
[ -n "$sess" ] || exit 0
user=$(loginctl show-session "$sess" -p Name --value)
uid=$(loginctl show-session "$sess" -p User --value)
disp=$(for p in $(pgrep -u "$user" -x gnome-shell 2>/dev/null); do
         tr '\0' '\n' < /proc/$p/environ | sed -n 's/^WAYLAND_DISPLAY=//p'
       done | head -1)
echo "$user" "$uid" "${disp:-wayland-0}"
EOF
)
    [[ -n "${SEAT_USER:-}" ]] || {
        echo "No session on seat0 -- the board needs to be at the login screen"
        echo "or logged in, otherwise nothing can reach the GPU."
        exit 1
    }
    echo "seat0: user=$SEAT_USER uid=$SEAT_UID display=$SEAT_DISPLAY"
}
