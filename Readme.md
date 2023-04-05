# rvcpp

A simple RISC-V simulator which I'll hack on occasionally to get to grips with new parts of the architecture.

This isn't really in a fit state for anyone else to use, but the code is here if you are willing to brave it.

```
$ git clone git@github.com:Wren6991/rvcpp.git
$ cd rvcpp/rvcpp
$ make
g++ -std=c++17 -O3 -Wall rv.cpp -o rvcpp
$ ./rvcpp --help
Unrecognised argument --help
Usage: tb [--bin x.bin] [--dump start end] [--vcd x.vcd] [--cycles n] [--cpuret]
    --bin x.bin      : Flat binary file loaded to address 0x0 in RAM
    --vcd x.vcd      : Dummy option for compatibility with CXXRTL tb
    --dump start end : Print out memory contents between start and end (exclusive)
                       after execution finishes. Can be passed multiple times.
    --cycles n       : Maximum number of cycles to run before exiting.
    --memsize n      : Memory size in units of 1024 bytes, default is 16 MB
    --trace          : Print out execution tracing info
    --cpuret         : Testbench's return code is the return code written to
                       IO_EXIT by the CPU, or -1 if timed out.
$ cd ../test/riscv-tests
$ ./run-isa-tests.sh
...
$ echo $?
0
```
