#!/usr/bin/env bash
# Installs a real application as a real systemd service, and checks how it behaves. It changes the
# machine it runs on, so it's meant for CI. It needs sudo, systemd and Docker, and an SDK from
# cmake --install:
#
#   scripts/test-linux-service.sh <sdk folder>
set -euo pipefail

sdk="$(realpath "$1")"
name=svc-demo
port=18100
url="http://127.0.0.1:$port"
work="$(mktemp -d)"
settings="/etc/$name/$name.env"
secret="a-long-random-secret-for-this-test-only-please"
password="service-test-password"

step() { echo; echo "=== $*"; }
fail() {
    echo "FAILED: $*"
    sudo systemctl status "$name" --no-pager || true
    sudo journalctl -u "$name" --no-pager -n 40 || true
    exit 1
}
expect_equal() { [ "$2" = "$3" ] || fail "$1: expected '$3', got '$2'"; echo "ok: $1 = $3"; }
service_property() { systemctl show "$name" -p "$1" --value; }
write_settings() { printf '%s\n' "$@" | sudo tee "$settings" >/dev/null; }

step "Create and package a project"
cd "$work"
"$sdk/bin/cppboot" create "$name" --web --sqlite --security --actuator
cd "$name"
"$sdk/bin/cppboot" package
tar -xzf "dist/$name-linux.tar.gz" -C "$work"
package="$work/$name"
ls -la "$package" "$package/lib"

step "The package runs in a container that has nothing but the base system"
docker run --rm -v "$package:/app:ro" -w /app ubuntu:24.04 sh -c "! ldd bin/$name | grep 'not found'"
output="$(docker run --rm -v "$package:/app:ro" -w /app \
    -e CPPBOOT_SECURITY_JWT_SECRET="$secret" -e CPPBOOT_MYAPP_PASSWORD="$password" \
    -e CPPBOOT_DATABASE_URL=sqlite:/tmp/test.db \
    ubuntu:24.04 "./bin/$name" --cppboot.server.port=0 --cppboot.runtime.shutdown_after_start=true 2>&1)"
echo "$output" | tail -n 4
grep -q "Application is running" <<<"$output" || fail "the package didn't run in a clean container"

step "Install it"
sudo "$package/install.sh"
expect_equal "account has no login" "$(getent passwd "$name" | cut -d: -f7)" /usr/sbin/nologin
expect_equal "settings are private" "$(sudo stat -c '%a %U %G' "$settings")" "640 root $name"
expect_equal "program belongs to root" "$(stat -c '%U' "/opt/$name/bin/$name")" root
expect_equal "enabled at boot" "$(systemctl is-enabled "$name")" enabled
grep -q "sqlite:/var/lib/$name/$name.db" <(sudo cat "$settings") || fail "no SQLite setting"

step "Without its secret, it refuses to start, and doesn't report success"
if sudo systemctl start "$name"; then
    fail "the service started with no secret"
fi
sudo systemctl reset-failed "$name"

step "With its settings, it's ready as soon as systemctl start returns"
write_settings "CPPBOOT_SERVER_PORT=$port" "CPPBOOT_SECURITY_JWT_SECRET=$secret" \
    "CPPBOOT_MYAPP_PASSWORD=$password" "CPPBOOT_DATABASE_URL=sqlite:/var/lib/$name/$name.db"
sudo systemctl start "$name"
# No waiting here on purpose. Type=notify means that it's already serving.
expect_equal "health straight after start" "$(curl -s -o /dev/null -w '%{http_code}' "$url/actuator/health")" 200
expect_equal "active" "$(service_property ActiveState)" active

step "It runs with limited access"
pid="$(service_property MainPID)"
expect_equal "runs as its own account" "$(ps -o user= -p "$pid")" "$name"
expect_equal "no new privileges" "$(sudo awk '/^NoNewPrivs/ {print $2}' "/proc/$pid/status")" 1
expect_equal "no capabilities" "$(sudo awk '/^CapEff/ {print $2}' "/proc/$pid/status")" 0000000000000000
sudo -u "$name" test -r "$settings" || fail "the service's account can't read its settings"
if sudo -u "$name" touch "/opt/$name/written" 2>/dev/null; then
    fail "the service's account can write to its program folder"
fi
echo "ok: the program folder is read-only to its account"
expect_equal "system folders are read-only" "$(service_property ProtectSystem)" strict
if ps -eo args | grep -v grep | grep -q "$secret"; then
    fail "the secret is visible in the process list"
fi
echo "ok: the secret isn't in the process list"
systemd-analyze security "$name" --no-pager | tail -n 1

