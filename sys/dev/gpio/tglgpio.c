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

/*
 * Intel Tiger Lake-LP GPIO controller driver.
 *
 * Provides GPIO pin access and per-pin interrupt dispatch for Tiger Lake
 * platforms.  Since FreeBSD x86/amd64 does not have INTRNG, this driver
 * implements its own interrupt routing via exported tglgpio_setup_intr()
 * and tglgpio_teardown_intr() functions.
 *
 * Pin numbering matches the ACPI GpioInt pin namespace so that ACPI
 * consumers can reference pins directly.
 *
 * Reference: Linux pinctrl-tigerlake.c, pinctrl-intel.c
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/gpio.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/malloc.h>

#include <machine/bus.h>
#include <machine/resource.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/gpio/gpiobusvar.h>

#include "opt_acpi.h"
#include "opt_platform.h"
#include "gpio_if.h"

#include "tglgpio.h"

#define TGLGPIO_LOCK(_sc)		mtx_lock_spin(&(_sc)->sc_mtx)
#define TGLGPIO_UNLOCK(_sc)		mtx_unlock_spin(&(_sc)->sc_mtx)
#define TGLGPIO_LOCK_INIT(_sc)		\
	mtx_init(&(_sc)->sc_mtx, device_get_nameunit((_sc)->sc_dev), \
	    "tglgpio", MTX_SPIN)
#define TGLGPIO_LOCK_DESTROY(_sc)	mtx_destroy(&(_sc)->sc_mtx)

/*
 * Tiger Lake-LP pin group tables.
 * From Linux drivers/pinctrl/intel/pinctrl-tigerlake.c
 */

/* Community 0 (barno 0): GPP_B, GPP_T, GPP_A */
static const struct tglgpio_group tgl_community0_groups[] = {
	{ "GPP_B",	0,	26 },
	{ "GPP_T",	32,	16 },
	{ "GPP_A",	64,	25 },
};

/* Community 1 (barno 1): GPP_S, GPP_H, GPP_D, GPP_U, vGPIO */
static const struct tglgpio_group tgl_community1_groups[] = {
	{ "GPP_S",	96,	8 },
	{ "GPP_H",	128,	24 },
	{ "GPP_D",	160,	21 },
	{ "GPP_U",	192,	24 },
	{ "vGPIO",	224,	27 },
};

/* Community 4 (barno 2): GPP_C, GPP_F, HVCMOS, GPP_E, JTAG */
static const struct tglgpio_group tgl_community4_groups[] = {
	{ "GPP_C",	256,	24 },
	{ "GPP_F",	288,	25 },
	{ "HVCMOS",	320,	6 },
	{ "GPP_E",	352,	25 },
	{ "JTAG",	384,	9 },
};

/* Community 5 (barno 3): GPP_R, SPI */
static const struct tglgpio_group tgl_community5_groups[] = {
	{ "GPP_R",	416,	8 },
	{ "SPI",	448,	9 },
};

static const struct tglgpio_community tgl_communities[] = {
	{ 0, tgl_community0_groups, nitems(tgl_community0_groups) },
	{ 1, tgl_community1_groups, nitems(tgl_community1_groups) },
	{ 2, tgl_community4_groups, nitems(tgl_community4_groups) },
	{ 3, tgl_community5_groups, nitems(tgl_community5_groups) },
};

/*
 * Map a GPIO number to its community index, group, and pad offset within
 * the group.  Returns 0 on success, EINVAL if the GPIO number is invalid
 * (falls in a gap or is out of range).
 */
static int
tglgpio_pin_lookup(struct tglgpio_softc *sc, uint32_t pin,
    int *comm_idx, const struct tglgpio_group **grpp, int *pad_in_group)
{
	int ci, gi;

	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		const struct tglgpio_community *comm = &sc->sc_communities[ci];
		for (gi = 0; gi < comm->ngroups; gi++) {
			const struct tglgpio_group *grp = &comm->groups[gi];
			if (pin >= grp->gpio_base &&
			    pin < grp->gpio_base + grp->npins) {
				if (comm_idx != NULL)
					*comm_idx = ci;
				if (grpp != NULL)
					*grpp = grp;
				if (pad_in_group != NULL)
					*pad_in_group = pin - grp->gpio_base;
				return (0);
			}
		}
	}
	return (EINVAL);
}

