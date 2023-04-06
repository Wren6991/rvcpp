#ifndef _MMIO_UART8250_H
#define _MMIO_UART8250_H

#include <cstdint>
#include <cstdio>
#include <optional>

#include "rv_mem.h"

// Mock of a standard 8250 UART. Enough for OpenSBI to implement blocking
// putc/getc, but no IRQ support etc.

// Register definitions straight out of OpenSBI:

#define UART_RBR_OFFSET  0  // In:  Recieve Buffer Register
#define UART_THR_OFFSET  0  // Out: Transmitter Holding Register
#define UART_DLL_OFFSET  0  // Out: Divisor Latch Low
#define UART_IER_OFFSET  1  // I/O: Interrupt Enable Register
#define UART_DLM_OFFSET  1  // Out: Divisor Latch High
#define UART_FCR_OFFSET  2  // Out: FIFO Control Register
#define UART_IIR_OFFSET  2  // I/O: Interrupt Identification Register
#define UART_LCR_OFFSET  3  // Out: Line Control Register
#define UART_MCR_OFFSET  4  // Out: Modem Control Register
#define UART_LSR_OFFSET  5  // In:  Line Status Register
#define UART_MSR_OFFSET  6  // In:  Modem Status Register
#define UART_SCR_OFFSET  7  // I/O: Scratch Register
#define UART_MDR1_OFFSET 8  // I/O:  Mode Register

#define UART_LSR_FIFOE          0x80  // Fifo error
#define UART_LSR_TEMT           0x40  // Transmitter empty
#define UART_LSR_THRE           0x20  // Transmit-hold-register empty
#define UART_LSR_BI             0x10  // Break interrupt indicator
#define UART_LSR_FE             0x08  // Frame error indicator
#define UART_LSR_PE             0x04  // Parity error indicator
#define UART_LSR_OE             0x02  // Overrun error indicator
#define UART_LSR_DR             0x01  // Receiver data ready
#define UART_LSR_BRK_ERROR_BITS 0x1E  // BI, FE, PE, OE bits


struct UART8250: MemBase32 {

	virtual bool w8(ux_t addr, uint8_t data) {
		if (addr == UART_THR_OFFSET) {
			putchar((char)data);
		}
		return true;
	}

	virtual std::optional<uint8_t> r8(ux_t addr) {
		if (addr == UART_LSR_OFFSET) {
			return UART_LSR_TEMT | UART_LSR_THRE;
		} else {
			return 0;
		}
	}

};

#endif
