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

static int	p4d_match(struct device *, void *, void *);
static void	p4d_attach(struct device *, struct device *, void *);

struct p4d_softc {
	struct device	 sc_dev;
	bus_space_tag_t tag;
	bus_space_handle_t handle;
};

const struct cfattach p4d_ca = {
	.ca_devsize 	= sizeof(struct p4d_softc),
	.ca_match 	= p4d_match,
	.ca_attach 	= p4d_attach
};

struct cfdriver p4d_cd = {
	.cd_devs 	= NULL,
	.cd_name	= "p4d",
	.cd_class	= DV_DULL
};

static int
p4d_match(struct device *parent, void *match, void *aux)
{
	struct pci_attach_args *pa = aux;
	if (PCI_VENDOR(pa->pa_id) == 0x3301 &&
	    PCI_PRODUCT(pa->pa_id) == 0x0001)
		return (1);
	return (0);
}

struct p4d_bar {
        uint64_t        a;
        uint64_t        b;
        uint64_t        sum;
};

static void
p4d_attach(struct device *parent, struct device *self, void *aux)
{
	printf(": hello world x2 x2\n");
	struct pci_attach_args *paa = aux;
	struct p4d_softc *sc = (struct p4d_softc*)self;
	pcireg_t reg_type = pci_mapreg_type(paa->pa_pc, paa->pa_tag, 0x10);
	bus_size_t size = 0;

	pci_mapreg_map(paa,
			0x10,
			reg_type,
			BUS_SPACE_MAP_LINEAR, 
			&sc->tag, 
			&sc->handle,
			NULL, 
			&size,
			0);
	printf(": size %lx\n", size);
	printf(": hello done :3\n");

	bus_space_write_8(sc->tag, sc->handle, 0x00, 42);
	bus_space_write_8(sc->tag, sc->handle, 0x08, 8);
	unsigned long long v = bus_space_read_8(sc->tag, sc->handle, 0x10);
	printf("p4d read sum = %llu\n", v);

	struct p4d_bar* bar;
	bar = bus_space_vaddr(sc->tag, sc->handle);
	bus_space_barrier(sc->tag, sc->handle, 0, size,
            BUS_SPACE_BARRIER_WRITE);
	bar->a = 42;
        bar->b = 8;
        bus_space_barrier(sc->tag, sc->handle, 0, size,
            BUS_SPACE_BARRIER_WRITE | BUS_SPACE_BARRIER_READ);
        printf("p6d read sum = %llu\n", bar->sum);
}