/*
 * Compute the cumulative pad offset of a pin within its community
 * (i.e., how many pads precede this pin across all groups in the
 * same community).  Used to calculate the PADCFG0 register offset.
 */
static int
tglgpio_pad_offset_in_community(const struct tglgpio_community *comm,
    uint32_t pin)
{
	int gi, offset;

	offset = 0;
	for (gi = 0; gi < comm->ngroups; gi++) {
		const struct tglgpio_group *grp = &comm->groups[gi];
		if (pin >= grp->gpio_base &&
		    pin < grp->gpio_base + grp->npins) {
			return (offset + (pin - grp->gpio_base));
		}
		offset += grp->npins;
	}
	return (-1);
}

/*
 * Read PADCFG0 for a given GPIO pin.
 * Caller must hold sc_mtx.
 */
static uint32_t
tglgpio_read_padcfg0(struct tglgpio_softc *sc, uint32_t pin)
{
	int ci;
	const struct tglgpio_community *comm;
	int pad_off;
	bus_size_t reg;

	if (tglgpio_pin_lookup(sc, pin, &ci, NULL, NULL) != 0)
		return (0);
	comm = &sc->sc_communities[ci];
	pad_off = tglgpio_pad_offset_in_community(comm, pin);
	reg = sc->sc_padbar[ci] + (bus_size_t)pad_off * TGLGPIO_PAD_STRIDE;
	return (bus_read_4(sc->sc_mem_res[ci], reg));
}

/*
 * Write PADCFG0 for a given GPIO pin.
 * Caller must hold sc_mtx.
 */
static void
tglgpio_write_padcfg0(struct tglgpio_softc *sc, uint32_t pin, uint32_t val)
{
	int ci;
	const struct tglgpio_community *comm;
	int pad_off;
	bus_size_t reg;

	if (tglgpio_pin_lookup(sc, pin, &ci, NULL, NULL) != 0)
		return;
	comm = &sc->sc_communities[ci];
	pad_off = tglgpio_pad_offset_in_community(comm, pin);
	reg = sc->sc_padbar[ci] + (bus_size_t)pad_off * TGLGPIO_PAD_STRIDE;
	bus_write_4(sc->sc_mem_res[ci], reg, val);
}

/*
 * GPIO interface methods.
 */

static device_t
tglgpio_get_bus(device_t dev)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	return (sc->sc_busdev);
}

static int
tglgpio_pin_max(device_t dev, int *maxpin)
{
	*maxpin = TGLGPIO_PIN_MAX - 1;
	return (0);
}

static int
tglgpio_pin_getname(device_t dev, uint32_t pin, char *name)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	const struct tglgpio_group *grp;
	int pad_in_group;

	if (tglgpio_pin_lookup(sc, pin, NULL, &grp, &pad_in_group) != 0) {
		snprintf(name, GPIOMAXNAME, "pin%u", pin);
		return (0);
	}
	snprintf(name, GPIOMAXNAME, "%s_%d", grp->name, pad_in_group);
	return (0);
}

static int
tglgpio_pin_getcaps(device_t dev, uint32_t pin, uint32_t *caps)
{
	struct tglgpio_softc *sc = device_get_softc(dev);

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);
	*caps = GPIO_PIN_INPUT | GPIO_PIN_OUTPUT |
	    GPIO_INTR_EDGE_RISING | GPIO_INTR_EDGE_FALLING |
	    GPIO_INTR_EDGE_BOTH | GPIO_INTR_LEVEL_HIGH | GPIO_INTR_LEVEL_LOW;
	return (0);
}

