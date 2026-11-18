#!/usr/bin/env bash
# Compares builds of the benchmark server on the same machine, and shows where one of them spends
# its time. Separate runs of the Benchmark workflow land on different machines, whose results differ
# by up to 4 times, so only builds measured side by side can be compared.
#
#   scripts/compare-linux.sh LOAD_BIN NAME=SERVER_BIN [NAME=SERVER_BIN ...]
#
# Each round measures every build in turn, so a machine that slows down partway affects them all
# alike. The last build is the one that's profiled. The server gets core 0, with 1 network thread and
# 1 worker thread, and the load generator gets the other cores.
#
# It needs sudo, for perf and strace. Without them, the profile is skipped.
set -u

load_bin="$(realpath "$1")"
shift
names=()
servers=()
for variant in "$@"; do
    names+=("${variant%%=*}")
    servers+=("$(realpath "${variant#*=}")")
done

rounds="${ROUNDS:-5}"
seconds="${SECONDS_PER_RUN:-8}"
targets=(/plaintext /users/7/posts/9)
port=18090
cores="$(nproc)"
load_cores="1-$((cores - 1))"

echo "Machine: $cores cores"
grep -m1 'model name' /proc/cpuinfo
sudo prlimit --pid $$ --nofile=1048576:1048576

start_server() {
    taskset -c 0 "$1" --cppboot.server.worker_threads=1 >/tmp/compare-server.log 2>&1 &
    server_pid=$!
    for _ in $(seq 1 100); do
        if bash -c "exec 3<>/dev/tcp/127.0.0.1/$port" 2>/dev/null; then
            return 0
        fi
        sleep 0.1
    done
    echo "The server didn't start:"
    cat /tmp/compare-server.log
    exit 1
}

stop_server() {
    kill -TERM "$server_pid" 2>/dev/null
    wait "$server_pid" 2>/dev/null
    sleep 1
}

load() {
    taskset -c "$load_cores" "$load_bin" "$@"
}

field() {
    sed -n "s/.*$1=\([0-9]*\).*/\1/p"
}

# Sums a counter over every thread of the server.
thread_sum() {
    cat /proc/"$server_pid"/task/*/status 2>/dev/null | awk -v key="$1:" '$1 == key {s += $2} END {print s + 0}'
}

# User and system CPU time of the server, in clock ticks.
cpu_ticks() {
    awk '{print $14, $15}' "/proc/$server_pid/stat"
}

echo
echo "=== Requests per second, 64 connections, ${seconds} s each, ${rounds} rounds"
results="$(mktemp)"
for round in $(seq 1 "$rounds"); do
    for index in "${!servers[@]}"; do
        start_server "${servers[$index]}"
        load --connections 64 --seconds 2 --target /plaintext >/dev/null
        for target in "${targets[@]}"; do
            line="$(load --connections 64 --seconds "$seconds" --target "$target")"
            rps="$(echo "$line" | field requests_per_second)"
            p99="$(echo "$line" | field p99_us)"
            echo "round $round  ${names[$index]}  $target  $rps/s  p99 ${p99} us"
            echo "${names[$index]} $target $rps $p99" >>"$results"
        done
        stop_server
    done
done

echo
echo "=== Medians"
printf '%-12s %-20s %12s %10s\n' build target requests/s "p99 us"
for target in "${targets[@]}"; do
    for name in "${names[@]}"; do
        rps="$(awk -v n="$name" -v t="$target" '$1 == n && $2 == t {print $3}' "$results" | sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}')"
        p99="$(awk -v n="$name" -v t="$target" '$1 == n && $2 == t {print $4}' "$results" | sort -n | awk '{a[NR] = $1} END {print a[int((NR + 1) / 2)]}')"
        printf '%-12s %-20s %12s %10s\n' "$name" "$target" "$rps" "$p99"
    done
done

echo
echo "=== What a request costs the server: /plaintext, 64 connections, ${seconds} s"
ticks_per_second="$(getconf CLK_TCK)"
printf '%-12s %10s %12s %12s %16s %16s\n' build requests "user us/req" "sys us/req" "voluntary sw/req" "forced sw/req"
for index in "${!servers[@]}"; do
    start_server "${servers[$index]}"
    load --connections 64 --seconds 2 --target /plaintext >/dev/null
    read -r user_before system_before <<<"$(cpu_ticks)"
    voluntary_before="$(thread_sum voluntary_ctxt_switches)"
    forced_before="$(thread_sum nonvoluntary_ctxt_switches)"
    line="$(load --connections 64 --seconds "$seconds" --target /plaintext)"
    read -r user_after system_after <<<"$(cpu_ticks)"
    voluntary_after="$(thread_sum voluntary_ctxt_switches)"
    forced_after="$(thread_sum nonvoluntary_ctxt_switches)"
    requests=$(($(echo "$line" | field requests_per_second) * seconds))
    awk -v name="${names[$index]}" -v r="$requests" -v hz="$ticks_per_second" \
        -v u=$((user_after - user_before)) -v s=$((system_after - system_before)) \
        -v v=$((voluntary_after - voluntary_before)) -v f=$((forced_after - forced_before)) \
        'BEGIN {printf "%-12s %10d %12.2f %12.2f %16.2f %16.2f\n", name, r, u * 1e6 / hz / r, s * 1e6 / hz / r, v / r, f / r}'
    stop_server
done

profiled="${names[-1]}"
server_bin="${servers[-1]}"

echo
echo "=== System calls for each request: $profiled, /plaintext, 64 connections, 5 s under strace"
if command -v strace >/dev/null; then
    start_server "$server_bin"
    load --connections 64 --seconds 2 --target /plaintext >/dev/null
    sudo strace -c -f -p "$server_pid" -o /tmp/compare-strace.txt &
    strace_pid=$!
    sleep 1
    line="$(load --connections 64 --seconds 5 --target /plaintext)"
    sudo kill -INT "$strace_pid"
    wait "$strace_pid" 2>/dev/null
    requests=$(($(echo "$line" | field requests_per_second) * 5))
    echo "requests: $requests (strace slows the server, so only the counts matter)"
    awk -v r="$requests" '$NF ~ /^[a-z_0-9]+$/ && $4 ~ /^[0-9]+$/ && $NF != "total" {printf "%-20s %10d %8.2f per request\n", $NF, $4, $4 / r}' \
        /tmp/compare-strace.txt | sort -k2 -n -r | head -n 15
    stop_server
else
    echo "strace isn't installed, so this is skipped."
fi

echo
echo "=== Where $profiled spends its time: /plaintext, 64 connections, ${seconds} s"
if command -v perf >/dev/null && perf version >/dev/null 2>&1; then
    sudo sysctl -q -w kernel.perf_event_paranoid=-1 kernel.kptr_restrict=0
    start_server "$server_bin"
    load --connections 64 --seconds 2 --target /plaintext >/dev/null
    sudo perf record -q -e cpu-clock -F 1999 -g -p "$server_pid" -o /tmp/compare.perf -- sleep "$seconds" &
    perf_pid=$!
    load --connections 64 --seconds "$seconds" --target /plaintext >/dev/null
    wait "$perf_pid"
    stop_server

    echo
    echo "--- By library or kernel"
    sudo perf report -i /tmp/compare.perf --no-children --sort dso --stdio -g none 2>/dev/null | grep -v -e '^#' -e '^$' 2>/dev/null | head -n 12
    echo
    echo "--- By function, own time only"
    sudo perf report -i /tmp/compare.perf --no-children --sort sym --stdio -g none 2>/dev/null | grep -v -e '^#' -e '^$' 2>/dev/null | head -n 45
    echo
    echo "--- By function, including what it calls"
    sudo perf report -i /tmp/compare.perf --children --sort sym --stdio -g none 2>/dev/null | grep -v -e '^#' -e '^$' 2>/dev/null | head -n 45
else
    echo "perf isn't available on this kernel, so this is skipped."
fi

echo
echo "Done."
