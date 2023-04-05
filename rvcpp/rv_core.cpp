#include "rv_core.h"
#include "rv_types.h"
#include "rv_mem.h"
#include "encoding/rv_opcodes.h"

#include <cstdio>
#include <cassert>

// Use unsigned arithmetic everywhere, with explicit sign extension as required.
static inline ux_t sext(ux_t bits, int sign_bit) {
	if (sign_bit >= XLEN - 1)
		return bits;
	else
		return (bits & ((1u << (sign_bit + 1)) - 1)) - ((bits & 1u << sign_bit) << 1);
}

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

void RVCore::step(MemBase32 &mem, bool trace) {
	std::optional<ux_t> rd_wdata;
	std::optional<ux_t> pc_wdata;
	std::optional<uint> exception_cause;
	std::optional<ux_t> xtval_wdata;
	uint regnum_rd = 0;

	std::optional<ux_t> trace_csr_addr;
	std::optional<ux_t> trace_csr_result;
	std::optional<uint> trace_priv;

	std::optional<uint16_t> fetch0, fetch1;
	std::optional<ux_t> fetch_paddr0 = vmap_fetch(pc);
	if (fetch_paddr0) {
		fetch0 = mem.r16(*fetch_paddr0);
	}
	std::optional<ux_t> fetch_paddr1 = vmap_fetch(pc + 2);
	if (fetch_paddr1) {
		fetch1 = mem.r16(*fetch_paddr1);
	}
	uint32_t instr = *fetch0 | (*fetch1 << 16);

	if (!fetch_paddr0 || (fetch0 && (*fetch0 & 0x3) == 0x3 && !fetch_paddr1)) {
		// Note xtval points to the virtual address which failed translation, which may be part
		// way through the instruction if the instruction crosses a page boundary.
		exception_cause = XCAUSE_INSTR_PAGEFAULT;
		xtval_wdata = fetch_paddr0 ? pc + 2 : pc;
	} else if (!fetch0 || ((*fetch0 & 0x3) == 0x3 && !fetch1)) {
		exception_cause = XCAUSE_INSTR_FAULT;
		xtval_wdata = *fetch0 ? pc + 2 : pc;
	} else if ((instr & 0x3) == 0x3) {
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
						mul_op_a -= (mul_op_a & (1 << (XLEN - 1))) << 1;
					if (funct3 < 0b010)
						mul_op_b -= (mul_op_b & (1 << (XLEN - 1))) << 1;
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
			ux_t load_addr_v = rs1 + imm_i(instr);
			ux_t align_mask = ~(-1u << (funct3 & 0x3));
			bool misalign = load_addr_v & align_mask;
			if (funct3 == 0b011 || funct3 > 0b101) {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			} else if (misalign) {
				exception_cause = XCAUSE_LOAD_ALIGN;
				xtval_wdata = load_addr_v;
			} else {
				std::optional<ux_t> load_addr_p = vmap_ls(load_addr_v, PTE_R);
				if (!load_addr_p) {
					exception_cause = XCAUSE_LOAD_PAGEFAULT;
				} else if (funct3 == 0b000) {
					rd_wdata = mem.r8(*load_addr_p);
					if (rd_wdata) {
						rd_wdata = sext(*rd_wdata, 7);
					} else {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else if (funct3 == 0b001) {
					rd_wdata = mem.r16(*load_addr_p);
					if (rd_wdata) {
						rd_wdata = sext(*rd_wdata, 15);
					} else {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else if (funct3 == 0b010) {
					rd_wdata = mem.r32(*load_addr_p);
					if (!rd_wdata) {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else if (funct3 == 0b100) {
					rd_wdata = mem.r8(*load_addr_p);
					if (!rd_wdata) {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else if (funct3 == 0b101) {
					rd_wdata = mem.r16(*load_addr_p);
					if (!rd_wdata) {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				}
				if (exception_cause) {
					xtval_wdata = load_addr_v;
				}
			}
			break;
		}

		case OPC_STORE: {
			ux_t store_addr_v = rs1 + imm_s(instr);
			ux_t align_mask = ~(-1u << (funct3 & 0x3));
			bool misalign = store_addr_v & align_mask;
			if (funct3 > 0b010) {
				exception_cause = XCAUSE_INSTR_ILLEGAL;
			} else if (misalign) {
				exception_cause = XCAUSE_STORE_ALIGN;
				xtval_wdata = store_addr_v;
			} else {
				std::optional<ux_t> store_addr_p = vmap_ls(store_addr_v, PTE_W);
				if (!store_addr_p) {
					exception_cause = XCAUSE_STORE_PAGEFAULT;
				} else if (funct3 == 0b000) {
					if (!mem.w8(*store_addr_p, rs2 & 0xffu)) {
						exception_cause = XCAUSE_STORE_FAULT;
					}
				} else if (funct3 == 0b001) {
					if (!mem.w16(*store_addr_p, rs2 & 0xffffu)) {
						exception_cause = XCAUSE_STORE_FAULT;
					}
				} else if (funct3 == 0b010) {
					if (!mem.w32(*store_addr_p, rs2)) {
						exception_cause = XCAUSE_STORE_FAULT;
					}
				}
				if (exception_cause) {
					xtval_wdata = store_addr_v;
				}
			}
			break;
		}

		case OPC_AMO: {
			if (RVOPC_MATCH(instr, LR_W)) {
				if (rs1 & 0x3) {
					exception_cause = XCAUSE_LOAD_ALIGN;
				} else {
					std::optional<ux_t> lr_addr_p = vmap_ls(rs1, PTE_R);
					if (!lr_addr_p) {
						exception_cause = XCAUSE_LOAD_PAGEFAULT;
					} else {
						rd_wdata = mem.r32(*lr_addr_p);
						if (rd_wdata) {
							load_reserved = true;
						} else {
							exception_cause = XCAUSE_LOAD_FAULT;
						}
					}
				}
				if (exception_cause) {
					xtval_wdata = rs1;
				}
			} else if (RVOPC_MATCH(instr, SC_W)) {
				if (rs1 & 0x3) {
					exception_cause = XCAUSE_STORE_ALIGN;
				} else {
					if (load_reserved) {
						std::optional<ux_t> sc_addr_p = vmap_ls(rs1, PTE_W);
						if (!sc_addr_p) {
							exception_cause = XCAUSE_STORE_PAGEFAULT;
						} else {
							load_reserved = false;
							if (mem.w32(*sc_addr_p, rs2)) {
								rd_wdata = 0;
							} else {
								exception_cause = XCAUSE_STORE_FAULT;
							}
						}
					} else {
						rd_wdata = 1;
					}
					if (exception_cause) {
						xtval_wdata = rs1;
					}
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
					std::optional<ux_t> amo_addr_p = vmap_ls(rs1, PTE_W | PTE_R);
					if (!amo_addr_p) {
						exception_cause = XCAUSE_STORE_PAGEFAULT;
					} else {
						rd_wdata = mem.r32(*amo_addr_p);
						if (!rd_wdata) {
							exception_cause = XCAUSE_STORE_FAULT; // Yes, AMO/Store
						} else {
							bool write_success = false;
							switch (instr & RVOPC_AMOSWAP_W_MASK) {
								case RVOPC_AMOSWAP_W_BITS: write_success = mem.w32(*amo_addr_p, rs2);                                            break;
								case RVOPC_AMOADD_W_BITS:  write_success = mem.w32(*amo_addr_p, *rd_wdata + rs2);                                break;
								case RVOPC_AMOXOR_W_BITS:  write_success = mem.w32(*amo_addr_p, *rd_wdata ^ rs2);                                break;
								case RVOPC_AMOAND_W_BITS:  write_success = mem.w32(*amo_addr_p, *rd_wdata & rs2);                                break;
								case RVOPC_AMOOR_W_BITS:   write_success = mem.w32(*amo_addr_p, *rd_wdata | rs2);                                break;
								case RVOPC_AMOMIN_W_BITS:  write_success = mem.w32(*amo_addr_p, (sx_t)*rd_wdata < (sx_t)rs2 ? *rd_wdata : rs2);  break;
								case RVOPC_AMOMAX_W_BITS:  write_success = mem.w32(*amo_addr_p, (sx_t)*rd_wdata > (sx_t)rs2 ? *rd_wdata : rs2);  break;
								case RVOPC_AMOMINU_W_BITS: write_success = mem.w32(*amo_addr_p, *rd_wdata < rs2 ? *rd_wdata : rs2);              break;
								case RVOPC_AMOMAXU_W_BITS: write_success = mem.w32(*amo_addr_p, *rd_wdata > rs2 ? *rd_wdata : rs2);              break;
								default:                   assert(false);                                                                        break;
							}
							if (!write_success) {
								exception_cause = XCAUSE_STORE_FAULT;
							}
						}
					}
				}
				if (exception_cause) {
					xtval_wdata = rs1;
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
				if (csr.get_true_priv() == PRV_M) {
					pc_wdata = csr.trap_mret();
					if (trace) {
						trace_priv = csr.get_true_priv();
					}
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
			} else if (RVOPC_MATCH(instr, SRET)) {
				if (csr.get_true_priv() >= PRV_S) {
					pc_wdata = csr.trap_sret(pc);
					if (trace) {
						trace_priv = csr.get_true_priv();
					}
				} else {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
			} else if (RVOPC_MATCH(instr, SFENCE_VMA)) {
				if (!csr.permit_sfence_vma()) {
					exception_cause = XCAUSE_INSTR_ILLEGAL;
				}
				// Otherwise nop.
			} else if (RVOPC_MATCH(instr, ECALL)) {
				exception_cause = XCAUSE_ECALL_U + csr.get_true_priv();
				xtval_wdata = 0;
			} else if (RVOPC_MATCH(instr, EBREAK)) {
				exception_cause = XCAUSE_EBREAK;
				xtval_wdata = 0;
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
			ux_t addr_v = regs[c_rs1_s(instr)]
				+ (GETBIT(instr, 6) << 2)
				+ (GETBITS(instr, 12, 10) << 3)
				+ (GETBIT(instr, 5) << 6);
			if (addr_v & 0x3) {
				exception_cause = XCAUSE_LOAD_ALIGN;
			} else {
				std::optional<ux_t> addr_p = vmap_ls(addr_v, PTE_R);
				if (addr_p) {
					rd_wdata = mem.r32(*addr_p);
					if (!rd_wdata) {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else {
					exception_cause = XCAUSE_LOAD_PAGEFAULT;
				}
			}
			if (exception_cause) {
				xtval_wdata = addr_v;
			}
		} else if (RVOPC_MATCH(instr, C_SW)) {
			ux_t addr_v = regs[c_rs1_s(instr)]
				+ (GETBIT(instr, 6) << 2)
				+ (GETBITS(instr, 12, 10) << 3)
				+ (GETBIT(instr, 5) << 6);
			if (addr_v & 0x3) {
				exception_cause = XCAUSE_STORE_ALIGN;
			} else {
				std::optional<ux_t> addr_p = vmap_ls(addr_v, PTE_W);
				if (addr_p) {
					if (!mem.w32(*addr_p, regs[c_rs2_s(instr)])) {
						exception_cause = XCAUSE_STORE_FAULT;
					}
				} else {
					exception_cause = XCAUSE_STORE_PAGEFAULT;
				}
			}
			if (exception_cause) {
				xtval_wdata = addr_v;
			}
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
					xtval_wdata = 0;
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
			ux_t addr_v = regs[2]
				+ (GETBIT(instr, 12) << 5)
				+ (GETBITS(instr, 6, 4) << 2)
				+ (GETBITS(instr, 3, 2) << 6);
			if (addr_v & 0x3) {
				exception_cause = XCAUSE_LOAD_ALIGN;
			} else {
				std::optional<ux_t> addr_p = vmap_ls(addr_v, PTE_R);
				if (addr_p) {
					rd_wdata = mem.r32(*addr_p);
					if (!rd_wdata) {
						exception_cause = XCAUSE_LOAD_FAULT;
					}
				} else {
					exception_cause = XCAUSE_LOAD_PAGEFAULT;
				}
			}
			if (exception_cause) {
				xtval_wdata = addr_v;
			}
		} else if (RVOPC_MATCH(instr, C_SWSP)) {
			ux_t addr_v = regs[2]
				+ (GETBITS(instr, 12, 9) << 2)
				+ (GETBITS(instr, 8, 7) << 6);
			if (addr_v & 0x3) {
				exception_cause = XCAUSE_STORE_ALIGN;
			} else {
				std::optional<ux_t> addr_p = vmap_ls(addr_v, PTE_W);
				if (addr_p) {
					if (!mem.w32(*addr_p, regs[c_rs2_l(instr)])) {
						exception_cause = XCAUSE_STORE_FAULT;
					}
				} else {
					exception_cause = XCAUSE_STORE_PAGEFAULT;
				}
			}
			if (exception_cause) {
				xtval_wdata = addr_v;
			}
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
			printf("%-3s   <- %08x ", friendly_reg_names[regnum_rd], *rd_wdata);
		} else {
			printf("                  ");
		}
		if (pc_wdata) {
			printf(": pc <- %08x\n", *pc_wdata);
		} else {
			printf(":\n");
		}
		if (*trace_csr_result) {
			printf("                   : #%03x  <- %08x :\n", *trace_csr_addr, *trace_csr_result);
		}
	}

	if (exception_cause) {
		if (*exception_cause == XCAUSE_INSTR_ILLEGAL && !xtval_wdata) {
			xtval_wdata = instr & ((instr & 0x3) == 0x3 ? 0xffffffffu : 0x0000ffffu);
		}
		pc_wdata = csr.trap_enter_exception(*exception_cause, pc);
		if (xtval_wdata) {
			csr.trap_set_xtval(*xtval_wdata);
		}
		if (trace) {
			printf("^^^ Trap           : cause <- %-2u       : pc <- %08x\n", *exception_cause, *pc_wdata);
			trace_priv = csr.get_true_priv();
		}
	} else {
		std::optional<ux_t> irq_target_pc = csr.trap_check_enter_irq(pc_wdata ? *pc_wdata : pc);
		if (irq_target_pc) {
			pc_wdata = irq_target_pc;
			if (trace)
				printf("^^^ IRQ            : priv  <- %c        : pc <- %08x\n", "US.M"[csr.get_true_priv() & 0x3], *pc_wdata);
		}
	}

	if (trace && trace_priv) {
		printf("|||                : priv  <- %c        :\n", "US.M"[*trace_priv & 0x3]);
	}
	if (trace && xtval_wdata) {
		printf("|||                : tval  <- %08x :\n", *xtval_wdata);
	}

	if (pc_wdata)
		pc = *pc_wdata;
	else
		pc = pc + ((instr & 0x3) == 0x3 ? 4 : 2);
	if (rd_wdata && regnum_rd != 0)
		regs[regnum_rd] = *rd_wdata;

	csr.step_counters();
}
