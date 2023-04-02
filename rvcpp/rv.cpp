#include <cstdint>
#include <cassert>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <optional>
#include <tuple>
#include <vector>

#include "rv_opcodes.h"
#include "rv_types.h"
#include "rv_csr.h"
#include "mem.h"

// Minimal RISC-V interpreter, supporting:
// - RV32I
// - M
// - A
// - C (also called Zca)
// - Zcmp
// - Zicsr
// - Zicntr
// - M-mode traps

// Use unsigned arithmetic everywhere, with explicit sign extension as required.
static inline ux_t sext(ux_t bits, int sign_bit) {
	if (sign_bit >= XLEN - 1)
		return bits;
	else
		return (bits & (1u << sign_bit + 1) - 1) - ((bits & 1u << sign_bit) << 1);
}

// Inclusive msb:lsb style, like Verilog (and like the ISA manual)
#define BITS_UPTO(msb) (~((-1u << (msb)) << 1))
#define BITRANGE(msb, lsb) (BITS_UPTO((msb) - (lsb)) << (lsb))
#define GETBITS(x, msb, lsb) (((x) & BITRANGE(msb, lsb)) >> (lsb))
#define GETBIT(x, bit) (((x) >> (bit)) & 1u)

static inline ux_t imm_i(uint32_t instr) {
	return (instr >> 20) - (instr >> 19 & 0x1000);
}

static inline ux_t imm_s(uint32_t instr) {
	return (instr >> 20 & 0xfe0u)
		+ (instr >> 7 & 0x1fu)
		- (instr >> 19 & 0x1000u);
}

static inline ux_t imm_u(uint32_t instr) {
	return instr & 0xfffff000u;
}

static inline ux_t imm_b(uint32_t instr) {
	return (instr >> 7 & 0x1e)
		+ (instr >> 20 & 0x7e0)
		+ (instr << 4 & 0x800)
		- (instr >> 19 & 0x1000);
}

static inline ux_t imm_j(uint32_t instr) {
	 return (instr >> 20 & 0x7fe)
	 	+ (instr >> 9 & 0x800)
	 	+ (instr & 0xff000)
	 	- (instr >> 11 & 0x100000);
}

static inline ux_t imm_ci(uint32_t instr) {
	return GETBITS(instr, 6, 2) - (GETBIT(instr, 12) << 5);
}

static inline ux_t imm_cj(uint32_t instr) {
	return -(GETBIT(instr, 12) << 11)
		+ (GETBIT(instr, 11) << 4)
		+ (GETBITS(instr, 10, 9) << 8)
		+ (GETBIT(instr, 8) << 10)
		+ (GETBIT(instr, 7) << 6)
		+ (GETBIT(instr, 6) << 7)
		+ (GETBITS(instr, 5, 3) << 1)
		+ (GETBIT(instr, 2) << 5);
}

static inline ux_t imm_cb(uint32_t instr) {
	return -(GETBIT(instr, 12) << 8)
		+ (GETBITS(instr, 11, 10) << 3)
		+ (GETBITS(instr, 6, 5) << 6)
		+ (GETBITS(instr, 4, 3) << 1)
		+ (GETBIT(instr, 2) << 5);
}

static inline uint c_rs1_s(uint32_t instr) {
	return GETBITS(instr, 9, 7) + 8;
}

static inline uint c_rs2_s(uint32_t instr) {
	return GETBITS(instr, 4, 2) + 8;
}

static inline uint c_rs1_l(uint32_t instr) {
	return GETBITS(instr, 11, 7);
}

static inline uint c_rs2_l(uint32_t instr) {
	return GETBITS(instr, 6, 2);
}

static inline uint zcmp_n_regs(uint32_t instr) {
	uint rlist = GETBITS(instr, 7, 4);
	return rlist == 0xf ? 13 : rlist - 3;
}

static inline uint zcmp_stack_adj(uint32_t instr) {
	uint nregs = zcmp_n_regs(instr);
	uint adj_base =
		nregs > 12 ? 0x40 :
		nregs >  8 ? 0x30 :
		nregs >  4 ? 0x20 : 0x10;
	return adj_base + 16 * GETBITS(instr, 3, 2);

}

static inline uint32_t zcmp_reg_mask(uint32_t instr) {
	uint32_t mask = 0;
	switch (zcmp_n_regs(instr)) {
		case 13: mask |= 1u << 27; // s11
		         mask |= 1u << 26; // s10
		case 11: mask |= 1u << 25; // s9
		case 10: mask |= 1u << 24; // s8
		case  9: mask |= 1u << 23; // s7
		case  8: mask |= 1u << 22; // s6
		case  7: mask |= 1u << 21; // s5
		case  6: mask |= 1u << 20; // s4
		case  5: mask |= 1u << 19; // s3
		case  4: mask |= 1u << 18; // s2
		case  3: mask |= 1u <<  9; // s1
		case  2: mask |= 1u <<  8; // s0
		case  1: mask |= 1u <<  1; // ra
	}
	return mask;
}

