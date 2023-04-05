#include "rv_csr.h"
#include <optional>
#include <cassert>

void RVCSR::step_counters() {
	uint64_t mcycle_next = ((uint64_t)mcycleh << 32) + mcycle + 1u;
	mcycle = mcycle_next & 0xffffffffu;
	mcycleh = mcycle_next >> 32;
	uint64_t minstret_next = ((uint64_t)minstreth << 32) + minstret + 1u;
	minstret = minstret_next & 0xffffffffu;
	minstreth = minstret_next >> 32;
}

static const ux_t SSTATUS_MASK =
	SSTATUS_SIE |
	SSTATUS_SPIE |
	SSTATUS_SPP |
	SSTATUS_SUM |
	SSTATUS_MXR;

static const ux_t MSTATUS_MASK =
	SSTATUS_MASK |
	MSTATUS_MIE |
	MSTATUS_MPIE |
	MSTATUS_MPP |
	MSTATUS_MPRV |
	MSTATUS_TVM |
	MSTATUS_TW |
	MSTATUS_TSR;

static const ux_t SIP_MASK =
	MIP_SSIP |
	MIP_STIP |
	MIP_SEIP;

static const ux_t ALL_MIP_BITS =
	SIP_MASK |
	MIP_MSIP |
	MIP_MTIP |
	MIP_MEIP;

static const ux_t MIP_R_MASK = ALL_MIP_BITS;
static const ux_t MIP_W_MASK = SIP_MASK;
static const ux_t MIE_R_MASK = ALL_MIP_BITS;
static const ux_t MIE_W_MASK = ALL_MIP_BITS;

// Returns None on permission/decode fail
std::optional<ux_t> RVCSR::read(uint16_t addr, __attribute__((unused)) bool side_effect) {
	// Minimum privilege check
	if (addr >= 1u << 12 || GETBITS(addr, 9, 8) > priv)
		return {};
	// Additional privilege checks
	bool permit_cycle =
		(priv >= PRV_M || mcounteren & 0x1) &&
		(priv >= PRV_S || scounteren & 0x1);
	bool permit_instret =
		(priv >= PRV_M || mcounteren & 0x4) &&
		(priv >= PRV_S || scounteren & 0x4);
	bool permit_satp = priv >= PRV_M || !(xstatus & MSTATUS_TVM);

	switch (addr) {
		// Machine ID
		case CSR_MISA:       return 0x40101105; // RV32IMAC + U
		case CSR_MHARTID:    return 0;
		case CSR_MARCHID:    return 0;
		case CSR_MIMPID:     return 0;
		case CSR_MVENDORID:  return 0;

		// Machine trap handling
		case CSR_MSTATUS:    return xstatus & MSTATUS_MASK;
		case CSR_MIE:        return xie & MIE_R_MASK;
		case CSR_MIP:        return get_effective_xip() & MIP_R_MASK;
		case CSR_MTVEC:      return mtvec;
		case CSR_MSCRATCH:   return mscratch;
		case CSR_MEPC:       return mepc;
		case CSR_MCAUSE:     return mcause;
		case CSR_MTVAL:      return mtval;
		case CSR_MEDELEG:    return medeleg;
		case CSR_MIDELEG:    return mideleg;

		// Machine counter
		case CSR_MCOUNTEREN: return mcounteren;
		case CSR_MCYCLE:     return mcycle;
		case CSR_MCYCLEH:    return mcycleh;
		case CSR_MINSTRET:   return minstreth;
		case CSR_MINSTRETH:  return minstreth;

		// Supervisor trap handling
		case CSR_SSTATUS:    return xstatus & SSTATUS_MASK;
		case CSR_SIE:        return xie & SIP_MASK;
		case CSR_SIP:        return get_effective_xip() & SIP_MASK & mideleg;
		case CSR_STVEC:      return stvec;
		case CSR_SCOUNTEREN: return scounteren;
		case CSR_SSCRATCH:   return sscratch;
		case CSR_SEPC:       return sepc;
		case CSR_SCAUSE:     return scause;
		case CSR_STVAL:      return stval;
		case CSR_SATP:       if (permit_satp)        return satp;       else return {};

		// Unprivileged
		case CSR_CYCLE:      if (permit_cycle)       return mcycle;     else return {};
		case CSR_CYCLEH:     if (permit_cycle)       return mcycleh;    else return {};
		case CSR_INSTRET:    if (permit_instret)     return minstreth;  else return {};
		case CSR_INSTRETH:   if (permit_instret)     return minstreth;  else return {};

		default:             return {};
	}
}

