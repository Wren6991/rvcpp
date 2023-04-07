ifndef SRCS
$(error Must define list of test sources as SRCS)
endif

ifndef APP
$(error Must define application name as APP)
endif

CCFLAGS      ?=
INCDIR       ?= $(SWTEST_COMMON)
MAX_CYCLES   ?= 100000
TMP_PREFIX   ?= tmp/

###############################################################################

.SUFFIXES:
.PHONY: all run trace view tb clean clean_tb

all: run

run: $(TMP_PREFIX)$(APP).bin
	$(SIM_EXEC) --bin $(TMP_PREFIX)$(APP).bin --vcd $(TMP_PREFIX)$(APP)_run.vcd --cycles $(MAX_CYCLES)

trace:
	$(SIM_EXEC) --bin $(TMP_PREFIX)$(APP).bin --trace --cycles $(MAX_CYCLES) > $(TMP_PREFIX)$(APP)_run.log

annotate: trace
	$(SWTEST_SCRIPTS)/annotate_trace.py $(TMP_PREFIX)$(APP)_run.log $(TMP_PREFIX)$(APP)_run_annotated.log -d $(TMP_PREFIX)$(APP).dis

view: run
	gtkwave $(TMP_PREFIX)$(APP)_run.vcd

bin: $(TMP_PREFIX)$(APP).bin

tb:
	$(MAKE) -C $(SIM_DIR)

clean:
	rm -rf $(TMP_PREFIX)

clean_tb: clean
	$(MAKE) -C $(SIM_DIR) clean

###############################################################################

$(TMP_PREFIX)$(APP).bin: $(TMP_PREFIX)$(APP).elf
	$(SWTEST_CROSS_PREFIX)objcopy -O binary $^ $@
	$(SWTEST_CROSS_PREFIX)objdump -h $^ > $(TMP_PREFIX)$(APP).dis
	$(SWTEST_CROSS_PREFIX)objdump -d $^ >> $(TMP_PREFIX)$(APP).dis

$(TMP_PREFIX)$(APP).elf: $(SRCS) $(wildcard %.h)
	mkdir -p $(TMP_PREFIX)
	$(SWTEST_CROSS_PREFIX)gcc $(CCFLAGS) -march=$(SWTEST_MARCH) $(SRCS) -T $(SWTEST_LDSCRIPT) $(addprefix -I,$(INCDIR)) -o $@
