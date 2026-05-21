import socket
import struct
import time
import concurrent.futures
import argparse
from collections import Counter

# Protocol request type constants mapping to backend definitions
REQUEST_TYPE_LS = 1
REQUEST_TYPE_PWD = 2
REQUEST_TYPE_CAT = 3

# Server connection configurations
SOCKET_PATH = "/tmp/recruitment_socket.sock"
SERVER_IP = "127.0.0.1"
SERVER_PORT = 8080

def create_payload(req_type, arg=""):
    """
    Serializes request components into a binary payload matching the protocol structure.
    
    Header Format (!HHH): Big-Endian, 3 unsigned short integers (2 bytes each)
    - req_len: Total length of the packet (6 bytes header + variable argument length)
    - req_type: Integer ID representing the remote operation
    - arg_len: Length of the command argument string in bytes
    """
    arg_bytes = arg.encode('utf-8')
    arg_len = len(arg_bytes)
    req_len = 6 + arg_len
    header = struct.pack('!HHH', req_len, req_type, arg_len)
    return header + arg_bytes

def send_request(mode, req_type, arg=""):
    """
    Dispatches a single binary packet to the target server using the specified socket family.
    Measures duration and reads response stream boundaries safely.
    """
    payload = create_payload(req_type, arg)
    start_time = time.time()

    try:
        # Establish connection based on the targeted architecture mode
        if mode == 'unix':
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            # Short timeout ensuring stalled or backlogged connections fail fast
            sock.settimeout(2.0)
            sock.connect(SOCKET_PATH)
        else:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.settimeout(2.0)
            sock.connect((SERVER_IP, SERVER_PORT))

        # Transmit the entire network frame payload
        sock.sendall(payload)

        # Read the initial 2 bytes of the response header to determine the overall message length
        resp_hdr_data = sock.recv(2)
        if len(resp_hdr_data) < 2:
            return False, "Server closed connection early (Dropped by server)", time.time() - start_time

        # Unpack response length and calculate payload body size minus the length field itself
        resp_len = struct.unpack('!H', resp_hdr_data)[0]
        data_len = resp_len - 2

        # Consume the incoming payload body data stream completely
        received = 0
        while received < data_len:
            chunk = sock.recv(min(4096, data_len - received))
            if not chunk:
                break
            received += len(chunk)

        sock.close()
        return True, "Success", time.time() - start_time

    except socket.timeout:
        return False, "Timeout (Server overwhelmed)", time.time() - start_time
    except Exception as e:
        # Extract underlying OS error string (e.g., Connection refused, Too many open files)
        return False, str(e), time.time() - start_time

def run_stress_test(mode, concurrent_clients, total_requests):
    """
    Manages concurrent network client threads to measure throughput and failure rates under load.
    """
    print(f"[{mode.upper()} SERVER] Starting Stress Test...")
    print(f"Concurrent Workers: {concurrent_clients} | Total Requests: {total_requests}\n")

    start_time = time.time()
    success_count = 0
    error_reasons = Counter()
    response_times = []

    # Execute transactions concurrently using a thread pool worker architecture
    with concurrent.futures.ThreadPoolExecutor(max_workers=concurrent_clients) as executor:
        futures = []
        for i in range(total_requests):
            # Alternate commands across generated test tasks to diversify request load
            req_type = REQUEST_TYPE_PWD if i % 2 == 0 else REQUEST_TYPE_LS
            futures.append(executor.submit(send_request, mode, req_type, "."))

        # Collect async worker returns as they finish execution
        for future in concurrent.futures.as_completed(futures):
            success, msg, duration = future.result()
            if success:
                success_count += 1
                response_times.append(duration)
            else:
                error_reasons[msg] += 1 # Track exact system or protocol failure counts

    total_time = time.time() - start_time
    avg_time = (sum(response_times) / len(response_times)) * 1000 if response_times else 0

    # Real operational throughput is calculated strictly against successful request rounds
    real_throughput = success_count / total_time if total_time > 0 else 0

    print("-" * 50)
    print(f"RESULTS FOR {mode.upper()} ARCHITECTURE:")
    print(f"Total Execution Time  : {total_time:.2f} seconds")
    print(f"Successful Requests   : {success_count} / {total_requests}")
    print(f"Real Throughput       : {real_throughput:.2f} SUCCESSFUL requests/second")
    print(f"Average Response Time : {avg_time:.2f} ms (For successful reqs)")

    # Print a granular breakdown of runtime failure types if any were caught
    if error_reasons:
        print("\n--- Error Breakdown ---")
        for reason, count in error_reasons.items():
            print(f"- {reason}: {count} times")
    print("-" * 50)

if __name__ == "__main__":
    # Define and parse CLI arguments for test runner parameter configurations
    parser = argparse.ArgumentParser(description="Server Architecture Stress Test")
    parser.add_argument('--mode', choices=['unix', 'tcp'], required=True)
    parser.add_argument('--clients', type=int, default=100)
    parser.add_argument('--requests', type=int, default=5000)
    args = parser.parse_args()
    
    run_stress_test(args.mode, args.clients, args.requests)