// Returns false on permission/decode fail
bool RVCSR::write(uint16_t addr, ux_t data, uint op) {
	// Check minimum privilege + !RO
	if (addr >= 1u << 12 || GETBITS(addr, 9, 8) > priv || GETBITS(addr, 11, 10) == 0x3)
		return false;

	// Apply read-modify-write behaviour
	if (op == WRITE_CLEAR || op == WRITE_SET) {
		std::optional<ux_t> rdata = read(addr, false);
		if (!rdata)
			return false;
		if (op == WRITE_CLEAR)
			data = *rdata & ~data;
		else
			data = *rdata |  data;
	}

	bool permit_satp = priv >= PRV_M || !(xstatus & MSTATUS_TVM);

	ux_t sip_mask_deleg = SIP_MASK & mideleg;

	switch (addr) {
		case CSR_MISA:                                                                       break;
		case CSR_MHARTID:                                                                    break;
		case CSR_MARCHID:                                                                    break;
		case CSR_MIMPID:                                                                     break;
		case CSR_MVENDORID:                                                                  break;

		case CSR_MSTATUS:    xstatus    = (data & MSTATUS_MASK) | (xstatus & ~MSTATUS_MASK); break;
		case CSR_MIE:        xie        = (data & MIE_W_MASK) | (xie & ~MIE_W_MASK);         break;
		case CSR_MIP:        xip        = (data & MIP_W_MASK) | (xip & ~MIP_W_MASK);         break;
		case CSR_MTVEC:      mtvec      = data & 0xfffffffdu;                                break;
		case CSR_MSCRATCH:   mscratch   = data;                                              break;
		case CSR_MEPC:       mepc       = data & 0xfffffffeu;                                break;
		case CSR_MCAUSE:     mcause     = data & 0x800000ffu;                                break;
		case CSR_MTVAL:      mtval      = data;                                              break;
		case CSR_MEDELEG:    medeleg    = data;                                              break;
		case CSR_MIDELEG:    mideleg    = data;                                              break;

		case CSR_MCOUNTEREN: mcounteren = data & 0x7u;                                       break;
		case CSR_MCYCLE:     mcycle     = data;                                              break;
		case CSR_MCYCLEH:    mcycleh    = data;                                              break;
		case CSR_MINSTRET:   minstret   = data;                                              break;
		case CSR_MINSTRETH:  minstreth  = data;                                              break;

		case CSR_SSTATUS:    xstatus    = (data & SSTATUS_MASK) | (xstatus & ~SSTATUS_MASK); break;
		case CSR_SIE:        xie        = (data & SIP_MASK) | (xie & ~SIP_MASK);             break;
		case CSR_SIP:        xip        = (data & sip_mask_deleg) | (xip & ~sip_mask_deleg); break;
		case CSR_STVEC:      stvec      = data & 0xfffffffdu;                                break;
		case CSR_SCOUNTEREN: scounteren = data & 0x7u;                                       break;
		case CSR_SSCRATCH:   sscratch   = data;                                              break;
		case CSR_SEPC:       sepc       = data & 0xfffffffeu;                                break;
		case CSR_SCAUSE:     scause     = data & 0x800000ffu;                                break;
		case CSR_STVAL:      stval      = data;                                              break;
		case CSR_SATP:       if (permit_satp) satp = data & ~SATP32_ASID; else return false; break;

		default:             return false;
	}
	return true;
}

