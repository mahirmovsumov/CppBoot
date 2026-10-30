#!/usr/bin/env bash
# Measures the framework on one CPU core with a 2 GB memory limit, with the load generator on the
# other cores. Run it from the repository root after building with -DCPPBOOT_BUILD_BENCHMARKS=ON:
#
#   scripts/benchmark-linux.sh build/bench
#
# It needs sudo, to set the memory limit and raise the limits on open files.
set -u

build_dir="${1:-build/bench}"
server_bin="$(realpath "$build_dir/benchmarks/cppboot-bench-server")"
load_bin="$(realpath "$build_dir/benchmarks/cppboot-load")"

cores="$(nproc)"
if [ "$cores" -lt 2 ]; then
    echo "This needs at least 2 CPU cores, so that the load generator has its own."
    exit 1
fi
load_cores="1-$((cores - 1))"
port=18090
run_number=0

echo "Machine: $cores cores, $(awk '/MemTotal/ {printf "%.1f", $2/1048576}' /proc/meminfo) GB RAM"
grep -m1 'model name' /proc/cpuinfo
echo "Core 0 shares a physical core with: $(cat /sys/devices/system/cpu/cpu0/topology/thread_siblings_list)"

sudo sysctl -q -w net.core.somaxconn=65535 net.ipv4.tcp_max_syn_backlog=65535 \
    net.ipv4.ip_local_port_range="1024 65535"
sudo prlimit --pid $$ --nofile=1048576:1048576

start_server() {
    run_number=$((run_number + 1))
    unit="cppboot-bench-$run_number"
    cgroup="/sys/fs/cgroup/system.slice/$unit.scope"
    peak_file="$(mktemp)"

    # A systemd scope applies a real memory limit, including the kernel's socket memory, and turns
    # off swap so that it can't be avoided.
    sudo systemd-run --quiet --scope --unit="$unit" -p MemoryMax=2G -p MemorySwapMax=0 \
        taskset -c 0 prlimit --nofile=1048576:1048576 \
        "$server_bin" "$@" >/tmp/bench-server.log 2>&1 &

    for _ in $(seq 1 100); do
        if bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            server_pid="$(pgrep -x cppboot-bench-s | head -n 1)"
            watch_memory &
            watcher_pid=$!
            return 0
        fi
        sleep 0.1
    done
    echo "The server didn't start:"
    cat /tmp/bench-server.log
    exit 1
}

# The cgroup disappears with the server, so this keeps its last readings for afterwards.
watch_memory() {
    while [ -d "$cgroup" ]; do
        {
            echo "peak=$(cat "$cgroup/memory.peak" 2>/dev/null)"
            grep -E '^(oom_kill|max) ' "$cgroup/memory.events" 2>/dev/null | tr ' ' '='
        } >"$peak_file.tmp" && mv "$peak_file.tmp" "$peak_file"
        sleep 0.2
    done
}

report_limit() {
    awk -F= '/^peak=/ {printf "  peak memory including kernel sockets: %.0f MB", $2/1048576}
             /^oom_kill=/ {printf ", killed by the limit: %s times", $2}
             END {print ""}' "$peak_file"
}

server_alive() {
    [ -d "/proc/$server_pid" ]
}

stop_server() {
    if server_alive; then
        sudo kill -TERM "$server_pid" 2>/dev/null
        for _ in $(seq 1 50); do
            server_alive || break
            sleep 0.1
        done
    fi
    sudo systemctl stop "$unit.scope" 2>/dev/null
    sudo systemctl reset-failed "$unit.scope" 2>/dev/null
    kill "$watcher_pid" 2>/dev/null
    wait "$watcher_pid" 2>/dev/null
    sleep 1
}

rss_mb() {
    awk '/VmRSS/ {printf "%.1f", $2/1024}' "/proc/$server_pid/status" 2>/dev/null
}

cgroup_mb() {
    awk '{printf "%.0f", $1/1048576}' "$cgroup/memory.current" 2>/dev/null
}

load() {
    taskset -c "$load_cores" "$load_bin" "$@"
}

echo
echo "=== Throughput and latency: 1 core, 1 network thread, 1 worker thread"
start_server --cppboot.server.worker_threads=1
echo "Memory at start: $(rss_mb) MB"
for target in /plaintext /json /users/7/posts/9 /db; do
    load --connections 64 --seconds 10 --target "$target"
done
for connections in 1 16 256 1024 4096 10000; do
    load --connections "$connections" --seconds 10 --target /plaintext --many-sources
    echo "  server memory: $(rss_mb) MB"
done
stop_server

echo
echo "=== Open connections with 2 GB: each size gets a fresh server. They open, wait, then all send"
echo "=== one request at the same moment, which is the worst case for memory."
for connections in 1000 10000 50000 100000 150000 200000 250000 300000; do
    start_server --cppboot.server.worker_threads=1
    base="$(rss_mb)"

    output_file="$(mktemp)"
    load --connections "$connections" --hold-idle --seconds 15 --many-sources \
        --connects-per-second 8000 >"$output_file" 2>&1 &
    load_pid=$!

    while kill -0 "$load_pid" 2>/dev/null && ! grep -q HOLDING "$output_file"; do
        sleep 0.5
    done
    sleep 8

    if server_alive; then
        memory="$(rss_mb)"
        total="$(cgroup_mb)"
        per_connection="$(awk -v m="$memory" -v b="$base" -v c="$connections" \
            'BEGIN {printf "%.1f", (m - b) * 1024 / c}')"
    else
        memory="gone"
        total="gone"
        per_connection="unknown"
    fi

    wait "$load_pid"
    tail -n 1 "$output_file"
    rm -f "$output_file"
    echo "  while idle: server $memory MB, with kernel sockets $total MB, $per_connection KB per connection"
    sleep 1
    report_limit

    survived=yes
    server_alive || survived=no
    echo "  server survived: $survived"
    stop_server

    if [ "$survived" = no ]; then
        break
    fi
done

echo
echo "=== Every connection active at once, with 2 GB"
for connections in 1000 10000 50000; do
    start_server --cppboot.server.worker_threads=1
    load --connections "$connections" --seconds 20 --target /plaintext --many-sources \
        --connects-per-second 8000
    echo "  server memory: $(rss_mb) MB, with kernel sockets $(cgroup_mb) MB"
    report_limit
    stop_server
done

echo
echo "=== A realistic load: a fixed 1,000 connections, reading from SQLite"
start_server --cppboot.server.worker_threads=1
load --connections 1000 --seconds 20 --target /db --many-sources
echo "  server memory: $(rss_mb) MB"
stop_server

echo
echo "Done."
