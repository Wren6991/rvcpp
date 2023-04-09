#include <cstdio>
#include <fstream>

#include "rv_mem.h"
#include "rv_core.h"
#include "mmio/uart8250.h"
#include "mmio/mtimer.h"

// Minimal RISC-V interpreter, supporting:
// - RV32I
// - M
// - A
// - C
// - Zicsr
// - Zicntr
// - M-mode and S-mode traps
// - Sv32 virtual memory

#define RAM_SIZE_DEFAULT (256 * 1024 * 1024)
#define RAM_BASE         0x80000000u
#define IO_BASE          0xe0000000u
#define TBIO_BASE        (IO_BASE + 0x0000)
#define UART8250_BASE    (IO_BASE + 0x4000)
#define MTIMER_BASE      (IO_BASE + 0x8000)

const char *help_str =
"Usage: rvcpp [--bin x.bin [@addr]] [--dump start end] [--cycles n] [--cpuret]\n"
"    --bin x [@addr]  : Flat binary file loaded to absolute address addr.\n"
"                       If no address is provided, load to the beginning of RAM.\n"
"    --vcd x.vcd      : Dummy option for compatibility with CXXRTL tb\n"
"    --dump start end : Print out memory contents between start and end (exclusive)\n"
"                       after execution finishes. Can be passed multiple times.\n"
"    --cycles n       : Maximum number of cycles to run before exiting.\n"
"    --memsize n      : Memory size in units of 1024 bytes, default is 256 MB\n"
"    --trace          : Print out execution tracing info\n"
"    --ton-pc pc      : Enable tracing upon reaching address pc\n"
"                       (can be passed multiple times\n"
"    --toff-pc pc     : Disable tracing upon reaching address pc\n"
"                       (can be passed multiple times\n"
"    --ton-pc         : Enable tracing upon reaching address pc\n"
"    --cpuret         : Testbench's return code is the return code written to\n"
"                       IO_EXIT by the CPU, or -1 if timed out.\n";

void exit_help(const char *errtext) {
	fputs(errtext, stderr);
	fputs(help_str, stderr);
	exit(-1);
}