ux_t RVCSR::trap_enter_exception(uint xcause, ux_t xepc) {
	assert(xcause < 32);
	uint target_priv = medeleg & (1u << xcause) ? PRV_S : PRV_M;
	if (target_priv < priv) {
		target_priv = priv;
	}
	return trap_enter_at_priv(xcause, xepc, target_priv);
}

std::optional<ux_t> RVCSR::trap_check_enter_irq(ux_t xepc) {
	ux_t m_targeted_irqs = get_effective_xip() & xie & MIP_R_MASK & ~mideleg;
	ux_t s_targeted_irqs = get_effective_xip() & xie & SIP_MASK   &  mideleg;
	bool take_m_irq = m_targeted_irqs && ((xstatus & MSTATUS_MIE) || priv < PRV_M);
	bool take_s_irq = s_targeted_irqs && ((xstatus & SSTATUS_SIE) || priv < PRV_S) && priv <= PRV_S ;
	if (take_m_irq) {
		ux_t cause = (1u << 31) | __builtin_ctz(m_targeted_irqs);
		return trap_enter_at_priv(cause, xepc, PRV_M);
	} else if (take_s_irq) {
		ux_t cause = (1u << 31) | __builtin_ctz(s_targeted_irqs);
		return trap_enter_at_priv(cause, xepc, PRV_S);
	} else {
		return {};
	}
}

ux_t RVCSR::trap_enter_at_priv(uint xcause, ux_t xepc, uint target_priv) {
	if (target_priv == PRV_M) {
		// Trap to M-mode
		xstatus = (xstatus & ~MSTATUS_MPP) | (priv << 11);
		priv = PRV_M;

		if (xstatus & MSTATUS_MIE)
			xstatus |= MSTATUS_MPIE;
		xstatus &= ~MSTATUS_MIE;

		mcause = xcause;
		mepc = xepc;
		if ((mtvec & 0x1) && (xcause & (1u << 31))) {
			return (mtvec & -2) + 4 * (xcause & ~(1u << 31));
		} else {
			return mtvec & -2;
		}
	} else {
		// Trap to S-mode
		assert(target_priv == PRV_S);
		xstatus = (xstatus & ~SSTATUS_SPP) | (priv << 8);
		priv = PRV_S;

		if (xstatus & SSTATUS_SIE)
			xstatus |= SSTATUS_SPIE;
		xstatus &= ~SSTATUS_SIE;

		scause = xcause;
		sepc = xepc;

		if ((stvec & 0x1) && (xcause & (1u << 31))) {
			return (stvec & -2) + 4 * (xcause & ~(1u << 31));
		} else {
			return stvec & -2;
		}
	}
}

ux_t RVCSR::trap_mret() {
	priv = GETBITS(xstatus, 12, 11);
	xstatus &= ~MSTATUS_MPP;
	if (priv != PRV_M)
		xstatus &= ~MSTATUS_MPRV;

	if (xstatus & MSTATUS_MPIE)
		xstatus |= MSTATUS_MIE;
	xstatus &= ~MSTATUS_MPIE;

	return mepc;
}

ux_t RVCSR::trap_sret(ux_t pc) {
	if (xstatus & MSTATUS_TSR && priv == PRV_S) {
		// Note M-mode may have delegated this exception, which is perhaps
		// unwise, but we have to respect its decisions
		return trap_enter_exception(XCAUSE_INSTR_ILLEGAL, pc);
	} else {
		priv = GETBIT(xstatus, 8);
		xstatus &= ~SSTATUS_SPP;
		if (xstatus & SSTATUS_SPIE)
			xstatus |= SSTATUS_SIE;
		xstatus &= ~SSTATUS_SPIE;
		// Note target of sret is never M, so MPRV always cleared.
		xstatus &= ~MSTATUS_MPRV;
		return sepc;
	}
}

void RVCSR::trap_set_xtval(ux_t xtval) {
	assert(priv >= PRV_S);
	if (priv == PRV_S) {
		stval = xtval;
	} else {
		mtval = xtval;
	}
}
