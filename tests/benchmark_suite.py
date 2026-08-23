import socket
import struct
import time
import statistics
import subprocess
import os
import signal
import concurrent.futures
from collections import Counter

REQUEST_TYPE_LS = 1
REQUEST_TYPE_PWD = 2
REQUEST_TYPE_CAT = 3

SOCKET_PATH = "/tmp/recruitment_socket.sock"
SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080

def create_payload(req_type, arg=""):
    arg_bytes = arg.encode('utf-8')
    arg_len = len(arg_bytes)
    req_len = 6 + arg_len
    header = struct.pack('!HHH', req_len, req_type, arg_len)
    return header + arg_bytes

def send_request(mode, req_type, arg=""):
    payload = create_payload(req_type, arg)
    start_time = time.perf_counter()
    try:
        if mode == 'unix':
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.settimeout(3.0)
            sock.connect(SOCKET_PATH)
        else:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(3.0)
            sock.connect((SERVER_IP, SERVER_PORT))

        sock.sendall(payload)
        resp_hdr_data = sock.recv(2)
        if len(resp_hdr_data) < 2:
            return False, 0.0

        resp_len = struct.unpack('!H', resp_hdr_data)[0]
        data_len = resp_len - 2

        received = 0
        while received < data_len:
            chunk = sock.recv(min(4096, data_len - received))
            if not chunk:
                break
            received += len(chunk)

        sock.close()
        latency_ms = (time.perf_counter() - start_time) * 1000.0
        return True, latency_ms
    except Exception:
        return False, (time.perf_counter() - start_time) * 1000.0

def run_trial(mode, workers=100, requests=2000):
    start_time = time.perf_counter()
    success_count = 0
    latencies = []

    with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as executor:
        futures = []
        for i in range(requests):
            req_type = REQUEST_TYPE_PWD if i % 2 == 0 else REQUEST_TYPE_LS
            futures.append(executor.submit(send_request, mode, req_type, "."))

        for f in concurrent.futures.as_completed(futures):
            ok, lat = f.result()
            if ok:
                success_count += 1
                latencies.append(lat)

    total_duration = time.perf_counter() - start_time
    throughput = success_count / total_duration if total_duration > 0 else 0
    return {
        "success_count": success_count,
        "total_requests": requests,
        "duration": total_duration,
        "throughput": throughput,
        "latencies": latencies,
        "mean_latency": statistics.mean(latencies) if latencies else 0,
        "p50_latency": statistics.median(latencies) if latencies else 0,
        "p95_latency": statistics.quantiles(latencies, n=20)[18] if len(latencies) >= 20 else 0,
        "p99_latency": statistics.quantiles(latencies, n=100)[98] if len(latencies) >= 100 else 0,
    }

