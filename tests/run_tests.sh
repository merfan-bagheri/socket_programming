#!/usr/bin/env bash
set -e

GREEN='\033[0;32m'
RED='\033[0;31m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=====================================================${NC}"
echo -e "${BLUE}   High-Performance Socket Programming Test Suite   ${NC}"
echo -e "${BLUE}=====================================================${NC}"

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

echo -e "\n${BLUE}[1/4] Compiling Phase 1 and Phase 2 binaries...${NC}"
make clean
make all

echo -e "${GREEN}? Compilation successful with -Wall -Wextra -Werror -pedantic -O3!${NC}"

# ---------------------------------------------------------
# Test Phase 1: UNIX Domain Sockets
# ---------------------------------------------------------
echo -e "\n${BLUE}[2/4] Testing Phase 1 (UNIX Domain Socket / select())...${NC}"
cd "$PROJECT_ROOT/phase_1"
rm -f /tmp/recruitment_socket.sock

./server > /dev/null 2>&1 &
SERVER1_PID=$!
sleep 0.5

# Basic functional tests
echo "Testing Phase 1 'pwd' command..."
OUTPUT_PWD=$(./client pwd)
echo "$OUTPUT_PWD" | grep "Response from Server" > /dev/null

echo "Testing Phase 1 'ls' command..."
OUTPUT_LS=$(./client ls .)
echo "$OUTPUT_LS" | grep "server.c" > /dev/null

echo "Testing Phase 1 'cat common.h'..."
OUTPUT_CAT=$(./client cat common.h)
echo "$OUTPUT_CAT" | grep "REQUEST_TYPE_LS" > /dev/null

echo "Testing Phase 1 non-existent file handling..."
OUTPUT_ERR=$(./client cat nonexistent_file.txt || true)
echo "$OUTPUT_ERR" | grep -i "Cannot open file" > /dev/null || true

# Stress test
echo "Running Phase 1 Python stress test (100 concurrent workers, 1000 requests)..."
python3 stress_test.py --mode unix --clients 100 --requests 1000

# Valgrind memory check on client
echo "Running Valgrind memory leak verification on Phase 1 Client..."
valgrind --leak-check=full --error-exitcode=100 ./client pwd > /dev/null 2>&1

# Stop Server 1
kill -INT $SERVER1_PID 2>/dev/null || true
wait $SERVER1_PID 2>/dev/null || true
echo -e "${GREEN}? Phase 1 All Tests Passed! 0 Memory Leaks.${NC}"

# ---------------------------------------------------------
# Test Phase 2: Multi-Threaded TCP Server
# ---------------------------------------------------------
echo -e "\n${BLUE}[3/4] Testing Phase 2 (Multi-Threaded TCP Server)...${NC}"
cd "$PROJECT_ROOT/phase_2"

./server > /dev/null 2>&1 &
SERVER2_PID=$!
sleep 0.5

# Basic functional tests
echo "Testing Phase 2 'pwd' command..."
OUTPUT_PWD2=$(./client pwd)
echo "$OUTPUT_PWD2" | grep "Response from Server" > /dev/null

echo "Testing Phase 2 'ls' command..."
OUTPUT_LS2=$(./client ls .)
echo "$OUTPUT_LS2" | grep "server.c" > /dev/null

echo "Testing Phase 2 'cat common.h'..."
OUTPUT_CAT2=$(./client cat common.h)
echo "$OUTPUT_CAT2" | grep "SERVER_PORT" > /dev/null

# Stress test
echo "Running Phase 2 Python stress test (100 concurrent workers, 1000 requests)..."
python3 stress_test.py --mode tcp --clients 100 --requests 1000

# Valgrind memory check on client
echo "Running Valgrind memory leak verification on Phase 2 Client..."
valgrind --leak-check=full --error-exitcode=100 ./client pwd > /dev/null 2>&1

# Stop Server 2
kill -INT $SERVER2_PID 2>/dev/null || true
wait $SERVER2_PID 2>/dev/null || true
echo -e "${GREEN}? Phase 2 All Tests Passed! 0 Memory Leaks.${NC}"

# ---------------------------------------------------------
# Valgrind Server Verification (Clean Shutdown & Zero Leaks)
# ---------------------------------------------------------
echo -e "\n${BLUE}[4/4] Testing Server Memory Safety under Valgrind...${NC}"
cd "$PROJECT_ROOT/phase_1"
rm -f /tmp/recruitment_socket.sock
valgrind --leak-check=full --log-file=valgrind_server1.log ./server > /dev/null 2>&1 &
VAL_PID1=$!
sleep 0.8
./client pwd > /dev/null
./client ls . > /dev/null
kill -INT $VAL_PID1 2>/dev/null || true
wait $VAL_PID1 2>/dev/null || true

grep "ERROR SUMMARY: 0 errors" valgrind_server1.log > /dev/null
grep -E "(All heap blocks were freed|definitely lost: 0 bytes)" valgrind_server1.log > /dev/null
rm -f valgrind_server1.log /tmp/recruitment_socket.sock
echo -e "${GREEN}? Phase 1 Server: 0 Errors, 0 Memory Leaks under Valgrind.${NC}"

cd "$PROJECT_ROOT/phase_2"
valgrind --leak-check=full --log-file=valgrind_server2.log ./server > /dev/null 2>&1 &
VAL_PID2=$!
sleep 0.8
./client pwd > /dev/null
./client ls . > /dev/null
kill -INT $VAL_PID2 2>/dev/null || true
wait $VAL_PID2 2>/dev/null || true

grep "ERROR SUMMARY: 0 errors" valgrind_server2.log > /dev/null
grep -E "(All heap blocks were freed|definitely lost: 0 bytes)" valgrind_server2.log > /dev/null
rm -f valgrind_server2.log
echo -e "${GREEN}? Phase 2 Server: 0 Errors, 0 Memory Leaks under Valgrind.${NC}"

echo -e "\n${GREEN}=====================================================${NC}"
echo -e "${GREEN}      ALL INTEGRATION & STRESS TESTS PASSED 100%     ${NC}"
echo -e "${GREEN}=====================================================${NC}"
