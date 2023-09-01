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

static void
p4d_attach(struct device *parent, struct device *self, void *aux)
{
	printf(": hello world\n");
}
