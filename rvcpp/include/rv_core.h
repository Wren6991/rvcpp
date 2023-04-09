#ifndef _RV_CORE_H
#define _RV_CORE_H

#include <optional>
#include <cassert>

#include "rv_csr.h"
#include "rv_types.h"
#include "rv_mem.h"
#include "encoding/rv_csr.h"

struct RVCore {
	std::array<ux_t, 32> regs;
	ux_t pc;
	RVCSR csr;
	bool load_reserved;
	MemBase32 &mem;

	// A single flat RAM is handled as a special case, in addition to whatever
	// is in `mem`, because this avoids virtual calls for the majority of
	// memory accesses. This RAM takes precedence over whatever is mapped at
	// the same address in `mem`. (Note the size of this RAM may be zero, and
	// RAM can also be added to the `mem` object.)
	ux_t *ram;
	ux_t ram_base;
	ux_t ram_top;

	RVCore(MemBase32 &_mem, ux_t reset_vector, ux_t ram_base_, ux_t ram_size_) : mem(_mem) {
		std::fill(std::begin(regs), std::end(regs), 0);
		pc = reset_vector;
		load_reserved = false;
		ram_base = ram_base_;
		ram_top = ram_base_ + ram_size_;
		ram = new ux_t[ram_size_ / sizeof(ux_t)];
		assert(ram);
		assert(!(ram_base_ & 0x3));
		assert(!(ram_size_ & 0x3));
		assert(ram_base_ + ram_size_ >= ram_base_);
		for (ux_t i = 0; i < ram_size_ / sizeof(ux_t); ++i)
			ram[i] = 0;
	}

	~RVCore() {
		delete ram;
	}

	enum {
		OPC_LOAD     = 0b00'000,
		OPC_MISC_MEM = 0b00'011,
		OPC_OP_IMM   = 0b00'100,
		OPC_AUIPC    = 0b00'101,
		OPC_STORE    = 0b01'000,
		OPC_AMO      = 0b01'011,
		OPC_OP       = 0b01'100,
		OPC_LUI      = 0b01'101,
		OPC_BRANCH   = 0b11'000,
		OPC_JALR     = 0b11'001,
		OPC_JAL      = 0b11'011,
		OPC_SYSTEM   = 0b11'100
	};

	// Fetch and execute one instruction from memory.
	void step(bool trace=false);

	// Functions to read/write memory from this hart's point of view
	std::optional<uint8_t> r8(ux_t addr) {
		if (addr >= ram_base && addr < ram_top) {
			return ram[(addr - ram_base) >> 2] >> 8 * (addr & 0x3) & 0xffu;
		} else {
			return mem.r8(addr);
		}
	}

	bool w8(ux_t addr, uint8_t data) {
		if (addr >= ram_base && addr < ram_top) {
			ram[(addr - ram_base) >> 2] &= ~(0xffu << 8 * (addr & 0x3));
			ram[(addr - ram_base) >> 2] |= (uint32_t)data << 8 * (addr & 0x3);
			return true;
		} else {
			return mem.w8(addr, data);
		}
	}

	std::optional<uint16_t> r16(ux_t addr) {
		if (addr >= ram_base && addr < ram_top) {
			return ram[(addr - ram_base) >> 2] >> 8 * (addr & 0x2) & 0xffffu;
		} else {
			return mem.r16(addr);
		}
	}

	bool w16(ux_t addr, uint16_t data) {
		if (addr >= ram_base && addr < ram_top) {
			ram[(addr - ram_base) >> 2] &= ~(0xffffu << 8 * (addr & 0x2));
			ram[(addr - ram_base) >> 2] |= (uint32_t)data << 8 * (addr & 0x2);
			return true;
		} else {
			return mem.w16(addr, data);
		}
	}

	std::optional<uint32_t> r32(ux_t addr) {
		if (addr >= ram_base && addr < ram_top) {
			return ram[(addr - ram_base) >> 2];
		} else {
			return mem.r32(addr);
		}
	}

	bool w32(ux_t addr, uint32_t data) {
		if (addr >= ram_base && addr < ram_top) {
			ram[(addr - ram_base) >> 2] = data;
			return true;
		} else {
			return mem.w32(addr, data);
		}
	}

private:

	std::optional<ux_t> vmap_sv32(ux_t vaddr, ux_t atp, uint effective_priv, ux_t required_permissions) {
		assert(effective_priv <= PRV_S);
		// First translation stage: vaddr bits 31:22
		ux_t addr_of_pte1 = atp + ((vaddr >> 20) & 0xffcu);
		std::optional<ux_t> pte1 = r32(addr_of_pte1);
		if (!(pte1 && *pte1 & PTE_V))
			return std::nullopt;
		if (*pte1 & (PTE_X | PTE_W | PTE_R)) {
			// It's a leaf PTE. Permission check before touching A/D bits:
			if (!csr.pte_permissions_ok(*pte1, required_permissions))
				return std::nullopt;
			// First-level leaf PTEs must have lower PPN bits cleared, so that
			// they cover a 4 MiB-aligned range.
			if (*pte1 & 0x000ffc00u)
				return std::nullopt;
			// Looks good, so update A/D and return the mapped address
			ux_t pte1_a_d_update = *pte1 | PTE_A | (required_permissions & PTE_W ? PTE_D : 0);
			if (pte1_a_d_update != *pte1) {
				if (!w32(addr_of_pte1, pte1_a_d_update)) {
					return std::nullopt;
				}
			}
			return ((*pte1 << 2) & 0xffc00000u) | (vaddr & 0x003fffffu);
		}

		// Second translation stage: vaddr bits 21:12
		ux_t addr_of_pte0 = ((*pte1 << 2) & 0xfffff000u) | ((vaddr >> 10) & 0xffcu);
		std::optional<ux_t> pte0 = r32(addr_of_pte0);
		// Must be a valid leaf PTE.
		if (!(pte0 && (*pte0 & PTE_V) && (*pte0 & (PTE_X | PTE_W | PTE_R))))
			return std::nullopt;
		// Permission check
		if (!csr.pte_permissions_ok(*pte0, required_permissions))
			return std::nullopt;
		// PTE looks good, so update A/D bits before returning the mapped address
		ux_t pte0_a_d_update = *pte0 | PTE_A | (required_permissions & PTE_W ? PTE_D : 0);
		if (pte0_a_d_update != *pte0) {
			if (!w32(addr_of_pte0, pte0_a_d_update)) {
				return std::nullopt;
			}
		}
		return ((*pte0 << 2) & 0xfffff000u) | (vaddr & 0xfffu);
	}

	std::optional<ux_t> vmap_ls(ux_t vaddr, ux_t required_permissions) {
		if (csr.translation_enabled_ls()) {
			std::optional<ux_t> paddr = vmap_sv32(vaddr, csr.get_atp(), csr.get_effective_priv_ls(), required_permissions);
			return paddr;
		} else {
			return vaddr;
		}
	}

	std::optional<ux_t> vmap_fetch(ux_t vaddr) {
		if (csr.translation_enabled_fetch()) {
			return vmap_sv32(vaddr, csr.get_atp(), csr.get_true_priv(), PTE_X);
		} else {
			return vaddr;
		}
	}

};

#endif
