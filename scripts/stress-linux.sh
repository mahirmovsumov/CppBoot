#!/usr/bin/env bash
# Runs a long stress test against cppboot-stress-server, and fails on any unexpected response, a
# crash, or growth in memory, open files or threads. Build first with -DCPPBOOT_BUILD_BENCHMARKS=ON:
#
#   scripts/stress-linux.sh <build folder> <seconds> [http|https] [plain|sanitized]
#
# "sanitized" is for a build with AddressSanitizer or ThreadSanitizer. These change how memory
# behaves, so its growth isn't judged, and they report what they find through the exit code. Set
# STRESS_DATABASE_URL to use another database, and STRESS_IO_THREADS for the number of network threads.
set -u

build_dir="$1"
seconds="$2"
protocol="${3:-http}"
kind="${4:-plain}"

server_bin="$(realpath "$build_dir/benchmarks/cppboot-stress-server")"
client_bin="$(realpath "$build_dir/benchmarks/cppboot-stress-client")"
work="$(mktemp -d)"
port=18090
sample_every=10
failures=0

ulimit -n 65536 2>/dev/null

fail() {
    echo "FAILED: $*"
    failures=$((failures + 1))
}

server_args=(--cppboot.server.port="$port" --cppboot.logging.level=warn)
client_args=(--port "$port")
if [ -n "${STRESS_IO_THREADS:-}" ]; then
    server_args+=(--cppboot.server.io_threads="$STRESS_IO_THREADS")
fi
if [ -n "${STRESS_DATABASE_URL:-}" ]; then
    server_args+=(--cppboot.database.url="$STRESS_DATABASE_URL")
else
    server_args+=(--cppboot.database.url="sqlite:$work/stress.db")
fi
if [ "$protocol" = https ]; then
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
        -keyout "$work/key.pem" -out "$work/cert.pem" -days 2 -subj /CN=localhost 2>/dev/null
    server_args+=(--cppboot.server.tls.certificate_file="$work/cert.pem"
                  --cppboot.server.tls.private_key_file="$work/key.pem")
    client_args+=(--tls)
fi

start_server() {
    # Its log has an entry for each failure the client causes on purpose, so it only keeps the end.
    "$server_bin" "${server_args[@]}" "$@" > >(tail -n 200 >"$work/server.out") 2>"$work/server.err" &
    server_pid=$!
    for _ in $(seq 1 100); do
        if bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "The server didn't start:"
    cat "$work/server.out" "$work/server.err"
    exit 1
}

alive() { kill -0 "$server_pid" 2>/dev/null; }
memory_kb() { awk '/^VmRSS/ {print $2}' "/proc/$server_pid/status" 2>/dev/null; }
open_files() { ls "/proc/$server_pid/fd" 2>/dev/null | wc -l; }
threads() { awk '/^Threads/ {print $2}' "/proc/$server_pid/status" 2>/dev/null; }

echo "=== Stress for $seconds seconds over $protocol, $kind build"
start_server
idle_files="$(open_files)"
idle_threads="$(threads)"

"$client_bin" --seconds "$seconds" "${client_args[@]}" >"$work/client.out" 2>&1 &
client_pid=$!

: >"$work/samples"
elapsed=0
while kill -0 "$client_pid" 2>/dev/null; do
    sleep "$sample_every"
    elapsed=$((elapsed + sample_every))
    if ! alive; then
        break
    fi
    echo "$elapsed $(memory_kb) $(open_files) $(threads)" >>"$work/samples"
    if [ $((elapsed % 300)) -eq 0 ]; then
        echo "  $(tail -n 1 "$work/samples" | awk '{printf "%d s: %.1f MB, %d files, %d threads", $1, $2/1024, $3, $4}')"
    fi
done

wait "$client_pid"
client_code=$?
grep -v '^ok=' "$work/client.out" | tail -n 60
[ "$client_code" -eq 0 ] || fail "the client saw something unexpected"
alive || fail "the server died during the run"