static inline uint zcmp_s_mapping(uint s_raw) {
	return s_raw + 8 + 8 * ((s_raw & 0x6) != 0);
}

class RVCSR {
	// Current core privilege level (M/S/U)
	uint priv;

	// Machine trap handling
	ux_t xstatus;
	ux_t mie;
	ux_t mip;
	ux_t mtvec;
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
	ux_t sie;
	ux_t sip;
	ux_t stvec;
	ux_t scounteren;
	ux_t sscratch;
	ux_t sepc;
	ux_t scause;
	ux_t satp;

	const ux_t SSTATUS_MASK = 
		SSTATUS_SIE |
		SSTATUS_SPIE |
		SSTATUS_SPP |
		SSTATUS_SUM |
		SSTATUS_MXR;

	const ux_t MSTATUS_MASK =
		SSTATUS_MASK |
		MSTATUS_MIE |
		MSTATUS_MPIE |
		MSTATUS_MPP |
		MSTATUS_TVM |
		MSTATUS_TW |
		MSTATUS_TSR;

public:

	enum {
		WRITE = 0,
		WRITE_SET = 1,
		WRITE_CLEAR = 2
	};

	RVCSR() {
		priv       = 3;

		xstatus    = 0;
		mie        = 0;
		mip        = 0;
		mtvec      = 0;
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

		sie        = 0;
		sip        = 0;
		stvec      = 0;
		scounteren = 0;
		sscratch   = 0;
		sepc       = 0;
		scause     = 0;
		satp       = 0;
	}

	void step() {
		uint64_t mcycle_next = (uint64_t)mcycleh << 32 + mcycle + 1u;
		mcycle = mcycle_next & 0xffffffffu;
		mcycleh = mcycle_next >> 32;
		uint64_t minstret_next = (uint64_t)minstreth << 32 + minstret + 1u;
		minstret = minstret_next & 0xffffffffu;
		minstreth = minstret_next >> 32;
	}

