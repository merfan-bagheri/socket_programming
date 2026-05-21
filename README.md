# 🚀 Client-Server Message Passing Implementation
## 📖 Project Overview
This project is a low-level C implementation of a client-server architecture capable of processing specific system commands and returning their outputs to the client. The primary goal of this project is to demonstrate advanced system programming concepts, including socket programming, I/O multiplexing, concurrency (multi-threading), and strict memory management.
### Protocol & Message Structure
The communication relies on a custom, strictly-defined binary application-layer protocol. This ensures predictable data parsing over stream sockets.
**Request Format:**
 * REQ_LEN (2 Bytes): Total length of the request (Header + Argument).
 * REQ_TYPE (2 Bytes): Type of the command requested.
 * REQ_ARG_LEN (2 Bytes): Length of the argument string.
 * REQ_ARG (Variable): The actual argument data (e.g., directory path or file name).
**Supported Commands (REQ_TYPE):**
 * REQUEST_TYPE_LS (1): Executes ls (lists directory contents) and returns the output.
 * REQUEST_TYPE_PWD (2): Executes pwd (prints working directory) and returns the output.
 * REQUEST_TYPE_CAT (3): Executes cat <file> (reads file contents) and returns the output.
**Response Format:**
 * RESPONSE_LEN (2 Bytes): Total length of the response message (Header + Data).
 * RESPONSE_DATA (Variable): The output string or file content returned by the server.
The project is divided into two distinct phases to compare different networking and concurrency models.
## 🏗️ Part 1: Single-Threaded & I/O Multiplexing
### Architecture & Approach
In this phase, the server operates on a **Single-Threaded** architecture using **UNIX Domain Sockets** (AF_UNIX). Instead of creating a new process or thread for every client, it handles concurrent connections using **I/O Multiplexing**.
### Methods, Tools, & Challenges Solved
 * **I/O Multiplexing (select()):** The server uses the select() system call to monitor multiple socket descriptors simultaneously. It only wakes up when a socket is ready for reading or writing, allowing a single thread to serve multiple clients without blocking.
 * **Non-Blocking Sockets:** All client sockets are set to non-blocking mode (O_NONBLOCK using fcntl()). This prevents the single server thread from hanging if a read or write operation cannot complete immediately.
 * **Custom Request Queue:** To fulfill the requirement that no request is lost or unanswered, parsed requests are placed into a custom Linked-List Queue. The server dequeues and processes them sequentially during its event loop.
 * **Stream Fragmentation Management (memmove):** A major challenge in stream sockets is partial reads/writes. I implemented a robust ClientState structure to manage custom read/write buffers. By using memmove, unread/unwritten bytes are safely shifted to the beginning of the buffer, perfectly handling partial socket writes without corrupting the stream.
 * **Smart Timeout Management:** To prevent CPU hogging (Busy-Waiting), the select() timeout dynamically adjusts. It is set to NULL (blocking) when the request queue is empty, and 0 when there are pending requests, ensuring zero CPU waste.
## 🧵 Part 2: Multi-Threaded TCP Server
### Architecture & Differences
In the second phase, the architecture shifts to a **Multi-Threaded** model using **TCP Sockets** (AF_INET).
 * **Differences from Part 1:** Instead of a single thread managing everything via select() on a local file path, the server binds to a network IP and Port. For every incoming client connection, the server spawns a dedicated POSIX thread (Thread-per-Client model) to handle the communication lifecycle.
### Methods, Tools, & Challenges Solved
 * **Thread Management:** Used pthread_create() to spawn threads and pthread_detach() to ensure thread resources are automatically reclaimed by the OS upon termination, preventing "zombie" threads and thread leaks.
 * **TCP Stream Safety (read_all / write_all):** Since TCP does not guarantee that a single write() or read() will transfer all requested bytes, I implemented robust read_all and write_all wrapper functions using while loops to guarantee complete payload delivery.
 * **Avoiding Race Conditions:** Multi-threading introduces the risk of Race Conditions. I avoided this by utilizing a **Shared-Nothing Architecture**. Socket descriptors are dynamically allocated (malloc) specifically for each thread. Because no global state or shared memory is mutated across threads, the code remains 100% thread-safe without the need for expensive Mutex locks.