step "It keeps data, and the log reaches the journal"
printf '{"name":"admin","password":"%s"}' "$password" >"$work/login.json"
token="$(curl -s --data-binary "@$work/login.json" "$url/api/login" | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])')"
curl -s -o /dev/null -w '%{http_code}\n' -H "Authorization: Bearer $token" "$url/api/me"
sudo journalctl -u "$name" --no-pager | grep -q "Application is running" || fail "no log in the journal"
test -f "/var/lib/$name/$name.db" || sudo test -f "/var/lib/$name/$name.db" || fail "no database file"
echo "ok: the database is in /var/lib"

step "A crash restarts it"
before="$(service_property MainPID)"
sudo kill -9 "$before"
for _ in $(seq 1 20); do
    sleep 0.5
    [ "$(service_property ActiveState)" = active ] && [ "$(service_property MainPID)" != "$before" ] && break
done
[ "$(service_property MainPID)" != "$before" ] || fail "it wasn't restarted"
expect_equal "serving again" "$(curl -s -o /dev/null -w '%{http_code}' "$url/actuator/health")" 200
expect_equal "restart count" "$(service_property NRestarts)" 1

step "Stopping is clean"
sudo systemctl stop "$name"
expect_equal "exit code" "$(service_property ExecMainStatus)" 0
expect_equal "result" "$(service_property Result)" success
sudo journalctl -u "$name" --no-pager | grep -q "Shutting down" || fail "it didn't shut down gracefully"

step "It serves HTTPS with settings from its file"
sudo sh -c "cd /etc/$name && openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout privkey.pem -out fullchain.pem -days 2 -subj /CN=localhost -addext subjectAltName=DNS:localhost 2>/dev/null \
    && chgrp $name privkey.pem fullchain.pem && chmod 640 privkey.pem fullchain.pem"
sudo cp "/etc/$name/fullchain.pem" "$work/fullchain.pem"
sudo chmod 644 "$work/fullchain.pem"
write_settings "CPPBOOT_SERVER_PORT=$port" "CPPBOOT_SECURITY_JWT_SECRET=$secret" \
    "CPPBOOT_MYAPP_PASSWORD=$password" "CPPBOOT_DATABASE_URL=sqlite:/var/lib/$name/$name.db" \
    "CPPBOOT_SERVER_TLS_CERTIFICATE_FILE=/etc/$name/fullchain.pem" \
    "CPPBOOT_SERVER_TLS_PRIVATE_KEY_FILE=/etc/$name/privkey.pem"
sudo systemctl start "$name"
expect_equal "HTTPS health" "$(curl -s --cacert "$work/fullchain.pem" --resolve "localhost:$port:127.0.0.1" \
    -o /dev/null -w '%{http_code}' "https://localhost:$port/actuator/health")" 200
sudo systemctl stop "$name"

step "Invalid settings don't restart forever"
write_settings "CPPBOOT_LOGGING_LEVEL=loud"
sudo systemctl reset-failed "$name" || true
if sudo systemctl start "$name"; then
    fail "the service started with invalid settings"
fi
sleep 4
expect_equal "exit code for invalid settings" "$(service_property ExecMainStatus)" 78
expect_equal "not restarted" "$(service_property NRestarts)" 0
sudo systemctl reset-failed "$name" || true

step "An upgrade keeps settings and data, and restarts"
write_settings "CPPBOOT_SERVER_PORT=$port" "CPPBOOT_SECURITY_JWT_SECRET=$secret" \
    "CPPBOOT_MYAPP_PASSWORD=$password" "CPPBOOT_DATABASE_URL=sqlite:/var/lib/$name/$name.db"
sudo systemctl start "$name"
before="$(service_property MainPID)"
sudo "$package/install.sh"
expect_equal "still active" "$(service_property ActiveState)" active
[ "$(service_property MainPID)" != "$before" ] || fail "the upgrade didn't restart it"
sudo grep -q "CPPBOOT_SECURITY_JWT_SECRET=$secret" "$settings" || fail "the upgrade changed the settings"
expect_equal "health after upgrade" "$(curl -s -o /dev/null -w '%{http_code}' "$url/actuator/health")" 200

step "Removing it keeps settings and data"
sudo "$package/uninstall.sh"
if systemctl is-active --quiet "$name"; then
    fail "it's still running"
fi
test ! -e "/opt/$name" || fail "the program is still there"
test ! -e "/etc/systemd/system/$name.service" || fail "the unit is still there"
sudo test -e "$settings" || fail "the settings were deleted"
sudo test -e "/var/lib/$name/$name.db" || fail "the data was deleted"

echo
echo "All service checks passed."