	// Returns None on permission/decode fail
	std::optional<ux_t> read(uint16_t addr, bool side_effect=true) {
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
			case CSR_MIE:        return mie;
			case CSR_MIP:        return mip;
			case CSR_MTVEC:      return mtvec;
			case CSR_MSCRATCH:   return mscratch;
			case CSR_MEPC:       return mepc;
			case CSR_MCAUSE:     return mcause;
			case CSR_MTVAL:      return 0;
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
			case CSR_SIE:        return sie;
			case CSR_SIP:        return sip;
			case CSR_STVEC:      return stvec;
			case CSR_SCOUNTEREN: return scounteren;
			case CSR_SSCRATCH:   return sscratch;
			case CSR_SEPC:       return sepc;
			case CSR_SCAUSE:     return scause;
			case CSR_STVAL:      return 0;
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
	bool write(uint16_t addr, ux_t data, uint op=WRITE) {
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

		switch (addr) {
			case CSR_MISA:                                                                       break;
			case CSR_MHARTID:                                                                    break;
			case CSR_MARCHID:                                                                    break;
			case CSR_MIMPID:                                                                     break;
			case CSR_MVENDORID:                                                                  break;

			case CSR_MSTATUS:    xstatus    = data & MSTATUS_MASK | xstatus & ~MSTATUS_MASK;     break;
			case CSR_MIE:        mie        = data;                                              break;
			case CSR_MIP:                                                                        break;
			case CSR_MTVEC:      mtvec      = data & 0xfffffffdu;                                break;
			case CSR_MSCRATCH:   mscratch   = data;                                              break;
			case CSR_MEPC:       mepc       = data & 0xfffffffeu;                                break;
			case CSR_MCAUSE:     mcause     = data & 0x800000ffu;                                break;
			case CSR_MTVAL:                                                                      break;
			case CSR_MEDELEG:    medeleg    = data;                                              break;
			case CSR_MIDELEG:    mideleg    = data;                                              break;

			case CSR_MCOUNTEREN: mcounteren = data & 0x7u;                                       break;
			case CSR_MCYCLE:     mcycle     = data;                                              break;
			case CSR_MCYCLEH:    mcycleh    = data;                                              break;
			case CSR_MINSTRET:   minstret   = data;                                              break;
			case CSR_MINSTRETH:  minstreth  = data;                                              break;

			case CSR_SSTATUS:    xstatus    = data & SSTATUS_MASK | xstatus & ~SSTATUS_MASK;     break;
			case CSR_SIE:        sie        = data;                                              break;
			case CSR_SIP:                                                                        break;
			case CSR_STVEC:      stvec      = data & 0xfffffffdu;                                break;
			case CSR_SCOUNTEREN: scounteren = data & 0x7u;                                       break;
			case CSR_SSCRATCH:   sscratch   = data;                                              break;
			case CSR_SEPC:       sepc       = data & 0xfffffffeu;                                break;
			case CSR_SCAUSE:     scause     = data & 0x800000ffu;                                break;
			case CSR_STVAL:                                                                      break;
			case CSR_SATP:       if (permit_satp) satp = 0; else return false;                   break;

			default:             return false;
		}
		return true;
	}

	// Update trap state (including change of privilege level), return trap target PC
	ux_t trap_enter(uint xcause, ux_t xepc) {
		assert(xcause < 32);

		uint target_priv = medeleg & (1u << xcause) ? PRV_S : PRV_M;
		if (target_priv < priv) {
			target_priv = priv;
		}

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

	// Update trap state, return mepc:
	ux_t trap_mret() {
		priv = GETBITS(xstatus, 12, 11);

		if (xstatus & MSTATUS_MPIE)
			xstatus |= MSTATUS_MIE;
		xstatus &= ~MSTATUS_MPIE;

		return mepc;
	}

	ux_t trap_sret(ux_t pc) {
		if (xstatus & MSTATUS_TSR) {
			return trap_enter(XCAUSE_INSTR_ILLEGAL, pc);
		} else {
			priv = GETBIT(xstatus, 8);
			if (xstatus & SSTATUS_SPIE)
				xstatus |= SSTATUS_SIE;
			xstatus &= ~SSTATUS_SPIE;
			
			return sepc;
		}
	}

	uint getpriv() {
		return priv;
	}

	uint getxstatus() {
		return xstatus;
	}
};

struct RVCore {
	std::array<ux_t, 32> regs;
	ux_t pc;
	RVCSR csr;
	bool load_reserved;

	RVCore(ux_t reset_vector=0x40) {
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

	void step(MemBase32 &mem, bool trace=false) {
		uint32_t instr = mem.r16(pc) | (mem.r16(pc + 2) << 16);
		std::optional<ux_t> rd_wdata;
		std::optional<ux_t> pc_wdata;
		uint regnum_rd = 0;
		std::optional<uint> exception_cause;
		std::optional<ux_t> trace_csr_addr;
		std::optional<ux_t> trace_csr_result;

		if ((instr & 0x3) == 0x3) {
			// 32-bit instruction
			uint opc        = instr >> 2 & 0x1f;
			uint funct3     = instr >> 12 & 0x7;
			uint funct7     = instr >> 25 & 0x7f;
			uint regnum_rs1 = instr >> 15 & 0x1f;
			uint regnum_rs2 = instr >> 20 & 0x1f;
			regnum_rd       = instr >> 7 & 0x1f;
			ux_t rs1 = regs[regnum_rs1];
			ux_t rs2 = regs[regnum_rs2];
			switch (opc) {

			case OPC_OP: {
				if (funct7 == 0b00'00000) {
					if (funct3 == 0b000)
						rd_wdata = rs1 + rs2;
					else if (funct3 == 0b001)
						rd_wdata = rs1 << (rs2 & 0x1f);
					else if (funct3 == 0b010)
						rd_wdata = (sx_t)rs1 < (sx_t)rs2;
					else if (funct3 == 0b011)
						rd_wdata = rs1 < rs2;
					else if (funct3 == 0b100)
						rd_wdata = rs1 ^ rs2;
					else if (funct3 == 0b101)
						rd_wdata = rs1  >> (rs2 & 0x1f);
					else if (funct3 == 0b110)
						rd_wdata = rs1 | rs2;
					else if (funct3 == 0b111)
						rd_wdata = rs1 & rs2;
					else
						exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				else if (funct7 == 0b01'00000) {
					if (funct3 == 0b000)
						rd_wdata = rs1 - rs2;
					else if (funct3 == 0b101)
						rd_wdata = (sx_t)rs1 >> (rs2 & 0x1f);
					else
						exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				else if (funct7 == 0b00'00001) {
					if (funct3 < 0b100) {
						sdx_t mul_op_a = rs1;
						sdx_t mul_op_b = rs2;
						if (funct3 != 0b011)
							mul_op_a -= (mul_op_a & (1 << XLEN - 1)) << 1;
						if (funct3 < 0b010)
							mul_op_b -= (mul_op_b & (1 << XLEN - 1)) << 1;
						sdx_t mul_result = mul_op_a * mul_op_b;
						if (funct3 == 0b000)
							rd_wdata = mul_result;
						else
							rd_wdata = mul_result >> XLEN;
					}
					else {
						if (funct3 == 0b100) {
							if (rs2 == 0)
								rd_wdata = -1;
							else if (rs2 == ~0u)
								rd_wdata = -rs1;
							else
								rd_wdata = (sx_t)rs1 / (sx_t)rs2;
						}
						else if (funct3 == 0b101) {
							rd_wdata = rs2 ? rs1 / rs2 : ~0ul;
						}
						else if (funct3 == 0b110) {
							if (rs2 == 0)
								rd_wdata = rs1;
							else if (rs2 == ~0u) // potential overflow of division
								rd_wdata = 0;
							else
								rd_wdata = (sx_t)rs1 % (sx_t)rs2;
						}
						else if (funct3 == 0b111) {
							rd_wdata = rs2 ? rs1 % rs2 : rs1;
						}
					}
				}
				else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_OP_IMM: {
				ux_t imm = imm_i(instr);
				if (funct3 == 0b000)
					rd_wdata = rs1 + imm;
				else if (funct3 == 0b010)
					rd_wdata = !!((sx_t)rs1 < (sx_t)imm);
				else if (funct3 == 0b011)
					rd_wdata = !!(rs1 < imm);
				else if (funct3 == 0b100)
					rd_wdata = rs1 ^ imm;
				else if (funct3 == 0b110)
					rd_wdata = rs1 | imm;
				else if (funct3 == 0b111)
					rd_wdata = rs1 & imm;
				else if (funct3 == 0b001 || funct3 == 0b101) {
					// shamt is regnum_rs2
					if (funct7 == 0b00'00000 && funct3 == 0b001) {
						rd_wdata = rs1 << regnum_rs2;
					}
					else if (funct7 == 0b00'00000 && funct3 == 0b101) {
						rd_wdata = rs1 >> regnum_rs2;
					}
					else if (funct7 == 0b01'00000 && funct3 == 0b101) {
						rd_wdata = (sx_t)rs1 >> regnum_rs2;
					}
					else {
						exception_cause = XCAUSE_INSTR_ILLEGAL;
					}
				}
				else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_BRANCH: {
				ux_t target = pc + imm_b(instr);
				bool taken = false;
				if ((funct3 & 0b110) == 0b000)
					taken = rs1 == rs2;
				else if ((funct3 & 0b110) == 0b100)
					taken = (sx_t)rs1 < (sx_t) rs2;
				else if ((funct3 & 0b110) == 0b110)
					taken = rs1 < rs2;
				else
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				if (!exception_cause && funct3 & 0b001)
					taken = !taken;
				if (taken)
					pc_wdata = target;
				break;
			}

			case OPC_LOAD: {
				ux_t load_addr = rs1 + imm_i(instr);
				if (funct3 == 0b000) {
					rd_wdata = sext(mem.r8(load_addr), 7);
				} else if (funct3 == 0b001) {
					if (load_addr & 0x1)
						exception_cause = XCAUSE_LOAD_ALIGN;
					else
						rd_wdata = sext(mem.r16(load_addr), 15);
				} else if (funct3 == 0b010) {
					if (load_addr & 0x3)
						exception_cause = XCAUSE_LOAD_ALIGN;
					else
						rd_wdata = mem.r32(load_addr);
				} else if (funct3 == 0b100) {
					rd_wdata = mem.r8(load_addr);
				} else if (funct3 == 0b101) {
					if (load_addr & 0x1)
						exception_cause = XCAUSE_LOAD_ALIGN;
					else
						rd_wdata = mem.r16(load_addr);
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_STORE: {
				ux_t store_addr = rs1 + imm_s(instr);
				if (funct3 == 0b000) {
					mem.w8(store_addr, rs2 & 0xffu);
				} else if (funct3 == 0b001) {
					if (store_addr & 0x1)
						exception_cause = XCAUSE_STORE_ALIGN;
					else
						mem.w16(store_addr, rs2 & 0xffffu);
				} else if (funct3 == 0b010) {
					if (store_addr & 0x3)
						exception_cause = XCAUSE_STORE_ALIGN;
					else
						mem.w32(store_addr, rs2);
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_AMO: {
				if (RVOPC_MATCH(instr, LR_W)) {
					if (rs1 & 0x3) {
						exception_cause = XCAUSE_LOAD_ALIGN;
					} else {
						rd_wdata = mem.r32(rs1);
						load_reserved = true;
					}
				} else if (RVOPC_MATCH(instr, SC_W)) {
					if (rs1 & 0x3) {
						exception_cause = XCAUSE_STORE_ALIGN;
					} else {
						if (load_reserved) {
							load_reserved = false;
							mem.w32(rs1, rs2);
							rd_wdata = 0;
						} else {
							rd_wdata = 1;
						}
					}
				} else if (RVOPC_MATCH(instr, AMOSWAP_W)) {
					if (rs1 & 0x3) {
						exception_cause = XCAUSE_STORE_ALIGN;
					} else {
						rd_wdata = mem.r32(rs1);
						mem.w32(rs1, rs2);
					}
				} else if (
						RVOPC_MATCH(instr, AMOSWAP_W) ||
						RVOPC_MATCH(instr, AMOADD_W) ||
						RVOPC_MATCH(instr, AMOXOR_W) ||
						RVOPC_MATCH(instr, AMOAND_W) ||
						RVOPC_MATCH(instr, AMOOR_W) ||
						RVOPC_MATCH(instr, AMOMIN_W) ||
						RVOPC_MATCH(instr, AMOMAX_W) ||
						RVOPC_MATCH(instr, AMOMINU_W) ||
						RVOPC_MATCH(instr, AMOMAXU_W)) {
					if (rs1 & 0x3) {
						exception_cause = XCAUSE_STORE_ALIGN;
					} else {
						rd_wdata = mem.r32(rs1);
						switch (instr & RVOPC_AMOSWAP_W_MASK) {
							case RVOPC_AMOSWAP_W_BITS: mem.w32(rs1, *rd_wdata);                                      break;
							case RVOPC_AMOADD_W_BITS:  mem.w32(rs1, *rd_wdata + rs2);                                break;
							case RVOPC_AMOXOR_W_BITS:  mem.w32(rs1, *rd_wdata ^ rs2);                                break;
							case RVOPC_AMOAND_W_BITS:  mem.w32(rs1, *rd_wdata & rs2);                                break;
							case RVOPC_AMOOR_W_BITS:   mem.w32(rs1, *rd_wdata | rs2);                                break;
							case RVOPC_AMOMIN_W_BITS:  mem.w32(rs1, (sx_t)*rd_wdata < (sx_t)rs2 ? *rd_wdata : rs2);  break;
							case RVOPC_AMOMAX_W_BITS:  mem.w32(rs1, (sx_t)*rd_wdata > (sx_t)rs2 ? *rd_wdata : rs2);  break;
							case RVOPC_AMOMINU_W_BITS: mem.w32(rs1, *rd_wdata < rs2 ? *rd_wdata : rs2);              break;
							case RVOPC_AMOMAXU_W_BITS: mem.w32(rs1, *rd_wdata > rs2 ? *rd_wdata : rs2);              break;
							default:                   assert(false);                                                break;
						}
					}
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_MISC_MEM: {
				if (RVOPC_MATCH(instr, FENCE)) {
					// implement as nop
				} else if (RVOPC_MATCH(instr, FENCE_I)) {
					// implement as nop
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			case OPC_JAL:
				rd_wdata = pc + 4;
				pc_wdata = pc + imm_j(instr);
				break;

			case OPC_JALR:
				rd_wdata = pc + 4;
				pc_wdata = (rs1 + imm_i(instr)) & -2u;
				break;

			case OPC_LUI:
				rd_wdata = imm_u(instr);
				break;

			case OPC_AUIPC:
				rd_wdata = pc + imm_u(instr);
				break;

			case OPC_SYSTEM: {
				if (RVOPC_MATCH(instr, CSRRW) || RVOPC_MATCH(instr, CSRRS) || RVOPC_MATCH(instr, CSRRC) ||
						RVOPC_MATCH(instr, CSRRWI) || RVOPC_MATCH(instr, CSRRSI) || RVOPC_MATCH(instr, CSRRCI)) {
					uint16_t csr_addr = instr >> 20;
					uint write_op = (funct3 - 1) & 0x3;
					ux_t wdata = funct3 & 0x4 ? regnum_rs1 : rs1;

					if (write_op != RVCSR::WRITE || regnum_rd != 0) {
						rd_wdata = csr.read(csr_addr);
						if (!rd_wdata) {
							exception_cause = XCAUSE_INSTR_ILLEGAL;
						}
					}
					if (write_op == RVCSR::WRITE || regnum_rs1 != 0) {
						if (!csr.write(csr_addr, wdata, write_op)) {
							exception_cause = XCAUSE_INSTR_ILLEGAL;
						}
						if (trace && !exception_cause) {
							trace_csr_addr = csr_addr;
							trace_csr_result = csr.read(csr_addr, false);
						}
					}
					// Suppress GPR writeback of earlier read due to write exception
					if (exception_cause) {
						rd_wdata = {};
					}
				} else if (RVOPC_MATCH(instr, MRET)) {
					if (csr.getpriv() == PRV_M) {
						pc_wdata = csr.trap_mret();
					} else {
						exception_cause = XCAUSE_INSTR_ILLEGAL;
					}
				} else if (RVOPC_MATCH(instr, SRET)) {
					if (csr.getpriv() >= PRV_S) {
						pc_wdata = csr.trap_sret(pc);
					} else {
						exception_cause = XCAUSE_INSTR_ILLEGAL;
					}
				} else if (RVOPC_MATCH(instr, SFENCE_VMA)) {
					if (csr.getxstatus() & MSTATUS_TVM) {
						exception_cause = XCAUSE_INSTR_ILLEGAL;
					}
					// Otherwise nop.
				} else if (RVOPC_MATCH(instr, ECALL)) {
					exception_cause = XCAUSE_ECALL_U + csr.getpriv();
				} else if (RVOPC_MATCH(instr, EBREAK)) {
					exception_cause = XCAUSE_EBREAK;
				} else if (RVOPC_MATCH(instr, WFI)) {
					// implement as nop
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				break;
			}

			default:
				exception_cause = XCAUSE_INSTR_ILLEGAL;
				break;
			}
		} else if ((instr & 0x3) == 0x0) {
			// RVC Quadrant 00:
			if (RVOPC_MATCH(instr, ILLEGAL16)) {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			} else if (RVOPC_MATCH(instr, C_ADDI4SPN)) {
				regnum_rd = c_rs2_s(instr);
				rd_wdata = regs[2]
					+ (GETBITS(instr, 12, 11) << 4)
					+ (GETBITS(instr, 10, 7) << 6)
					+ (GETBIT(instr, 6) << 2)
					+ (GETBIT(instr, 5) << 3);
			} else if (RVOPC_MATCH(instr, C_LW)) {
				regnum_rd = c_rs2_s(instr);
				uint32_t addr = regs[c_rs1_s(instr)]
					+ (GETBIT(instr, 6) << 2)
					+ (GETBITS(instr, 12, 10) << 3)
					+ (GETBIT(instr, 5) << 6);
				rd_wdata = mem.r32(addr);
			} else if (RVOPC_MATCH(instr, C_SW)) {
				uint32_t addr = regs[c_rs1_s(instr)]
					+ (GETBIT(instr, 6) << 2)
					+ (GETBITS(instr, 12, 10) << 3)
					+ (GETBIT(instr, 5) << 6);
				mem.w32(addr, regs[c_rs2_s(instr)]);
			} else {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			}
		} else if ((instr & 0x3) == 0x1) {
			// RVC Quadrant 01:
			if (RVOPC_MATCH(instr, C_ADDI)) {
				regnum_rd = c_rs1_l(instr);
				rd_wdata = regs[c_rs1_l(instr)] + imm_ci(instr);
			} else if (RVOPC_MATCH(instr, C_JAL)) {
				pc_wdata = pc + imm_cj(instr);
				regnum_rd = 1;
				rd_wdata = pc + 2;
			} else if (RVOPC_MATCH(instr, C_LI)) {
				regnum_rd = c_rs1_l(instr);
				rd_wdata = imm_ci(instr);
			} else if (RVOPC_MATCH(instr, C_LUI)) {
				regnum_rd = c_rs1_l(instr);
				// ADDI16SPN if rd is sp
				if (regnum_rd == 2) {
					rd_wdata = regs[2]
						- (GETBIT(instr, 12) << 9)
						+ (GETBIT(instr, 6) << 4)
						+ (GETBIT(instr, 5) << 6)
						+ (GETBITS(instr, 4, 3) << 7)
						+ (GETBIT(instr, 2) << 5);
				} else {
					rd_wdata = -(GETBIT(instr, 12) << 17)
					+ (GETBITS(instr, 6, 2) << 12);
				}
			} else if (RVOPC_MATCH(instr, C_SRLI)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[regnum_rd] >> GETBITS(instr, 6, 2);
			} else if (RVOPC_MATCH(instr, C_SRAI)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = (sx_t)regs[regnum_rd] >> GETBITS(instr, 6, 2);
			} else if (RVOPC_MATCH(instr, C_ANDI)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[regnum_rd] & imm_ci(instr);
			} else if (RVOPC_MATCH(instr, C_SUB)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[c_rs1_s(instr)] - regs[c_rs2_s(instr)];
			} else if (RVOPC_MATCH(instr, C_XOR)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[c_rs1_s(instr)] ^ regs[c_rs2_s(instr)];
			} else if (RVOPC_MATCH(instr, C_OR)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[c_rs1_s(instr)] | regs[c_rs2_s(instr)];
			} else if (RVOPC_MATCH(instr, C_AND)) {
				regnum_rd = c_rs1_s(instr);
				rd_wdata = regs[c_rs1_s(instr)] & regs[c_rs2_s(instr)];
			} else if (RVOPC_MATCH(instr, C_J)) {
				pc_wdata = pc + imm_cj(instr);
			} else if (RVOPC_MATCH(instr, C_BEQZ)) {
				if (regs[c_rs1_s(instr)] == 0) {
					pc_wdata = pc + imm_cb(instr);
				}
			} else if (RVOPC_MATCH(instr, C_BNEZ)) {
				if (regs[c_rs1_s(instr)] != 0) {
					pc_wdata = pc + imm_cb(instr);
				}
			} else {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			}
		} else {
			// RVC Quadrant 10:
			if (RVOPC_MATCH(instr, C_SLLI)) {
				regnum_rd = c_rs1_l(instr);
				rd_wdata = regs[regnum_rd] << GETBITS(instr, 6, 2);
			} else if (RVOPC_MATCH(instr, C_MV)) {
				if (c_rs2_l(instr) == 0) {
					// c.jr
					pc_wdata = regs[c_rs1_l(instr)] & -2u;;
				} else {
					regnum_rd = c_rs1_l(instr);
					rd_wdata = regs[c_rs2_l(instr)];
				}
			} else if (RVOPC_MATCH(instr, C_ADD)) {
				if (c_rs2_l(instr) == 0) {
					if (c_rs1_l(instr) == 0) {
						// c.ebreak
						exception_cause = XCAUSE_EBREAK;
					} else {
						// c.jalr
						pc_wdata = regs[c_rs1_l(instr)] & -2u;
						regnum_rd = 1;
						rd_wdata = pc + 2;
					}
				} else {
					regnum_rd = c_rs1_l(instr);
					rd_wdata = regs[c_rs1_l(instr)] + regs[c_rs2_l(instr)];
				}
			} else if (RVOPC_MATCH(instr, C_LWSP)) {
				regnum_rd = c_rs1_l(instr);
				ux_t addr = regs[2]
					+ (GETBIT(instr, 12) << 5)
					+ (GETBITS(instr, 6, 4) << 2)
					+ (GETBITS(instr, 3, 2) << 6);
				rd_wdata = mem.r32(addr);
			} else if (RVOPC_MATCH(instr, C_SWSP)) {
				ux_t addr = regs[2]
					+ (GETBITS(instr, 12, 9) << 2)
					+ (GETBITS(instr, 8, 7) << 6);
				mem.w32(addr, regs[c_rs2_l(instr)]);
			// Zcmp:
			} else if (RVOPC_MATCH(instr, CM_PUSH)) {
				ux_t addr = regs[2];
				for (uint i = 31; i > 0; --i) {
					if (zcmp_reg_mask(instr) & (1u << i)) {
						addr -= 4;
						mem.w32(addr, regs[i]);
					}
				}
				regnum_rd = 2;
				rd_wdata = regs[2] - zcmp_stack_adj(instr);
			} else if (RVOPC_MATCH(instr, CM_POP) || RVOPC_MATCH(instr, CM_POPRET) || RVOPC_MATCH(instr, CM_POPRETZ)) {
				bool clear_a0 = RVOPC_MATCH(instr, CM_POPRETZ);
				bool ret = clear_a0 || RVOPC_MATCH(instr, CM_POPRET);
				ux_t addr = regs[2] + zcmp_stack_adj(instr);
				for (uint i = 31; i > 0; --i) {
					if (zcmp_reg_mask(instr) & (1u << i)) {
						addr -= 4;
						regs[i] = mem.r32(addr);
					}
				}
				if (clear_a0)
					regs[10] = 0;
				if (ret)
					pc_wdata = regs[1];
				regnum_rd = 2;
				rd_wdata = regs[2] + zcmp_stack_adj(instr);
			} else if (RVOPC_MATCH(instr, CM_MVSA01)) {
				regs[zcmp_s_mapping(GETBITS(instr, 9, 7))] = regs[10];
				regs[zcmp_s_mapping(GETBITS(instr, 4, 2))] = regs[11];
			} else if (RVOPC_MATCH(instr, CM_MVA01S)) {
				regs[10] = regs[zcmp_s_mapping(GETBITS(instr, 9, 7))];
				regs[11] = regs[zcmp_s_mapping(GETBITS(instr, 4, 2))];
			} else {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			}
		}


		if (trace) {
			printf("%08x: ", pc);
			if ((instr & 0x3) == 0x3) {
				printf("%08x : ", instr);
			} else {
				printf("    %04x : ", instr & 0xffffu);
			}
			if (regnum_rd != 0 && rd_wdata) {
				printf("%-3s  <- %08x ", friendly_reg_names[regnum_rd], *rd_wdata);
			} else {
				printf("                 ");
			}
			if (pc_wdata) {
				printf(": pc <- %08x\n", *pc_wdata);
			} else {
				printf(":\n");
			}
			if (*trace_csr_result) {
				printf("                   : #%03x <- %08x :\n", *trace_csr_addr, *trace_csr_result);
			}
		}

		if (exception_cause) {
			pc_wdata = csr.trap_enter(*exception_cause, pc);
			if (trace) {
				printf("^^^ Trap           : xcause <- %2u     : pc <- %08x\n", *exception_cause, *pc_wdata);
			}
		}

		if (pc_wdata)
			pc = *pc_wdata;
		else
			pc = pc + ((instr & 0x3) == 0x3 ? 4 : 2);
		if (rd_wdata && regnum_rd != 0)
			regs[regnum_rd] = *rd_wdata;
		csr.step();

	}
};


const char *help_str =
"Usage: tb [--bin x.bin] [--dump start end] [--vcd x.vcd] [--cycles n] [--cpuret]\n"
"    --bin x.bin      : Flat binary file loaded to address 0x0 in RAM\n"
"    --vcd x.vcd      : Dummy option for compatibility with CXXRTL tb\n"
"    --dump start end : Print out memory contents between start and end (exclusive)\n"
"                       after execution finishes. Can be passed multiple times.\n"
"    --cycles n       : Maximum number of cycles to run before exiting.\n"
"    --memsize n      : Memory size in units of 1024 bytes, default is 16 MB\n"
"    --trace          : Print out execution tracing info\n"
"    --cpuret         : Testbench's return code is the return code written to\n"
"                       IO_EXIT by the CPU, or -1 if timed out.\n";

void exit_help(std::string errtext = "") {
	std::cerr << errtext << help_str;
	exit(-1);
}

int main(int argc, char **argv) {
	if (argc < 2)
		exit_help();

	std::vector<std::tuple<uint32_t, uint32_t>> dump_ranges;
	int64_t max_cycles = 100000;
	uint32_t ramsize = 16 * (1 << 20);
	bool load_bin = false;
	std::string bin_path;
	bool trace_execution = false;
	bool propagate_return_code = false;

	for (int i = 1; i < argc; ++i) {
		std::string s(argv[i]);
		if (s == "--bin") {
			if (argc - i < 2)
				exit_help("Option --bin requires an argument\n");
			load_bin = true;
			bin_path = argv[i + 1];
			i += 1;
		}
		else if (s == "--vcd") {
			if (argc - i < 2)
				exit_help("Option --vcd requires an argument\n");
			// (We ignore this argument, it's supported for
			i += 1;
		}
		else if (s == "--dump") {
			if (argc - i < 3)
				exit_help("Option --dump requires 2 arguments\n");
			dump_ranges.push_back(std::make_tuple(
				std::stoul(argv[i + 1], 0, 0),
				std::stoul(argv[i + 2], 0, 0)
			));
			i += 2;
		}
		else if (s == "--cycles") {
			if (argc - i < 2)
				exit_help("Option --cycles requires an argument\n");
			max_cycles = std::stol(argv[i + 1], 0, 0);
			i += 1;
		}
		else if (s == "--memsize") {
			if (argc - i < 2)
				exit_help("Option --memsize requires an argument\n");
			ramsize = 1024 * std::stol(argv[i + 1], 0, 0);
			i += 1;
		}
		else if (s == "--trace") {
			trace_execution = true;
		}
		else if (s == "--cpuret") {
			propagate_return_code = true;
		}
		else {
			std::cerr << "Unrecognised argument " << s << "\n";
			exit_help("");
		}
	}

	FlatMem32 ram(ramsize);
	TBMemIO io;
	MemMap32 mem;
	mem.add(0, ramsize, &ram);
	mem.add(0x80000000u, 12, &io);

	if (load_bin) {
		std::ifstream fd(bin_path, std::ios::binary | std::ios::ate);
		std::streamsize bin_size = fd.tellg();
		if (bin_size > ramsize) {
			std::cerr << "Binary file (" << bin_size << " bytes) is larger than memory (" << ramsize << " bytes)\n";
			return -1;
		}
		fd.seekg(0, std::ios::beg);
		fd.read((char*)ram.mem, bin_size);
	}

	RVCore core;

	int64_t cyc;
	int rc = 0;
	try {
		for (cyc = 0; cyc < max_cycles; ++cyc)
			core.step(mem, trace_execution);
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
			printf("%02x%c", mem.r8(start + i), i % 16 == 15 ? '\n' : ' ');
		printf("\n");
	}

	return rc;
}
