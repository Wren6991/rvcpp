#ifndef _MMIO_MTIMER_H
#define _MMIO_MTIMER_H

#include "rv_mem.h"

// Standard RISC-V platform timer (ACLINT timer)

#ifndef MTIMER_N_HARTS
#define MTIMER_N_HARTS 1
#endif

struct MTimer: MemBase32 {
	uint64_t mtime;
	uint64_t mtimecmp[MTIMER_N_HARTS];

	MTimer() {
		mtime = 0;
		for (int i = 0; i < MTIMER_N_HARTS; ++i) {
			mtimecmp[0] = -1ull;
		}
	}

	void step_time() {
		++mtime;
	}

	bool irq_status(uint n) {
		assert(n < MTIMER_N_HARTS);
		return mtime >= mtimecmp[n];
	}

	virtual bool w32(ux_t addr, uint32_t wdata) {
		if (addr == 0) {
			mtime = (mtime & 0xffffffff00000000ull) | (uint64_t)wdata;
		} else if (addr == 4) {
			mtime = (mtime & 0x00000000ffffffffull) | ((uint64_t)wdata << 32);
		} else if (addr < 8 * (MTIMER_N_HARTS + 1)) {
			uint hart = (addr >> 3) - 1;
			if (addr & 0x4) {
				mtimecmp[hart] = (mtimecmp[hart] & 0x00000000ffffffffull) | ((uint64_t)wdata << 32);
			} else {
				mtimecmp[hart] = (mtimecmp[hart] & 0xffffffff00000000ull) | (uint64_t)wdata;
			}
		} else {
			return false;
		}
		return true;
	}

	virtual std::optional<uint32_t> r32(ux_t addr) {
		if (addr == 0) {
			return mtime & 0xffffffffull;
		} else if (addr == 4) {
			return mtime >> 32;
		} else if (addr < 8 * (MTIMER_N_HARTS + 1)) {
			uint hart = (addr >> 3) - 1;
			if (addr & 0x4) {
				return mtimecmp[hart] >> 32;
			} else {
				return mtimecmp[hart] & 0xffffffffull;
			}
		} else {
			return {};
		}
	}

};

#endif
