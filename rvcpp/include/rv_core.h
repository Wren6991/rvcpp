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

	RVCore(MemBase32 &_mem, ux_t reset_vector=0x40) : mem(_mem) {
		std::fill(std::begin(regs), std::end(regs), 0);
		pc = reset_vector;
		load_reserved = false;
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

	void step(MemBase32 &mem, bool trace=false);

private:

	std::optional<ux_t> vmap_sv32(ux_t vaddr, ux_t atp, uint effective_priv, ux_t required_permissions) {
		assert(effective_priv <= PRV_S);
		// First translation stage: vaddr bits 31:22
		ux_t addr_of_pte1 = atp + ((vaddr >> 20) & 0xffcu);
		std::optional<ux_t> pte1 = mem.r32(addr_of_pte1);
		if (!(pte1 && *pte1 & PTE_V))
			return {};
		if (*pte1 & (PTE_X | PTE_W | PTE_R)) {
			// It's a leaf PTE. Permission check before touching A/D bits:
			if (!csr.pte_permissions_ok(*pte1, required_permissions))
				return {};
			// Looks good, so update A/D and return the mapped address
			ux_t pte1_a_d_update = *pte1 | PTE_A | (required_permissions & PTE_W ? PTE_D : 0);
			if (pte1_a_d_update != *pte1) {
				if (!mem.w32(addr_of_pte1, pte1_a_d_update)) {
					return {};
				}
			}
			return ((*pte1 << 2) & 0xffc00000u) | (vaddr & 0x003fffffu);
		}

		// Second translation stage: vaddr bits 21:12
		ux_t addr_of_pte0 = ((*pte1 << 2) & 0xfffff000u) | ((vaddr >> 10) & 0xffcu);
		std::optional<ux_t> pte0 = mem.r32(addr_of_pte0);
		// Must be a valid leaf PTE.
		if (!(pte0 && (*pte0 & PTE_V) && (*pte0 & (PTE_X | PTE_W | PTE_R))))
			return {};
		// Permission check
		if (!csr.pte_permissions_ok(*pte0, required_permissions))
			return {};
		// PTE looks good, so update A/D bits before returning the mapped address
		ux_t pte0_a_d_update = *pte0 | PTE_A | (required_permissions & PTE_W ? PTE_D : 0);
		if (pte0_a_d_update != *pte0) {
			if (!mem.w32(addr_of_pte0, pte0_a_d_update)) {
				return {};
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