static int
tglgpio_pin_getflags(device_t dev, uint32_t pin, uint32_t *flags)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);

	TGLGPIO_LOCK(sc);
	val = tglgpio_read_padcfg0(sc, pin);
	TGLGPIO_UNLOCK(sc);

	*flags = 0;
	if (!(val & TGLGPIO_PADCFG0_GPIOTXDIS))
		*flags |= GPIO_PIN_OUTPUT;
	if (!(val & TGLGPIO_PADCFG0_GPIORXDIS))
		*flags |= GPIO_PIN_INPUT;
	return (0);
}

static int
tglgpio_pin_setflags(device_t dev, uint32_t pin, uint32_t flags)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);

	TGLGPIO_LOCK(sc);
	val = tglgpio_read_padcfg0(sc, pin);
	val &= ~(TGLGPIO_PADCFG0_GPIORXDIS | TGLGPIO_PADCFG0_GPIOTXDIS);
	if (!(flags & GPIO_PIN_INPUT))
		val |= TGLGPIO_PADCFG0_GPIORXDIS;
	if (!(flags & GPIO_PIN_OUTPUT))
		val |= TGLGPIO_PADCFG0_GPIOTXDIS;
	tglgpio_write_padcfg0(sc, pin, val);
	TGLGPIO_UNLOCK(sc);

	return (0);
}

static int
tglgpio_pin_get(device_t dev, uint32_t pin, unsigned int *value)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);

	TGLGPIO_LOCK(sc);
	val = tglgpio_read_padcfg0(sc, pin);
	TGLGPIO_UNLOCK(sc);

	*value = (val & TGLGPIO_PADCFG0_GPIORXSTATE) ?
	    GPIO_PIN_HIGH : GPIO_PIN_LOW;
	return (0);
}

static int
tglgpio_pin_set(device_t dev, uint32_t pin, unsigned int value)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);

	TGLGPIO_LOCK(sc);
	val = tglgpio_read_padcfg0(sc, pin);
	if (value == GPIO_PIN_LOW)
		val &= ~TGLGPIO_PADCFG0_GPIOTXSTATE;
	else
		val |= TGLGPIO_PADCFG0_GPIOTXSTATE;
	tglgpio_write_padcfg0(sc, pin, val);
	TGLGPIO_UNLOCK(sc);

	return (0);
}

static int
tglgpio_pin_toggle(device_t dev, uint32_t pin)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (tglgpio_pin_lookup(sc, pin, NULL, NULL, NULL) != 0)
		return (EINVAL);

	TGLGPIO_LOCK(sc);
	val = tglgpio_read_padcfg0(sc, pin);
	val ^= TGLGPIO_PADCFG0_GPIOTXSTATE;
	tglgpio_write_padcfg0(sc, pin, val);
	TGLGPIO_UNLOCK(sc);

	return (0);
}

/*
 * Shared interrupt filter handler.
 * Scans all communities/groups for pending interrupts and dispatches
 * to registered per-pin handlers.
 */
static int
tglgpio_intr(void *arg)
{
	struct tglgpio_softc *sc = arg;
	int ci, gi, bit, handled;
	uint32_t status, enabled, pending, gpio_num;

	handled = 0;
	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		const struct tglgpio_community *comm = &sc->sc_communities[ci];
		for (gi = 0; gi < comm->ngroups; gi++) {
			const struct tglgpio_group *grp = &comm->groups[gi];
			bus_size_t is_reg = TGLGPIO_GPI_IS + (bus_size_t)gi * 4;
			bus_size_t ie_reg = TGLGPIO_GPI_IE + (bus_size_t)gi * 4;

			status = bus_read_4(sc->sc_mem_res[ci], is_reg);
			enabled = bus_read_4(sc->sc_mem_res[ci], ie_reg);
			pending = status & enabled;

			while (pending != 0) {
				bit = ffs(pending) - 1;
				pending &= ~(1u << bit);
				gpio_num = grp->gpio_base + bit;

				if (gpio_num < TGLGPIO_MAX_PINS &&
				    sc->sc_pin_intrs[gpio_num].handler != NULL) {
					sc->sc_pin_intrs[gpio_num].handler(
					    sc->sc_pin_intrs[gpio_num].arg);
				}

				/* Clear this interrupt */
				bus_write_4(sc->sc_mem_res[ci], is_reg,
				    1u << bit);
				handled = 1;
			}
		}
	}
	return (handled ? FILTER_HANDLED : FILTER_STRAY);
}

