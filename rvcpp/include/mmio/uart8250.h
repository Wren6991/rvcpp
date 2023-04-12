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

#define UART_LCR_DLAB           0x80  // Bank select for addrs 0, 1

struct UART8250: MemBase32 {

	uint8_t dll;
	uint8_t ier;
	uint8_t dlm;
	uint8_t iir;
	uint8_t lcr;
	uint8_t mcr;
	uint8_t scr;

	virtual bool w8(ux_t addr, uint8_t data) {
		if (addr == UART_THR_OFFSET && !(lcr & UART_LCR_DLAB)) {
			putchar((char)data);
		} else if (addr == UART_DLL_OFFSET && (lcr & UART_LCR_DLAB)) {
			dll = data;
		} else if (addr == UART_IER_OFFSET && !(lcr & UART_LCR_DLAB)) {
			ier = data & 0xf;
		} else if (addr == UART_DLM_OFFSET && (lcr & UART_LCR_DLAB)) {
			dlm = data;
		} else if (addr == UART_LCR_OFFSET) {
			lcr = data;
		} else if (addr == UART_MCR_OFFSET) {
			mcr = data;
		} else if (data == UART_SCR_OFFSET) {
			scr = data;
		} else if (addr > UART_SCR_OFFSET) {
			return false;
		}
		return true;
	}

	virtual std::optional<uint8_t> r8(ux_t addr) {
		if (addr == UART_RBR_OFFSET && !(lcr & UART_LCR_DLAB)) {
			return 0;
		} else if (addr == UART_DLL_OFFSET && (lcr & UART_LCR_DLAB)) {
			return dll;
		} else if (addr == UART_IER_OFFSET && !(lcr & UART_LCR_DLAB)) {
			return ier;
		} else if (addr == UART_DLM_OFFSET && (lcr & UART_LCR_DLAB)) {
			return dlm;
		} else if (addr == UART_IIR_OFFSET) {
			// TODO IRQs
			return 0; 
		} else if (addr == UART_LCR_OFFSET) {
			return lcr;
		} else if (addr == UART_MCR_OFFSET) {
			return mcr;
		} else if (addr == UART_LSR_OFFSET) {
			// We are always ready to accept new data.
			return UART_LSR_TEMT | UART_LSR_THRE;
		} else if (addr == UART_SCR_OFFSET) {
			return scr;
		} else {
			return 0;
		}
	}

};

#endif
