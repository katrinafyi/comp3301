#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/fcntl.h>
#include <sys/device.h>
#include <sys/vnode.h>
#include <sys/atomic.h>

#include <machine/bus.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>

#include <dev/pci/p6statsvar.h>

struct p6stats_bar {
	uint64_t ibase;
	uint64_t icount;
	uint64_t obase;
	uint64_t dbell;
};

struct p6stats_softc {
	struct device		sc_dev;
	pci_chipset_tag_t	 sc_pc;

	bool sc_attached;

	struct {
		bus_space_tag_t tag;
		bus_space_handle_t handle;
	} sc_bus;
	struct p6stats_bar 	*sc_bar;

	bus_dmamap_t sc_in;
	bus_dmamap_t sc_out;

	bus_dma_tag_t sc_dma;
	struct mutex sc_mtx;
	enum {
		IDLE,
		WAIT,
		COMPLETE
	} sc_state;

	pci_intr_handle_t sc_intr;
	void* sc_intrp;

};

static int	p6stats_match(struct device *, void *, void *);
static void	p6stats_attach(struct device *, struct device *, void *);

const struct cfattach p6stats_ca = {
	.ca_devsize	= sizeof(struct p6stats_softc),
	.ca_match	= p6stats_match,
	.ca_attach	= p6stats_attach
};

struct cfdriver p6stats_cd = {
	.cd_devs	= NULL,
	.cd_name	= "p6stats",
	.cd_class	= DV_DULL
};

const struct pci_matchid p6stats_devices[] = {
	{ 0x3301, 0x0002 }
};

static int
p6stats_match(struct device *parent, void *match, void *aux)
{
        struct pci_attach_args *pa = aux;
        return (pci_matchbyid(pa, p6stats_devices, 1));
}

static int	p6stats_intr(void *);

static void
p6stats_attach(struct device *parent, struct device *self, void *aux)
{
	int result;
	size_t size;
        struct p6stats_softc *sc = (struct p6stats_softc *)self;
	sc->sc_attached = false;
        mtx_init(&sc->sc_mtx, IPL_BIO);

	struct pci_attach_args *pa = aux;
        sc->sc_pc = pa->pa_pc;
	printf(": attaching vkey device: bus=%d, device=%d, function=%d\n",
		pa->pa_bus, pa->pa_device, pa->pa_function);

	pcireg_t reg0 = pci_mapreg_type(pa->pa_pc, pa->pa_tag, PCI_MAPREG_START);

	result = pci_mapreg_map(pa, PCI_MAPREG_START, reg0, BUS_SPACE_MAP_LINEAR,
		&sc->sc_bus.tag, &sc->sc_bus.handle, NULL, &size, 0);
	if (result) {
		printf("map failed: %d\n", result);
		return;
	};

	printf("mapped size=%zu", size);

	sc->sc_bar = bus_space_vaddr(sc->sc_bus.tag, sc->sc_bus.handle);
	if (sc->sc_bar == 0) return;
	printf("bus_space_vaddr\n");

	sc->sc_dma = pa->pa_dmat;
	result = bus_dmamap_create(pa->pa_dmat, sizeof(uint64_t) * 100, 1, sizeof(uint64_t) * 100, 0, BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT,
			&sc->sc_in);
	if (result) return;
	printf("bus_dmamap_create\n");

	if (pci_intr_map_msix(pa, 0, &sc->sc_intr) != 0) {
                printf(": unable to map interrupt\n");
                return;
        }
        sc->sc_intrp = pci_intr_establish(sc->sc_pc, sc->sc_intr,
	    IPL_BIO, p6stats_intr, sc, sc->sc_dev.dv_xname);
	if (sc->sc_intrp == NULL) {
		printf(": unable to establish msix interrupt 0\n");
		return;
        }

	sc->sc_attached = true;
}

static struct p6stats_softc *
p6stats_lookup(dev_t dev)
{
	/* the device minor is 1:1 with the driver unit number */ 
	dev_t unit = minor(dev);
	struct p6stats_softc *sc;

	if (unit >= p6stats_cd.cd_ndevs)
		return (NULL);

	/* this will be NULL if there's no device */
	sc = p6stats_cd.cd_devs[unit];

	// CHECK FOR SUCCESSFUL ATTACH
	if (!sc || !sc->sc_attached)
		return NULL;

	printf("found p6 device: %p\n", sc);

	return (sc);
}

int
p6statsopen(dev_t dev, int mode, int flags, struct proc *p)
{
	struct p6stats_softc *sc = p6stats_lookup(dev);

	if (sc == NULL)
		return (ENXIO);

	return (0);
}

