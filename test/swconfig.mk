# Allow makefiles to find the test/ directory's path by following a chain of
# include ../swconfig.mk directives
SWTEST_ROOT         := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

SWTEST_COMMON       := $(SWTEST_ROOT)/common
SIM_DIR             := $(SWTEST_ROOT)/../rvcpp
SIM_EXEC            := $(SIM_DIR)/rvcpp

# Configure compiler and flags for all software tests (except e.g. riscv-tests
# which bring their own build system) 
SWTEST_CROSS_PREFIX := riscv32-unknown-elf-
SWTEST_MARCH        := rv32imac_zicsr_zifencei
SWTEST_LDSCRIPT     := $(SWTEST_COMMON)/memmap.ld
