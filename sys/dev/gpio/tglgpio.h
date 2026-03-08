/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Framework Computer Inc
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _DEV_GPIO_TGLGPIO_H_
#define _DEV_GPIO_TGLGPIO_H_

#include <sys/types.h>
#include <sys/bus.h>

/*
 * Tiger Lake-LP GPIO controller register definitions.
 * Reference: Intel Tiger Lake PCH datasheet, Linux pinctrl-tigerlake.c
 */

/* Community-level registers (relative to community MMIO base) */
#define TGLGPIO_REVID		0x000
#define TGLGPIO_PADBAR		0x00C
#define TGLGPIO_CAPLIST		0x010
#define TGLGPIO_GPI_IS		0x100	/* Interrupt Status */
#define TGLGPIO_GPI_IE		0x120	/* Interrupt Enable */

/* PADCFG0 register bits */
#define TGLGPIO_PADCFG0_GPIORXSTATE	(1u << 1)
#define TGLGPIO_PADCFG0_GPIOTXSTATE	(1u << 0)
#define TGLGPIO_PADCFG0_GPIORXDIS	(1u << 9)
#define TGLGPIO_PADCFG0_GPIOTXDIS	(1u << 8)
#define TGLGPIO_PADCFG0_RXEVCFG_SHIFT	25
#define TGLGPIO_PADCFG0_RXEVCFG_MASK	(3u << 25)
#define TGLGPIO_PADCFG0_RXEVCFG_LEVEL	(0u << 25)
#define TGLGPIO_PADCFG0_RXEVCFG_EDGE	(1u << 25)
#define TGLGPIO_PADCFG0_RXEVCFG_DIS	(2u << 25)
#define TGLGPIO_PADCFG0_RXEVCFG_BOTH	(3u << 25) /* edge on both */
#define TGLGPIO_PADCFG0_RXINV		(1u << 23)
#define TGLGPIO_PADCFG0_PMODE_MASK	(0xfu << 10)

/* Pad stride (bytes per pad register block) */
#define TGLGPIO_PAD_STRIDE	16

/* Max GPIO pins across all communities */
#define TGLGPIO_PIN_MAX		456

/* Maximum number of communities */
#define TGLGPIO_MAX_COMMUNITIES	4

/* Maximum per-pin interrupt handlers */
#define TGLGPIO_MAX_PINS	TGLGPIO_PIN_MAX

/*
 * Pin group descriptor: defines a block of GPIO pins within a community.
 */
struct tglgpio_group {
	const char	*name;		/* e.g., "GPP_B" */
	uint16_t	gpio_base;	/* first GPIO number */
	uint16_t	npins;		/* number of pins in group */
};

/*
 * Community descriptor: one MMIO region containing one or more pin groups.
 */
struct tglgpio_community {
	int		barno;		/* ACPI BAR index (resource ID) */
	const struct tglgpio_group *groups;
	int		ngroups;
};

/*
 * Per-pin interrupt handler entry.
 */
struct tglgpio_pin_intr {
	driver_filter_t	*handler;
	void		*arg;
};

/*
 * Softc for the Tiger Lake GPIO controller.
 */
struct tglgpio_softc {
	device_t	sc_dev;
	device_t	sc_busdev;
	struct mtx	sc_mtx;
	ACPI_HANDLE	sc_handle;

	/* Per-community resources */
	struct resource	*sc_mem_res[TGLGPIO_MAX_COMMUNITIES];
	int		sc_mem_rid[TGLGPIO_MAX_COMMUNITIES];
	bus_size_t	sc_padbar[TGLGPIO_MAX_COMMUNITIES];

	/* Shared interrupt */
	struct resource	*sc_irq_res;
	int		sc_irq_rid;
	void		*sc_irq_handle;

	/* Per-pin interrupt dispatch table */
	struct tglgpio_pin_intr	sc_pin_intrs[TGLGPIO_MAX_PINS];

	/* Community table (set at probe/attach) */
	const struct tglgpio_community *sc_communities;
	int		sc_ncommunities;
};

/*
 * Exported per-pin interrupt API for client drivers (e.g., acpi_soc_button).
 * These bypass INTRNG since it is not available on x86/amd64 FreeBSD.
 */
int tglgpio_setup_intr(device_t dev, uint32_t pin, uint32_t mode,
    driver_filter_t *handler, void *arg);
int tglgpio_teardown_intr(device_t dev, uint32_t pin);

#endif /* _DEV_GPIO_TGLGPIO_H_ */