int
p6statsclose(dev_t dev, int flag, int mode, struct proc *p)
{
	/* replace with your code */
	return (0);
}

int
p6statsioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	struct p6stats_softc *sc = p6stats_lookup(dev);
	assert(sc);

	if (cmd == P6STATS_IOC_CALC) {
		int err;
		struct p6stats_calc *x = (void *)data;
		struct uio in, out;
		struct iovec inv, outv;

		inv.iov_base = x->pc_inputs;
		inv.iov_len = x->pc_ninputs * sizeof(x->pc_inputs[0]);

		in.uio_offset = 0;
		in.uio_iov = &inv;
		in.uio_iovcnt = 1;
		in.uio_resid = inv.iov_len;
		in.uio_rw = UIO_WRITE;
		in.uio_segflg = UIO_USERSPACE;
		in.uio_procp = p;

		outv.iov_base = x->pc_output;
		outv.iov_len = sizeof(x->pc_output[0]);

		out.uio_offset = 0;
		out.uio_iov = &outv;
		out.uio_iovcnt = 1;
		out.uio_resid = outv.iov_len;
		out.uio_rw = UIO_READ;
		out.uio_segflg = UIO_USERSPACE;
		out.uio_procp = p;
		
		// claim mutex before beginning dma procedure
		mtx_enter(&sc->sc_mtx);
		do {
			err = msleep_nsec(&sc->sc_state, &sc->sc_mtx, PRIBIO|PCATCH, "p6wait", INFSLP);
			if (err) {
				mtx_leave(&sc->sc_mtx);
				return EIO;
			}
		} while (sc->sc_state != IDLE);

		// device is idle now

		bus_dmamap_load_uio(sc->sc_dma, sc->sc_in, &in, BUS_DMA_WRITE);
		bus_dmamap_load_uio(sc->sc_dma, sc->sc_out, &out, BUS_DMA_READ);

		bus_dmamap_sync(sc->sc_dma, sc->sc_in, 0, sc->sc_in->dm_segs[0].ds_len, BUS_DMASYNC_PREWRITE);
		bus_dmamap_sync(sc->sc_dma, sc->sc_out, 0, sc->sc_out->dm_segs[0].ds_len, BUS_DMASYNC_PREREAD);

		sc->sc_bar->ibase = sc->sc_in->dm_segs[0].ds_addr;
		sc->sc_bar->icount = x->pc_ninputs;
		sc->sc_bar->obase = sc->sc_out->dm_segs[0].ds_addr;
		bus_space_barrier(sc->sc_bus.tag, sc->sc_bus.handle, 0, sizeof(sc->sc_bar[0]), BUS_SPACE_BARRIER_WRITE);

		sc->sc_bar->dbell = 1;
		bus_space_barrier(sc->sc_bus.tag, sc->sc_bus.handle, 0, sizeof(sc->sc_bar[0]), BUS_SPACE_BARRIER_WRITE);


		// after writing doorbell, WAIT FOR REPLY LOL
		sc->sc_state = WAIT;

		// sleep until COMPLETED
		do {
			msleep_nsec(&sc->sc_state, &sc->sc_mtx, PRIBIO, "p6wait", INFSLP);
		} while (sc->sc_state != COMPLETE);
		
		bus_dmamap_sync(sc->sc_dma, sc->sc_out,
	    	    0, sc->sc_out->dm_segs[0].ds_len,
	    	    BUS_DMASYNC_POSTREAD);

		bus_dmamap_sync(sc->sc_dma, sc->sc_in,
	    	    0, sc->sc_in->dm_segs[0].ds_len,
	    	    BUS_DMASYNC_POSTWRITE);

		bus_dmamap_unload(sc->sc_dma, sc->sc_out);
		bus_dmamap_unload(sc->sc_dma, sc->sc_in);

		sc->sc_state = IDLE;
		wakeup(&sc->sc_state); /* let something else have a go */

		// bus_dmamap_unload(sc->sc_dma, sc->sc_in);
		mtx_leave(&sc->sc_mtx);

	}
	return (ENXIO);
}


static int
p6stats_intr(void *arg)
{
	struct p6stats_softc *sc = arg;

	mtx_enter(&sc->sc_mtx);
	if (sc->sc_state == WAIT) {
		sc->sc_state = COMPLETE;
		wakeup(&sc->sc_state);
	}
	mtx_leave(&sc->sc_mtx);

	return (1);
}

