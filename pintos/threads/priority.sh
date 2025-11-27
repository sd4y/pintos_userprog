cd pintos/threads && make clean
make build/tests/threads/priority-change.result
make build/tests/threads/priority-preempt.result
make build/tests/threads/priority-fifo.result
make build/tests/threads/priority-sema.result
make build/tests/threads/priority-condvar.result
#make build/tests/threads/priority-donate-multiple.result
#make build/tests/threads/priority-donate-multiple2.result
#make build/tests/threads/priority-donate-nest.result
#make build/tests/threads/priority-donate-chain.result
#make build/tests/threads/priority-donate-sema.result
#make build/tests/threads/priority-donate-lower.result
cd build
grep "PASS\\|FAIL" tests/threads/priority-*.result