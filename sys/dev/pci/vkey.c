#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/device.h>
#include <sys/atomic.h>

#include <machine/bus.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

static int	vkey_match(struct device *, void *, void *);
static void	vkey_attach(struct device *, struct device *, void *);

struct vkey_softc {
	struct device	 sc_dev;
	struct {
		bus_space_tag_t tag;
		bus_space_handle_t handle;
	} sc_bus[2];

	struct vkey_bar0 *sc_bar0;
};

struct vkey_bar0 {
	uint32_t vmin;
	uint32_t vmaj;

	uint32_t _reserved0;
	uint32_t flags;

	uint64_t cbase;
	uint32_t _reserved1;
	uint32_t cshift;

	uint64_t rbase;
	uint32_t _reserved2;
	uint32_t rshift;

	uint64_t cpbase;
	uint32_t _reserved3;
	uint32_t cpshift;

	uint32_t cpdbell;
	uint32_t dbell;
};

const struct cfattach vkey_ca = {
	.ca_devsize 	= sizeof(struct vkey_softc),
	.ca_match 	= vkey_match,
	.ca_attach 	= vkey_attach
};

struct cfdriver vkey_cd = {
	.cd_devs 	= NULL,
	.cd_name	= "vkey",
	.cd_class	= DV_DULL
};

static int
vkey_match(struct device *parent, void *match, void *aux)
{
	struct pci_attach_args *pa = aux;
	if (PCI_VENDOR(pa->pa_id) == 0x3301 &&
	    PCI_PRODUCT(pa->pa_id) == 0x0200)
		return (1);
	return (0);
}

static void
vkey_attach(struct device *parent, struct device *self, void *aux)
{
	struct vkey_softc *sc = (struct vkey_softc *)self;
	struct pci_attach_args *pa = aux;
	printf(": attaching vkey device: bus=%d, device=%d, function=%d\n",
			pa->pa_bus, pa->pa_device, pa->pa_function);

	size_t size0, size1;
	int result;
	pcireg_t reg0 = pci_mapreg_type(pa->pa_pc, pa->pa_tag, 0x10);
	pcireg_t reg1 = pci_mapreg_type(pa->pa_pc, pa->pa_tag, 0x18);

	result = pci_mapreg_map(pa, 0x10, reg0, BUS_SPACE_MAP_LINEAR,
			&sc->sc_bus[0].tag, &sc->sc_bus[0].handle, NULL, &size0, 0);
	printf(": map0 returned %d, size=%lu\n", result, size0);
	if (result) goto fail;

	result = pci_mapreg_map(pa, 0x18, reg1, BUS_SPACE_MAP_LINEAR,
			&sc->sc_bus[1].tag, &sc->sc_bus[1].handle, NULL, &size1, 0);
	printf(": map1 returned %d, size=%lu\n", result, size1);
	if (result) goto fail;

	CTASSERT(sizeof(struct vkey_bar0) <= 0x80);
	if (size0 != 0x80) goto fail;

	sc->sc_bar0 = bus_space_vaddr(sc->sc_bus[0].tag, sc->sc_bus[0].handle);
	if (sc->sc_bar0 == 0) goto fail;

	printf(": device maj=%u, min=%u\n", sc->sc_bar0->vmaj, sc->sc_bar0->vmin);
	if (sc->sc_bar0->vmaj != 1 || sc->sc_bar0->vmin < 0) goto fail;

	printf(": vkey_attach success\n");
	return;

fail:
	printf(": vkey_attach failing :(\n");
}
