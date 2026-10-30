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

echo "Machine: $cores cores, $(awk '/MemTotal/ {printf "%.1f", $2/1048576}' /proc/meminfo) GB RAM"
grep -m1 'model name' /proc/cpuinfo

sudo sysctl -q -w net.core.somaxconn=65535 net.ipv4.tcp_max_syn_backlog=65535 \
    net.ipv4.ip_local_port_range="1024 65535" net.netfilter.nf_conntrack_max=2000000 2>/dev/null
sudo sysctl -q -w net.core.somaxconn=65535 net.ipv4.ip_local_port_range="1024 65535"
sudo prlimit --pid $$ --nofile=1048576:1048576
ulimit -n

start_server() {
    # A systemd scope applies a real memory limit, and turns off swap so it can't be avoided.
    sudo systemd-run --quiet --scope --unit=cppboot-bench -p MemoryMax=2G -p MemorySwapMax=0 \
        taskset -c 0 prlimit --nofile=1048576:1048576 \
        "$server_bin" "$@" >/tmp/bench-server.log 2>&1 &
    for _ in $(seq 1 100); do
        if bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            server_pid="$(pgrep -x cppboot-bench-s | head -n 1)"
            return 0
        fi
        sleep 0.1
    done
    echo "The server didn't start:"
    cat /tmp/bench-server.log
    exit 1
}

stop_server() {
    sudo kill -TERM "$server_pid" 2>/dev/null
    for _ in $(seq 1 100); do
        kill -0 "$server_pid" 2>/dev/null || break
        sudo kill -0 "$server_pid" 2>/dev/null || break
        sleep 0.1
    done
    sudo kill -KILL "$server_pid" 2>/dev/null
    sudo systemctl stop cppboot-bench.scope 2>/dev/null
    sudo systemctl reset-failed 2>/dev/null
    sleep 1
}

rss_mb() {
    awk '/VmRSS/ {printf "%.1f", $2/1024}' "/proc/$server_pid/status" 2>/dev/null
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
echo "=== Open connections with 2 GB: how many stay open and still work"
start_server --cppboot.server.worker_threads=1
base="$(rss_mb)"
echo "Memory with no connections: $base MB"
for connections in 1000 10000 50000 100000 200000 400000; do
    output_file="$(mktemp)"
    load --connections "$connections" --hold-idle --seconds 15 --many-sources \
        --connects-per-second 8000 >"$output_file" 2>&1 &
    load_pid=$!

    while kill -0 "$load_pid" 2>/dev/null && ! grep -q HOLDING "$output_file"; do
        sleep 0.5
    done
    sleep 10

    if [ ! -d "/proc/$server_pid" ]; then
        echo "connections=$connections: THE SERVER WAS STOPPED, probably by the 2 GB limit"
        wait "$load_pid"
        cat "$output_file" | tail -n 1
        rm -f "$output_file"
        break
    fi

    memory="$(rss_mb)"
    wait "$load_pid"
    tail -n 1 "$output_file"
    echo "  server memory: $memory MB, about $(awk -v m="$memory" -v b="$base" -v c="$connections" \
        'BEGIN {printf "%.1f", (m - b) * 1024 / c}') KB per connection"
    rm -f "$output_file"

    if [ ! -d "/proc/$server_pid" ]; then
        echo "The server stopped after that run, probably from the 2 GB limit."
        break
    fi
    sleep 3
done
sudo systemctl show cppboot-bench.scope -p MemoryPeak 2>/dev/null || true
if [ -d "/proc/$server_pid" ]; then
    stop_server
fi

echo
echo "=== Everything active at once: requests on every connection, with 2 GB"
start_server --cppboot.server.worker_threads=1
for connections in 20000 50000; do
    load --connections "$connections" --seconds 20 --target /plaintext --many-sources \
        --connects-per-second 8000
    echo "  server memory: $(rss_mb) MB"
    if [ ! -d "/proc/$server_pid" ]; then
        echo "The server stopped during that run."
        break
    fi
done
stop_server
echo
echo "Done."