/*
 * Exported per-pin interrupt setup.
 * Configures pad interrupt mode and registers a filter handler.
 */
int
tglgpio_setup_intr(device_t dev, uint32_t pin, uint32_t mode,
    driver_filter_t *handler, void *arg)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	int ci, pad_in_group;
	const struct tglgpio_group *grp;
	const struct tglgpio_community *comm;
	uint32_t val;
	bus_size_t ie_reg;
	int gi;

	if (pin >= TGLGPIO_MAX_PINS || handler == NULL)
		return (EINVAL);
	if (tglgpio_pin_lookup(sc, pin, &ci, &grp, &pad_in_group) != 0)
		return (EINVAL);

	comm = &sc->sc_communities[ci];

	/* Find group index within community for IE register */
	for (gi = 0; gi < comm->ngroups; gi++) {
		if (&comm->groups[gi] == grp)
			break;
	}

	TGLGPIO_LOCK(sc);

	/* Configure PADCFG0: set RXEVCFG based on requested interrupt mode */
	val = tglgpio_read_padcfg0(sc, pin);

	/* Clear RXEVCFG and RXINV bits */
	val &= ~(TGLGPIO_PADCFG0_RXEVCFG_MASK | TGLGPIO_PADCFG0_RXINV);

	/* Ensure RX is enabled */
	val &= ~TGLGPIO_PADCFG0_GPIORXDIS;

	if (mode & GPIO_INTR_EDGE_BOTH) {
		val |= TGLGPIO_PADCFG0_RXEVCFG_BOTH;
	} else if (mode & GPIO_INTR_EDGE_RISING) {
		val |= TGLGPIO_PADCFG0_RXEVCFG_EDGE;
	} else if (mode & GPIO_INTR_EDGE_FALLING) {
		val |= TGLGPIO_PADCFG0_RXEVCFG_EDGE;
		val |= TGLGPIO_PADCFG0_RXINV;
	} else if (mode & GPIO_INTR_LEVEL_HIGH) {
		val |= TGLGPIO_PADCFG0_RXEVCFG_LEVEL;
	} else if (mode & GPIO_INTR_LEVEL_LOW) {
		val |= TGLGPIO_PADCFG0_RXEVCFG_LEVEL;
		val |= TGLGPIO_PADCFG0_RXINV;
	}

	tglgpio_write_padcfg0(sc, pin, val);

	/* Store handler */
	sc->sc_pin_intrs[pin].handler = handler;
	sc->sc_pin_intrs[pin].arg = arg;

	/* Enable interrupt: set bit in GPI_IE */
	ie_reg = TGLGPIO_GPI_IE + (bus_size_t)gi * 4;
	val = bus_read_4(sc->sc_mem_res[ci], ie_reg);
	val |= (1u << pad_in_group);
	bus_write_4(sc->sc_mem_res[ci], ie_reg, val);

	TGLGPIO_UNLOCK(sc);

	device_printf(sc->sc_dev, "interrupt enabled for pin %u\n", pin);
	return (0);
}

/*
 * Exported per-pin interrupt teardown.
 */