int main(int argc, char **argv) {
	if (argc < 2)
		exit_help("");

	std::vector<std::tuple<uint32_t, uint32_t>> dump_ranges;
	int64_t max_cycles = 100000;
	uint32_t ram_size = RAM_SIZE_DEFAULT;
	std::vector<std::string> bin_paths;
	std::vector<ux_t> bin_addrs;
	bool trace_execution = false;
	bool trace_on_pc = false;
	std::vector<ux_t> trace_on_pc_val;
	bool trace_off_pc = false;
	std::vector<ux_t> trace_off_pc_val;
	bool propagate_return_code = false;

	for (int i = 1; i < argc; ++i) {
		std::string s(argv[i]);
		if (s == "--bin") {
			if (argc - i < 2)
				exit_help("Option --bin requires an argument\n");
			bin_paths.push_back(argv[i + 1]);
			i += 1;
			if (argc - i >= 2 && argv[i + 1][0] == '@') {
				bin_addrs.push_back(std::stoul(&argv[i + 1][1], 0, 0));
				i += 1;
			} else {
				bin_addrs.push_back(RAM_BASE);
			}
		} else if (s == "--vcd") {
			if (argc - i < 2)
				exit_help("Option --vcd requires an argument\n");
			// (We ignore this argument, it's supported for compatibility with Hazard3 tb.
			i += 1;
		} else if (s == "--dump") {
			if (argc - i < 3)
				exit_help("Option --dump requires 2 arguments\n");
			dump_ranges.push_back(std::make_tuple(
				std::stoul(argv[i + 1], 0, 0),
				std::stoul(argv[i + 2], 0, 0)
			));
			i += 2;
		} else if (s == "--cycles") {
			if (argc - i < 2)
				exit_help("Option --cycles requires an argument\n");
			max_cycles = std::stol(argv[i + 1], 0, 0);
			i += 1;
		} else if (s == "--memsize") {
			if (argc - i < 2)
				exit_help("Option --memsize requires an argument\n");
			ram_size = 1024 * std::stol(argv[i + 1], 0, 0);
			i += 1;
		} else if (s == "--trace") {
			trace_execution = true;
		} else if (s == "--ton-pc") {
			trace_on_pc = true;
			if (argc - i < 2)
				exit_help("Option --ton-pc requires an argument\n");
			trace_on_pc_val.push_back(std::stol(argv[i + 1], 0, 0));
			i += 1;
		} else if (s == "--toff-pc") {
			trace_off_pc = true;
			if (argc - i < 2)
				exit_help("Option --toff-pc requires an argument\n");
			trace_off_pc_val.push_back(std::stol(argv[i + 1], 0, 0));
			i += 1;
		} else if (s == "--cpuret") {
			propagate_return_code = true;
		} else {
			fprintf(stderr, "Unrecognised argument %s\n", s.c_str());
			exit_help("");
		}
	}

	// Main RAM is handled inside of RVCore, but MMIO (and additional small
	// memories like boot RAMs) go in the memmap.
	TBMemIO io;
	MemMap32 mem;
	UART8250 uart;
	MTimer mtimer;
	mem.add(TBIO_BASE, 12, &io);
	mem.add(UART8250_BASE, 8, &uart);
	mem.add(MTIMER_BASE, 16, &mtimer);

	RVCore core(mem, RAM_BASE, RAM_BASE, ram_size);

	for (size_t i = 0; i < bin_paths.size(); ++i) {
		if (trace_execution || trace_on_pc) {
			printf("Loading file \"%s\" at %08x\n", bin_paths[i].c_str(), bin_addrs[i]);
		}
		std::ifstream fd(bin_paths[i], std::ios::binary | std::ios::ate);
		std::streamsize bin_size = fd.tellg();
		if (bin_size + bin_addrs[i] - RAM_BASE > ram_size) {
			fprintf(stderr, "Binary file (%ld bytes) loaded to %08x extends past end of memory (%08x through %08x)\n", bin_size, bin_addrs[i], RAM_BASE, RAM_BASE + ram_size - 1);
			return -1;
		} else if (bin_addrs[i] < RAM_BASE) {
			fprintf(stderr, "Binary file load address %08x is less than RAM base address %08x\n", bin_addrs[i], RAM_BASE);
			return -1;
		}
		fd.seekg(0, std::ios::beg);
		fd.read((char*)&core.ram[(bin_addrs[i] - RAM_BASE) >> 2], bin_size);
	}

	int64_t cyc;
	int rc = 0;
	try {
		for (cyc = 0; cyc < max_cycles || max_cycles == 0; ++cyc) {
			core.step(trace_execution);
			if (!(cyc & 0xfff)) {
				mtimer.step_time();
				core.csr.set_irq_t(mtimer.irq_status(0));
			}
			if (!trace_execution && trace_on_pc) {
				for (ux_t addr : trace_on_pc_val) {
					if (addr == core.pc) {
						printf("(Trace enabled at PC %08x)\n", addr);
						trace_execution = true;
					}
				}
			}
			if (trace_execution && trace_off_pc) {
				for (ux_t addr : trace_off_pc_val) {
					if (addr == core.pc) {
						printf("(Trace disabled at PC %08x)\n", addr);
						trace_execution = false;
					}
				}
			}
		}
		if (cyc == max_cycles) {
			printf("Timed out.\n");
			if (propagate_return_code)
				rc = -1;
		}
	}
	catch (TBExitException e) {
		printf("CPU requested halt. Exit code %d\n", e.exitcode);
		printf("Ran for %ld cycles\n", cyc + 1);
		if (propagate_return_code)
			rc = e.exitcode;
	}

	for (auto [start, end] : dump_ranges) {
		printf("Dumping memory from %08x to %08x:\n", start, end);
		for (uint32_t i = 0; i < end - start; ++i)
			printf("%02x%c", *core.r8(start + i), i % 16 == 15 ? '\n' : ' ');
		printf("\n");
	}

	return rc;
}
