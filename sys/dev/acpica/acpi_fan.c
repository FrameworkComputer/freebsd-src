/**
 * ACPI Fan implemented according to
 * https://uefi.org/htmlspecs/ACPI_Spec_6_4_html/11_Thermal_Management/fan-device.html
 *
 * _FIF - Fan Information
 * _FPS - Fan Performance States
 * _FSL - Fan Speed Level
 * _FST - Fan Status
 */

#include "opt_acpi.h"
#include <sys/types.h>
#include <sys/bus.h>
#include <sys/cdefs.h>
#include <sys/cpu.h>
#include <sys/eventhandler.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/param.h>
#include <sys/power.h>
#include <sys/proc.h>
#include <sys/reboot.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/unistd.h>

#include <contrib/dev/acpica/include/acpi.h>
#include <contrib/dev/acpica/include/accommon.h>

#include <dev/acpica/acpivar.h>
#include <dev/acpica/acpiio.h>

#define _COMPONENT ACPI_BUTTON
ACPI_MODULE_NAME("FAN")

struct acpi_fan_softc {
    device_t	fan_dev;
    ACPI_HANDLE	fan_handle;
    ACPI_BUFFER fst;
    int		fan_speed;
};

ACPI_HANDLE acpi_fan_handle;

ACPI_SERIAL_DECL(fan, "ACPI Fan");

static int	acpi_fan_probe(device_t dev);
static int	acpi_fan_attach(device_t dev);
static int	acpi_fan_suspend(device_t dev);
static int	acpi_fan_resume(device_t dev);

static device_method_t acpi_fan_methods[] = {
    /* Device interface */
    DEVMETHOD(device_probe,	acpi_fan_probe),
    DEVMETHOD(device_attach,	acpi_fan_attach),
    DEVMETHOD(device_suspend,	acpi_fan_suspend),
    DEVMETHOD(device_resume,	acpi_fan_resume),

    DEVMETHOD_END
};

static driver_t acpi_fan_driver = {
    "acpi_fan_",
    acpi_fan_methods,
    sizeof(struct acpi_fan_softc),
};

static int
acpi_fan_speed_update(struct acpi_fan_softc *sc)
{
	// ACPI_STATUS status;
	int fan_speed;
	ACPI_BUFFER fst;
	ACPI_OBJECT *obj;

	ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    	/* Erase any existing state. */
	if (sc->fst.Pointer != NULL)
	    AcpiOsFree(sc->fst.Pointer);
	/* Initialize */
	// TODO
    	//bzero(sc->fst, sizeof(sc->fst));

	sc->fst.Length = ACPI_ALLOCATE_BUFFER;
	sc->fst.Pointer = NULL;
	AcpiEvaluateObject(sc->fan_handle, "_FST", NULL, &fst);
	obj = (ACPI_OBJECT *)sc->fst.Pointer;
	if (obj != NULL) {
	    /* Should be a package containing details about fan status */
	    if (obj->Type != ACPI_TYPE_PACKAGE) {
		// TODO: invalid print
		// device_printf(sc->fst, "%s has unknown type %d, rejecting\n",
		// 	      "_FSL", obj->Type);
		return_VALUE (ENXIO);
	    }
	}


	// Revision == 0
	// Control: Integer
	// Speed: Integer
	// obj->Package.Elements[2];
	if (obj->Package.Elements[2].Type != ACPI_TYPE_INTEGER) {
		// TODO: Invalid print
		// device_printf(sc->fst,
                //     "Speed object is not an integer : %i type\n",
                //      obj->Type);
		return (ENXIO);
	}
	fan_speed = obj->Package.Elements[0].Integer.Value;

	if (sc->fan_speed == fan_speed)
		return (EALREADY);
	sc->fan_speed = fan_speed;

	return (0);
}

static int
acpi_fan_probe(device_t dev)
{
    static char *fan_ids[] = { "PNP0C0B", NULL };
    int rv;

    if (acpi_disabled("fan"))
	return (ENXIO);
    rv = ACPI_ID_PROBE(device_get_parent(dev), dev, fan_ids, NULL);
    if (rv <= 0)
	device_set_desc(dev, "Control Method Fan Device");
    return (rv);
}

static int
sysctl_handle_int_range(SYSCTL_HANDLER_ARGS)
{
	struct acpi_fan_softc	*sc;
	
	sc = (struct acpi_fan_softc *)arg1;
	acpi_fan_speed_update(sc);
	
	return sysctl_handle_int(oidp, &sc->fan_speed, 0, req);
}

static int
acpi_fan_attach(device_t dev)
{
    struct acpi_fan_softc	*sc;

    ACPI_FUNCTION_TRACE((char *)(uintptr_t)__func__);

    sc = device_get_softc(dev);
    sc->fan_dev = dev;
    acpi_fan_handle = sc->fan_handle = acpi_get_handle(dev);
    sc->fan_speed = -1;

    /* Get the initial fan status */
    acpi_fan_speed_update(sc);

    /*
     * Export the fan speed
     */
    SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO,
	"speed", CTLFLAG_RD | CTLTYPE_INT, sc, 0,
	sysctl_handle_int_range, "I",
	"Current speed of the fan (FPS)");

    return (0);
}

static int
acpi_fan_suspend(device_t dev)
{
    return (0);
}

static int
acpi_fan_resume(device_t dev)
{
    struct acpi_fan_softc	*sc;

    sc = device_get_softc(dev);

    /* Update fan status, if any */
    acpi_fan_speed_update(sc);

    return (0);
}

DRIVER_MODULE(acpi_fan, acpi, acpi_fan_driver, 0, 0);
MODULE_DEPEND(acpi_fan, acpi, 1, 1, 1);
