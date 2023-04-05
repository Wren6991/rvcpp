set -e

make -C ../../rvcpp
cd riscv-tests/isa
make XLEN=32 clean

# Note SKIP_V is something I added to their Makefile. We have no need for the
# virtual memory test machine configuration.
make -j$(nproc) XLEN=32 rv32ui rv32uc rv32um rv32ua rv32mi rv32si

echo ""
echo ">>> Running V environment tests"
echo ""
for test in $(find -name "*-v-*.bin"); do echo $test; ../../../../rvcpp/rvcpp --bin $test --cycles 100000 --cpuret; done

echo ""
echo ">>> Running P environment tests"
echo ""
for test in $(find -name "*-p-*.bin"); do echo $test; ../../../../rvcpp/rvcpp --bin $test --cycles 100000 --cpuret; done
