#ifndef _RV_CSR_H
#define _RV_CSR_H

#include <optional>
#include <cassert>

#include "encoding/rv_csr.h"
#include "rv_types.h"

class RVCSR {
	// Current core privilege level (M/S/U)
	uint priv;

	// Latched IRQ signals into core
	bool irq_t;
	bool irq_s;
	bool irq_e;

	// Machine trap handling
	ux_t xstatus;
	ux_t xie;
	ux_t xip;
	ux_t mtvec;
	ux_t mtval;
	ux_t mscratch;
	ux_t mepc;
	ux_t mcause;
	ux_t medeleg;
	ux_t mideleg;

	// Machine counter
	ux_t mcounteren;
	ux_t mcycle;
	ux_t mcycleh;
	ux_t minstret;
	ux_t minstreth;

	// Supervisor trap handling
	// (Note mstatus/sstatus are views of xstatus)
	ux_t stvec;
	ux_t stval;
	ux_t scounteren;
	ux_t sscratch;
	ux_t sepc;
	ux_t scause;
	ux_t satp;

	// xip's read value is a combination of local read/write bits and external
	// interrupt signals
	ux_t get_effective_xip() {
		return xip |
			(irq_s ? MIP_MSIP | MIP_SSIP : 0) |
			(irq_t ? MIP_MTIP | MIP_STIP : 0) |
			(irq_e ? MIP_MEIP | MIP_SEIP : 0);
	}

	// Internal interface for updating trap state once a trap's target
	// privilege has been calculated. Returns trap target pc.
	ux_t trap_enter_at_priv(uint xcause, ux_t xepc, uint target_priv);

public:

	enum {
		WRITE = 0,
		WRITE_SET = 1,
		WRITE_CLEAR = 2
	};

	RVCSR() {
		priv       = 3;

		xstatus    = 0;
		xie        = 0;
		xip        = 0;
		mtvec      = 0;
		mtval      = 0;
		mscratch   = 0;
		mepc       = 0;
		mcause     = 0;
		medeleg    = 0;
		mideleg    = 0;

		mcounteren = 0;
		mcycle     = 0;
		mcycleh    = 0;
		minstret   = 0;
		minstreth  = 0;

		stvec      = 0;
		stval      = 0;
		scounteren = 0;
		sscratch   = 0;
		sepc       = 0;
		scause     = 0;
		satp       = 0;
	}

	void step_counters();

	// Returns None on permission/decode fail
	std::optional<ux_t> read(uint16_t addr, bool side_effect=true);

	// Returns false on permission/decode fail
	bool write(uint16_t addr, ux_t data, uint op=WRITE);

	// Determine target privilege level of an exception, update trap state
	// (including change of privilege level), return trap target PC
	ux_t trap_enter_exception(uint xcause, ux_t xepc);

	// If there is currently a pending IRQ that must be entered, then
	// determine its target privilege level, update trap state, and return
	// trap target PC. Otherwise return None.
	std::optional<ux_t> trap_check_enter_irq(ux_t xepc);

	// Update trap state, return mepc:
	ux_t trap_mret();

	// Update trap state, return mepc (unless SRET is trapped via e.g.
	// mstatus.tsr, in which case take trap)
	ux_t trap_sret(ux_t pc);

	// Set *tval syndrome register for the current privilege mode
	void trap_set_xtval(ux_t xtval);

	// True privilege is also the effective privilege for instruction fetch
	// (fetch translation/protection is not affected by MPRV)
	uint get_true_priv() {
		return priv;
	}

	uint get_effective_priv_ls() {
		if (xstatus & MSTATUS_MPRV) {
			assert(priv == PRV_M);
			return GETBITS(xstatus, 12, 11); // MPP
		} else {
			return priv;
		}
	}

	bool translation_enabled_fetch() {
		return get_true_priv() != PRV_M && (satp & SATP32_MODE);
	}

	bool translation_enabled_ls() {
		return get_effective_priv_ls() != PRV_M && (satp & SATP32_MODE);
	}

	ux_t get_atp() {
		return (satp & SATP32_PPN) << 12;
	}

	bool permit_sfence_vma() {
		return (priv == PRV_S && !(xstatus & MSTATUS_TVM)) || priv == PRV_M;
	}

	bool pte_permissions_ok(ux_t pte, ux_t required_permissions) {
		// If it requires X permission, we can assume it's an instruction fetch
		uint effective_priv = required_permissions & PTE_X ? get_true_priv() : get_effective_priv_ls();
		assert(effective_priv <= PRV_S);

		// Bad S access to U:
		if (pte & PTE_U && effective_priv == PRV_S && !(xstatus & SSTATUS_SUM))
			return false;
		// Any U access to S:
		if (!(pte & PTE_U) && effective_priv == PRV_U)
			return false;
		// Permission fail:
		ux_t permissions = pte & (PTE_R | PTE_W | PTE_X);
		if ((xstatus & SSTATUS_MXR) && (permissions & PTE_X))
			permissions |= PTE_R;
		if (~permissions & required_permissions)
			return false;

		// Nothing to complain about
		return true;
	}

	void set_irq_t(bool irq) {
		irq_t = irq;
	}

	void set_irq_s(bool irq) {
		irq_s = irq;
	}

	void set_irq_e(bool irq) {
		irq_e = irq;
	}

	ux_t get_xcause() {
		return priv == PRV_M ? mcause : scause;
	}
};

#endif
