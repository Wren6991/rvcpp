APP          := coremark
MAX_CYCLES   := 100000000

include ../swconfig.mk

###############################################################################

.SUFFIXES:
.PHONY: all run waves view bin tb clean clean_tb

all: run

run: $(APP).bin
	$(SIM_EXEC) --bin $(APP).bin --cycles $(MAX_CYCLES)

bin: $(APP).bin

tb:
	$(MAKE) -C $(SIM_DIR) tb

clean:
	rm -f $(APP).elf $(APP).bin $(APP).dis $(APP)_run.vcd
	rm -rf dist/build/

clean_tb: clean
	$(MAKE) -C $(SIM_DIR) clean

###############################################################################

$(APP).bin: $(APP).elf
	$(SWTEST_CROSS_PREFIX)objcopy -O binary $^ $@
	$(SWTEST_CROSS_PREFIX)objdump -h $(APP).elf > $(APP).dis
	$(SWTEST_CROSS_PREFIX)objdump -d $(APP).elf >> $(APP).dis

$(APP).elf:
	make -C dist
	cp dist/build/coremark.elf $(APP).elf