int
tglgpio_teardown_intr(device_t dev, uint32_t pin)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	int ci, pad_in_group;
	const struct tglgpio_group *grp;
	const struct tglgpio_community *comm;
	uint32_t val;
	bus_size_t ie_reg, is_reg;
	int gi;

	if (pin >= TGLGPIO_MAX_PINS)
		return (EINVAL);
	if (tglgpio_pin_lookup(sc, pin, &ci, &grp, &pad_in_group) != 0)
		return (EINVAL);

	comm = &sc->sc_communities[ci];
	for (gi = 0; gi < comm->ngroups; gi++) {
		if (&comm->groups[gi] == grp)
			break;
	}

	TGLGPIO_LOCK(sc);

	/* Disable interrupt: clear bit in GPI_IE */
	ie_reg = TGLGPIO_GPI_IE + (bus_size_t)gi * 4;
	val = bus_read_4(sc->sc_mem_res[ci], ie_reg);
	val &= ~(1u << pad_in_group);
	bus_write_4(sc->sc_mem_res[ci], ie_reg, val);

	/* Clear handler */
	sc->sc_pin_intrs[pin].handler = NULL;
	sc->sc_pin_intrs[pin].arg = NULL;

	/* Clear any pending interrupt status */
	is_reg = TGLGPIO_GPI_IS + (bus_size_t)gi * 4;
	bus_write_4(sc->sc_mem_res[ci], is_reg, 1u << pad_in_group);

	TGLGPIO_UNLOCK(sc);

	return (0);
}

/*
 * Mask all interrupts and clear pending status for a community.
 */
static void
tglgpio_mask_community(struct tglgpio_softc *sc, int ci)
{
	const struct tglgpio_community *comm = &sc->sc_communities[ci];
	int gi;

	for (gi = 0; gi < comm->ngroups; gi++) {
		bus_write_4(sc->sc_mem_res[ci],
		    TGLGPIO_GPI_IE + (bus_size_t)gi * 4, 0);
		bus_write_4(sc->sc_mem_res[ci],
		    TGLGPIO_GPI_IS + (bus_size_t)gi * 4, 0xFFFFFFFFu);
	}
}

/*
 * ACPI probe/attach/detach.
 */

static char *tglgpio_ids[] = {
	"INTC1055",	/* Tiger Lake-LP */
	NULL
};

static int
tglgpio_probe(device_t dev)
{
	int rv;

	if (acpi_disabled("tglgpio"))
		return (ENXIO);
	rv = ACPI_ID_PROBE(device_get_parent(dev), dev, tglgpio_ids, NULL);
	if (rv <= 0)
		device_set_desc(dev, "Intel Tiger Lake-LP GPIO");
	return (rv);
}

static int
tglgpio_attach(device_t dev)
{
	struct tglgpio_softc *sc;
	int ci, error;

	sc = device_get_softc(dev);
	sc->sc_dev = dev;
	sc->sc_handle = acpi_get_handle(dev);
	sc->sc_communities = tgl_communities;
	sc->sc_ncommunities = nitems(tgl_communities);

	TGLGPIO_LOCK_INIT(sc);
	memset(sc->sc_pin_intrs, 0, sizeof(sc->sc_pin_intrs));

	/* Allocate memory resources for each community */
	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		sc->sc_mem_rid[ci] = sc->sc_communities[ci].barno;
		sc->sc_mem_res[ci] = bus_alloc_resource_any(dev,
		    SYS_RES_MEMORY, &sc->sc_mem_rid[ci], RF_ACTIVE);
		if (sc->sc_mem_res[ci] == NULL) {
			device_printf(dev,
			    "can't allocate memory for community %d\n", ci);
			goto fail;
		}

		/* Read PADBAR to find pad register base */
		sc->sc_padbar[ci] = bus_read_4(sc->sc_mem_res[ci],
		    TGLGPIO_PADBAR);

		/* Mask all interrupts and clear status */
		tglgpio_mask_community(sc, ci);
	}

	/* Allocate shared IRQ resource */
	sc->sc_irq_rid = 0;
	sc->sc_irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ,
	    &sc->sc_irq_rid, RF_ACTIVE | RF_SHAREABLE);
	if (sc->sc_irq_res == NULL) {
		device_printf(dev, "can't allocate IRQ resource\n");
		goto fail;
	}

	error = bus_setup_intr(dev, sc->sc_irq_res,
	    INTR_TYPE_MISC | INTR_MPSAFE, tglgpio_intr, NULL, sc,
	    &sc->sc_irq_handle);
	if (error != 0) {
		device_printf(dev, "unable to setup IRQ: error %d\n", error);
		goto fail;
	}

	sc->sc_busdev = gpiobus_add_bus(dev);
	if (sc->sc_busdev == NULL) {
		device_printf(dev, "can't add GPIO bus\n");
		goto fail;
	}

	bus_attach_children(dev);

	device_printf(dev, "Tiger Lake-LP GPIO attached, %d pins\n",
	    TGLGPIO_PIN_MAX);
	return (0);