## 💻 Compilation & Execution
### Part 1 (UNIX Domain / Single-Threaded)
**Compilation:**
```bash
gcc server.c -o server
gcc client.c -o client

```
**Execution:**
Start the server first, then run the client with desired commands.
```bash
./server
# In another terminal:
./client pwd
./client ls /var
./client cat /etc/passwd

```
### Part 2 (TCP / Multi-Threaded)
**Compilation:**
*Note: The -pthread flag is required to link the POSIX threads library.*
```bash
gcc server.c -o server -pthread
gcc client.c -o client

```
**Execution:**
```bash
./server
# In another terminal:
./client pwd
./client ls /root
./client cat /etc/hostname

```
## 🧪 Memory Leak Prevention & Testing
A strict requirement of this project was to ensure **zero memory leaks** in both the client and server. I utilized **Valgrind** to rigorously verify memory safety.
### Testing the Server
Run the server using Valgrind with full leak checking and origin tracking:
```bash
valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./server

```
**Methodology:** While the server is running under Valgrind, I sent dozens of valid and invalid requests from multiple clients simultaneously. After the tests, I shut down the server gracefully using SIGINT (Ctrl+C). The custom signal handlers ensure that all open sockets are closed, dynamically allocated buffers in the ClientState are freed, and any remaining nodes in the Request Queue are properly deallocated.
### Testing the Client
```bash
valgrind --leak-check=full ./client ls /
valgrind --leak-check=full ./client cat /etc/passwd
valgrind --leak-check=full ./client unknown_command

```
**Methodology:** The client was tested with various commands, including missing arguments and requests for extremely large files, to ensure realloc and malloc failure states were handled correctly without leaking memory.
**Conclusion:** Valgrind reports 0 bytes in 0 blocks lost for both applications. All dynamically allocated memory is explicitly freed.
## 📊 Stress Testing & Architecture Limitations
To evaluate how the system handles high-rate concurrent requests, I developed a Python script (stress_test.py) that utilizes ThreadPoolExecutor to bomb the server with thousands of binary-encoded requests.
By introducing a slight artificial processing delay (usleep) to simulate heavy disk I/O, the stress test revealed the theoretical limits of both architectures:
 1. **Single-Threaded Server (Part 1 - I/O Multiplexing):**
   * *Throughput:* Extremely fast for lightweight, non-blocking tasks.
   * *Limitation:* Highly vulnerable to **Blocking I/O**. Because there is only one thread, if one client requests a massive file via cat, the entire server blocks. Subsequent clients time out, causing the throughput to drop to near zero.
     
**Execution:**
Run the stress test for unix mode.
```bash
​python3 stress_test.py --mode unix --clients 200 --requests 10000
```
 2. **Multi-Threaded Server (Part 2 - TCP):**
   * *Throughput:* High, but carries slight overhead due to OS thread creation and context switching.
   * *Limitation:* It perfectly isolates blocking operations (a heavy cat request doesn't block other clients). However, under extreme load (e.g., 2000+ concurrent clients), it hits the **C10K Problem**. The OS runs out of file descriptors (hitting the ulimit) or thread stack memory, leading to Too many open files errors and dropped connections.

**Execution:**
Run the stress test for tcp mode.
```bash
​python3 stress_test.py --mode tcp --clients 200 --requests 10000
```

## ❓ Technical Q&A
**1. How does I/O Multiplexing work to handle multiple sockets in a single thread?**
I/O Multiplexing (via select()) allows the OS to monitor a set of file descriptors (sockets) and block the thread until at least one becomes "ready" (e.g., data has arrived or the buffer is free to write). Instead of sequentially waiting on read() for one client and ignoring others, the single thread asks the OS: *"Wake me up when ANY of these clients have data."* Once awake, it loops through the ready sockets, processes the available chunks of data, and goes back to monitoring, effectively multitasking without multi-threading.

**2. Can the queue be implemented to hold other data types? What is the best approach?**
Yes, the request queue currently uses a specific RequestNode struct tailored to this protocol. However, to make the queue generic and capable of holding *any* data type, the best approach in C is to use void * (void pointers) for the data payload inside the queue node.
For example:
```c
typedef struct Node {
    void *data;
    struct Node *next;
} Node;

```
This allows the queue to store pointers to any arbitrary struct (e.g., HTTP requests, database queries, or UDP datagrams). The consumer of the queue simply casts the void * back to the expected specific struct type before processing.