if alive; then
    # Connections closed by the client can take a moment to finish on the server.
    sleep 15
    after_files="$(open_files)"
    after_threads="$(threads)"
    echo "After the load: $(memory_kb) KB, $after_files open files (was $idle_files when idle), $after_threads threads (was $idle_threads)"
    [ "$after_files" -le $((idle_files + 12)) ] || fail "open files grew from $idle_files to $after_files"

    if [ "$kind" = plain ]; then
        # The first part is warm-up, while caches and pools fill.
        awk -v seconds="$seconds" '
            $1 > seconds * 0.2 {
                count++
                value[count] = $2
            }
            END {
                if (count < 4) {
                    print "too short to judge memory"
                    exit 0
                }
                half = int(count / 2)
                for (i = 1; i <= half; i++) { first += value[i] }
                for (i = half + 1; i <= count; i++) { second += value[i] }
                first /= half
                second /= (count - half)
                printf "Memory: %.1f MB on average early on, %.1f MB later, a change of %.1f MB\n",
                    first / 1024, second / 1024, (second - first) / 1024
                allowed = first * 0.10
                if (allowed < 8192) { allowed = 8192 }
                exit (second - first > allowed) ? 1 : 0
            }' "$work/samples" || fail "memory kept growing"
    fi

    max_files="$(awk 'BEGIN {max = 0} $3 > max {max = $3} END {print max}' "$work/samples")"
    max_threads="$(awk 'BEGIN {max = 0} $4 > max {max = $4} END {print max}' "$work/samples")"
    echo "Largest during the run: $max_files open files, $max_threads threads"
    # Both thread pools are fixed.
    [ "$max_threads" -le $((idle_threads + 8)) ] || fail "threads grew from $idle_threads to $max_threads"

    kill -TERM "$server_pid"
    wait "$server_pid"
    code=$?
    echo "Exit code after the run: $code"
    [ "$code" -eq 0 ] || fail "exit code $code after a clean stop. A sanitizer may have found something"
fi

echo
echo "=== Stopping while under load"
start_server
"$client_bin" --seconds 25 "${client_args[@]}" >/dev/null 2>&1 &
client_pid=$!
sleep 10
stop_started="$(date +%s)"
kill -TERM "$server_pid"
wait "$server_pid"
code=$?
stop_seconds=$(($(date +%s) - stop_started))
echo "Stopped in $stop_seconds seconds, with exit code $code"
[ "$code" -eq 0 ] || fail "exit code $code when stopped under load"
[ "$stop_seconds" -le 40 ] || fail "stopping took $stop_seconds seconds"
wait "$client_pid"

if [ "$kind" = plain ]; then
    echo
    echo "=== More connections than max_connections allows: the rest are refused, and it recovers"
    start_server --cppboot.server.max_connections=200
    held_memory="$(memory_kb)"
    "$(dirname "$client_bin")/cppboot-load" --port "$port" --connections 5000 --hold-idle --seconds 5 \
        --connects-per-second 1000 $([ "$protocol" = https ] && echo --tls) --many-sources \
        >"$work/flood.out" 2>&1
    tail -n 1 "$work/flood.out"
    alive || fail "too many connections killed the server"
    echo "Memory: $held_memory KB before, $(memory_kb) KB after"
    "$(dirname "$client_bin")/cppboot-load" --port "$port" --connections 32 --seconds 3 \
        $([ "$protocol" = https ] && echo --tls) --target /plaintext >"$work/after.out" 2>&1
    tail -n 1 "$work/after.out"
    grep -q 'connect_errors=0 request_errors=0 bad_statuses=0' "$work/after.out" ||
        fail "the server didn't recover after too many connections"
    kill -TERM "$server_pid"
    wait "$server_pid"
fi

if grep -E 'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error' "$work/server.err" >/dev/null; then
    grep -E -A 12 'AddressSanitizer|LeakSanitizer|ThreadSanitizer|runtime error' "$work/server.err" | head -n 80
    fail "a sanitizer reported a problem"
fi

rm -rf "$work"
echo
if [ "$failures" -eq 0 ]; then
    echo "Stress test passed."
    exit 0
fi
echo "Stress test failed, with $failures problems."
exit 1