def benchmark_architecture(mode, name, binary_dir, num_trials=5, workers=100, requests_per_trial=2000):
    print(f"\n=======================================================")
    print(f"Benchmarking {name} ({mode.upper()})")
    print(f"Trials: {num_trials} | Workers: {workers} | Reqs/Trial: {requests_per_trial}")
    print(f"=======================================================")

    # Start server
    if mode == 'unix':
        try: os.unlink(SOCKET_PATH)
        except OSError: pass
    
    server_bin = os.path.join(binary_dir, "server")
    proc = subprocess.Popen([server_bin], cwd=binary_dir, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(0.6)

    # Warm-up run
    print("Executing warm-up run (500 requests)...")
    run_trial(mode, workers=50, requests=500)
    time.sleep(0.5)

    throughputs = []
    mean_latencies = []
    p95_latencies = []
    p99_latencies = []
    all_latencies = []
    total_success = 0
    total_requested = 0

    for t in range(1, num_trials + 1):
        print(f"  Trial {t}/{num_trials}...", end=" ", flush=True)
        res = run_trial(mode, workers=workers, requests=requests_per_trial)
        throughputs.append(res["throughput"])
        mean_latencies.append(res["mean_latency"])
        p95_latencies.append(res["p95_latency"])
        p99_latencies.append(res["p99_latency"])
        all_latencies.extend(res["latencies"])
        total_success += res["success_count"]
        total_requested += res["total_requests"]
        print(f"Throughput: {res['throughput']:.2f} req/s | Mean Latency: {res['mean_latency']:.2f} ms | Success: {res['success_count']}/{res['total_requests']}")
        time.sleep(0.3)

    # Terminate server
    proc.send_signal(signal.SIGINT)
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    avg_tp = statistics.mean(throughputs)
    std_tp = statistics.stdev(throughputs) if len(throughputs) > 1 else 0
    avg_lat = statistics.mean(mean_latencies)
    std_lat = statistics.stdev(mean_latencies) if len(mean_latencies) > 1 else 0
    overall_p95 = statistics.quantiles(all_latencies, n=20)[18] if len(all_latencies) >= 20 else 0
    overall_p99 = statistics.quantiles(all_latencies, n=100)[98] if len(all_latencies) >= 100 else 0
    success_rate = (total_success / total_requested) * 100.0

    print(f"\n--- Summary for {name} ---")
    print(f"Mean Throughput : {avg_tp:.2f} ? {std_tp:.2f} req/s")
    print(f"Mean Latency    : {avg_lat:.2f} ? {std_lat:.2f} ms")
    print(f"95th Percentile : {overall_p95:.2f} ms")
    print(f"99th Percentile : {overall_p99:.2f} ms")
    print(f"Success Rate    : {success_rate:.2f}% ({total_success}/{total_requested})")

    return {
        "avg_throughput": avg_tp,
        "std_throughput": std_tp,
        "avg_latency": avg_lat,
        "std_latency": std_lat,
        "p95_latency": overall_p95,
        "p99_latency": overall_p99,
        "success_rate": success_rate,
        "total_success": total_success,
        "total_requested": total_requested,
    }

if __name__ == "__main__":
    root_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    
    # Compile binaries
    subprocess.run(["make", "clean"], cwd=root_dir, check=True)
    subprocess.run(["make", "all"], cwd=root_dir, check=True)

    phase1_res = benchmark_architecture("unix", "Phase 1 (UNIX Domain / select)", os.path.join(root_dir, "phase_1"), num_trials=5, workers=100, requests_per_trial=2000)
    time.sleep(1)
    phase2_res = benchmark_architecture("tcp", "Phase 2 (Multi-Threaded TCP / pthreads)", os.path.join(root_dir, "phase_2"), num_trials=5, workers=100, requests_per_trial=2000)

    print("\n=======================================================")
    print("              FINAL BENCHMARK COMPARISON               ")
    print("=======================================================")
    print(f"| Metric | Phase 1 (UNIX Domain / select) | Phase 2 (Multi-Threaded TCP / pthreads) |")
    print(f"| :--- | :--- | :--- |")
    print(f"| **Transport** | `AF_UNIX` (Local IPC) | `AF_INET` (TCP :8080) |")
    print(f"| **Concurrency Model** | Single-threaded `select()` I/O Multiplexing | Multi-threaded POSIX `pthreads` |")
    print(f"| **Mean Throughput (5 runs)** | **{phase1_res['avg_throughput']:.2f} ? {phase1_res['std_throughput']:.2f} req/s** | **{phase2_res['avg_throughput']:.2f} ? {phase2_res['std_throughput']:.2f} req/s** |")
    print(f"| **Average Latency** | **{phase1_res['avg_latency']:.2f} ? {phase1_res['std_latency']:.2f} ms** | **{phase2_res['avg_latency']:.2f} ? {phase2_res['std_latency']:.2f} ms** |")
    print(f"| **p95 Latency** | **{phase1_res['p95_latency']:.2f} ms** | **{phase2_res['p95_latency']:.2f} ms** |")
    print(f"| **p99 Latency** | **{phase1_res['p99_latency']:.2f} ms** | **{phase2_res['p99_latency']:.2f} ms** |")
    print(f"| **Success Rate (10,000 reqs)** | **{phase1_res['success_rate']:.2f}% ({phase1_res['total_success']}/{phase1_res['total_requested']})** | **{phase2_res['success_rate']:.2f}% ({phase2_res['total_success']}/{phase2_res['total_requested']})** |")