fail:
	if (sc->sc_irq_handle != NULL)
		bus_teardown_intr(dev, sc->sc_irq_res, sc->sc_irq_handle);
	if (sc->sc_irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid,
		    sc->sc_irq_res);
	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		if (sc->sc_mem_res[ci] != NULL)
			bus_release_resource(dev, SYS_RES_MEMORY,
			    sc->sc_mem_rid[ci], sc->sc_mem_res[ci]);
	}
	TGLGPIO_LOCK_DESTROY(sc);
	return (ENXIO);
}

static int
tglgpio_detach(device_t dev)
{
	struct tglgpio_softc *sc = device_get_softc(dev);
	int ci;

	if (sc->sc_busdev != NULL)
		gpiobus_detach_bus(dev);

	/* Mask all interrupts */
	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		if (sc->sc_mem_res[ci] != NULL)
			tglgpio_mask_community(sc, ci);
	}

	if (sc->sc_irq_handle != NULL)
		bus_teardown_intr(dev, sc->sc_irq_res, sc->sc_irq_handle);
	if (sc->sc_irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, sc->sc_irq_rid,
		    sc->sc_irq_res);
	for (ci = 0; ci < sc->sc_ncommunities; ci++) {
		if (sc->sc_mem_res[ci] != NULL)
			bus_release_resource(dev, SYS_RES_MEMORY,
			    sc->sc_mem_rid[ci], sc->sc_mem_res[ci]);
	}

	TGLGPIO_LOCK_DESTROY(sc);
	return (0);
}

static device_method_t tglgpio_methods[] = {
	/* Device interface */
	DEVMETHOD(device_probe,		tglgpio_probe),
	DEVMETHOD(device_attach,	tglgpio_attach),
	DEVMETHOD(device_detach,	tglgpio_detach),

	/* GPIO interface */
	DEVMETHOD(gpio_get_bus,		tglgpio_get_bus),
	DEVMETHOD(gpio_pin_max,		tglgpio_pin_max),
	DEVMETHOD(gpio_pin_getname,	tglgpio_pin_getname),
	DEVMETHOD(gpio_pin_getflags,	tglgpio_pin_getflags),
	DEVMETHOD(gpio_pin_getcaps,	tglgpio_pin_getcaps),
	DEVMETHOD(gpio_pin_setflags,	tglgpio_pin_setflags),
	DEVMETHOD(gpio_pin_get,		tglgpio_pin_get),
	DEVMETHOD(gpio_pin_set,		tglgpio_pin_set),
	DEVMETHOD(gpio_pin_toggle,	tglgpio_pin_toggle),

	DEVMETHOD_END
};

static driver_t tglgpio_driver = {
	"gpio",
	tglgpio_methods,
	sizeof(struct tglgpio_softc)
};

DRIVER_MODULE(tglgpio, acpi, tglgpio_driver, NULL, NULL);
MODULE_DEPEND(tglgpio, acpi, 1, 1, 1);
MODULE_DEPEND(tglgpio, gpiobus, 1, 1, 1);
MODULE_VERSION(tglgpio, 1);
