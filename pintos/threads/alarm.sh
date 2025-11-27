cd pintos/threads && make clean
make build/tests/threads/alarm-single.result
make build/tests/threads/alarm-priority.result
make build/tests/threads/alarm-simultaneous.result
make build/tests/threads/alarm-multiple.result
make build/tests/threads/alarm-negative.result
make build/tests/threads/alarm-zero.result
cd build && grep "PASS\\|FAIL" tests/threads/alarm-*.result
grep "PASS\\|FAIL" tests/threads/alarm-*.result