#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/device.h>
#include <sys/atomic.h>
#include <sys/tree.h>

#include <machine/bus.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcivar.h>
#include <dev/pci/pcidevs.h>
#include <dev/vkeyvar.h>

#define log(msg, ...) printf("%s:%d\t" msg "\n", __func__, __LINE__, ##__VA_ARGS__)

#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)
#define ensure(cond, msg, ...) do {\
	if (!(cond)) {\
		log("assertion `%s' failure! " msg, TOSTRING(cond), ##__VA_ARGS__);\
		goto fail;\
	}\
} while (0)

static int	vkey_match(struct device *, void *, void *);
static void	vkey_attach(struct device *, struct device *, void *);

enum vkey_owner {
	DEVICE = 0xAA,
	HOST = 0x55,
};

struct vkey_flags {
	bool fltb : 1; // page fault of address from BAR
	bool fltr : 1; // page fault of address from ring
	bool drop : 1; // dropped due to insufficient reply buffers
	bool ovf : 1; // failed to write completion, owner or CPDBELL mismatch
	bool seq : 1; // operation out of sequence
	unsigned _reserved0 : 11;

	bool hwerr : 1; // misc hardware error
	unsigned _reserved : 14;
	bool rst : 1; // writable reset trigger
};

CTASSERT(sizeof(struct vkey_flags) == sizeof(uint32_t));

struct vkey_bar {
	uint32_t vmaj;
	uint32_t vmin;

	struct vkey_flags flags;
	uint32_t _reserved0;

	uint64_t cbase;
	uint32_t cshift;
	uint32_t _reserved1;

	uint64_t rbase;
	uint32_t rshift;
	uint32_t _reserved2;

	uint64_t cpbase;
	uint32_t cpshift;
	uint32_t _reserved3;

	uint32_t dbell;
	uint32_t cpdbell;
};

struct vkey_cmd {
	uint8_t owner;
	uint8_t type;
	uint16_t _reserved1;
	uint32_t _reserved0;

	uint32_t len1;
	uint32_t len2;
	uint32_t len3;
	uint32_t len4;

	uint64_t cookie;

	void* ptr1;
	void* ptr2;
	void* ptr3;
	void* ptr4;
};


CTASSERT(sizeof(struct vkey_cmd) == 8 * sizeof(uint64_t)); 

struct vkey_comp {
	uint8_t owner;
	uint8_t type;
	uint16_t _reserved1;
	uint32_t _reserved0;

	uint32_t msglen;
	uint32_t _reserved2;

	uint64_t cmd_cookie;
	uint64_t reply_cookie;
};

CTASSERT(sizeof(struct vkey_comp) == 4 * sizeof(uint64_t));


struct vkey_cookie {
	uint64_t cookie; // random cookie value
	uint64_t time;   // creation time of this cookie

	RB_ENTRY(vkey_cookie) cmd_entry;
	RB_ENTRY(vkey_cookie) reply_entry;
};

static int vkey_cookie_cmp(struct vkey_cookie *left, struct vkey_cookie *right)
{
	return ((int64_t)left->cookie) - ((int64_t)right->cookie);
};

static uint64_t
vkey_time()
{
	struct timeval tv;
	getmicrouptime(&tv);
	return tv.tv_sec;
}

RB_HEAD(cmdtree, vkey_cookie);
RB_HEAD(replytree, vkey_cookie);

RB_PROTOTYPE(cmdtree, vkey_cookie, cmd_entry, vkey_cookie_cmp);
RB_PROTOTYPE(replytree, vkey_cookie, cmd_entry, vkey_cookie_cmp);
RB_GENERATE(cmdtree, vkey_cookie, reply_entry, vkey_cookie_cmp);
RB_GENERATE(replytree, vkey_cookie, reply_entry, vkey_cookie_cmp);

struct vkey_dma {
	unsigned count;
	size_t esize;
	size_t size;

	enum vkey_owner *ownerlist; // in attach

	bus_dmamap_t map; // in attach
	bus_dma_segment_t seg; // in open
	union {
		struct vkey_cmd *cmds;
		struct vkey_cmd *replies;
		struct vkey_comp *comps;
		char *addr;
	} ptr; // in open
};

struct vkey_softc {
	struct device	 sc_dev;
	bool 		 sc_attached;
	struct {
		bus_space_tag_t tag;
		bus_space_handle_t handle;
	} sc_bus[2];

	struct vkey_bar *sc_bar;
	// struct vkey_flags *sc_flags;

	struct mutex sc_mtx;

	struct cmdtree sc_commands;
	struct replytree sc_replies;

	volatile unsigned long sc_cookiegen;
	volatile unsigned int sc_npending;

	bus_dma_tag_t sc_dmat;
	struct {
		struct vkey_dma cmd;
		struct vkey_dma reply;
		struct vkey_dma comp;
	} sc_dma;
};

// real things below here.

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
	    PCI_PRODUCT(pa->pa_id) == 0x200)
		return (1);
	return (0);
}

bool
vkey_check(struct vkey_softc *sc) {
	struct vkey_flags *errs = &sc->sc_bar->flags;
	ensure(!errs->fltb, "fault reading from bar");
	ensure(!errs->fltr, "fault reading from ring");
	ensure(!errs->drop, "insufficient reply buffer");
	ensure(!errs->ovf, "owner mismatch or cpdbell wrong");
	ensure(!errs->seq, "sequencing error");
	ensure(!errs->hwerr, "misc hardware error");
	return true;
fail:
	sc->sc_attached = false;
	return false;
}

const unsigned shift = 6;
const unsigned count = 1 << shift;

bool
vkey_ring_init(struct vkey_softc *sc, const char *name, struct vkey_dma *dma, size_t size, enum vkey_owner owner) {
	int error;

	dma->count = count;
	dma->esize = size;
	dma->size = count * size;
	dma->ownerlist = malloc(count * sizeof(enum vkey_owner), M_DEVBUF, M_NOWAIT);
	ensure(dma->ownerlist, "malloc");

	for (unsigned i = 0; i < count; i++)
		dma->ownerlist[i] = owner;

	error = bus_dmamap_create(sc->sc_dmat, dma->size,
			1, dma->size, 0, 
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT,
			&dma->map);
	ensure(!error, "dmamap %s", name);

	int nsegs;
	ensure(dma->ptr.addr == NULL, "double assignment");
	error = bus_dmamem_alloc(sc->sc_dmat, dma->size, 0, 0, &dma->seg, 1, &nsegs, BUS_DMA_WAITOK | BUS_DMA_ZERO);
	ensure(!error, "dmamem alloc");
	ensure(nsegs == 1, "dmamem alloc");

	error = bus_dmamem_map(sc->sc_dmat, &dma->seg, 1, dma->size, &dma->ptr.addr, BUS_DMA_WAITOK);
	ensure(!error && dma->ptr.addr, "dmamem map");

	error = bus_dmamap_load(sc->sc_dmat, dma->map, dma->ptr.addr, dma->size, NULL, BUS_DMA_WAITOK);
	ensure(!error, "dmamap load");

	log("ring allocated for %s of %zu size at %p", name, dma->size, dma->ptr.addr);

	return true;
fail:
	return false;
}

bool
vkey_rings(struct vkey_softc *sc) {
	ensure(vkey_ring_init(sc, "cmd", &sc->sc_dma.cmd, sizeof(struct vkey_cmd), HOST), "cmd");
	ensure(vkey_ring_init(sc, "reply", &sc->sc_dma.reply, sizeof(struct vkey_cmd), HOST), "reply");
	ensure(vkey_ring_init(sc, "comp", &sc->sc_dma.comp, sizeof(struct vkey_comp), DEVICE), "comp");

	for (unsigned i = 0; i < sc->sc_dma.cmd.count; i++) {
		sc->sc_dma.cmd.ptr.cmds[i].owner = HOST;
		sc->sc_dma.reply.ptr.replies[i].owner = HOST;
		sc->sc_dma.comp.ptr.comps[i].owner = DEVICE;
	}

	return true;
fail:
	return false;
}

void
vkey_bar_barrier(struct vkey_softc *sc, int barriers) {
	bus_space_barrier(sc->sc_bus[0].tag, sc->sc_bus[0].handle, 0,
	    sizeof(struct vkey_bar), barriers);
}


static void
vkey_attach(struct device *parent, struct device *self, void *aux)
{
	struct vkey_softc *sc = (struct vkey_softc *)self;
	sc->sc_attached = false;
	memset(&sc->sc_dma, 0, sizeof(sc->sc_dma));

	struct pci_attach_args *pa = aux;
	printf(": attaching vkey device: bus=%d, device=%d, function=%d\n",
			pa->pa_bus, pa->pa_device, pa->pa_function);

	mtx_init(&sc->sc_mtx, IPL_BIO);
	RB_INIT(&sc->sc_commands);
	RB_INIT(&sc->sc_replies);
	sc->sc_cookiegen = 1000;

	size_t size0, size1;
	int error;
	pcireg_t reg0 = pci_mapreg_type(pa->pa_pc, pa->pa_tag, 0x10);
	pcireg_t reg1 = pci_mapreg_type(pa->pa_pc, pa->pa_tag, 0x18);
	(void)size1;
	(void)reg1;

	error = pci_mapreg_map(pa, 0x10, reg0, BUS_SPACE_MAP_LINEAR,
			&sc->sc_bus[0].tag, &sc->sc_bus[0].handle, NULL, &size0, 0);
	printf(": map0 returned %d, size=%lu\n", error, size0);
	ensure(!error, "mapreg");

	// error = pci_mapreg_map(pa, 0x18, reg1, BUS_SPACE_MAP_LINEAR,
	// 		&sc->sc_bus[1].tag, &sc->sc_bus[1].handle, NULL, &size1, 0);
	// printf(": map1 returned %d, size=%lu\n", error, size1);
	// ensure(!error, "mapreg flags");

	CTASSERT(sizeof(struct vkey_bar) <= 0x80);
	ensure(size0 == 0x80, "size");

	sc->sc_bar = bus_space_vaddr(sc->sc_bus[0].tag, sc->sc_bus[0].handle);
	ensure(sc->sc_bar != NULL, "vaddr sc_bar");

	// sc->sc_flags = bus_space_vaddr(sc->sc_bus[1].tag, sc->sc_bus[1].handle);
	// ensure(sc->sc_flags != NULL, "vaddr sc_flags");

	printf(": device maj=%u, min=%u\n", sc->sc_bar->vmaj, sc->sc_bar->vmin);
	ensure(sc->sc_bar->vmaj == 1 && sc->sc_bar->vmin >= 0, "version");

	sc->sc_dmat = pa->pa_dmat;
	// XXX allocate rings
	ensure(vkey_rings(sc), "rings");

	sc->sc_bar->cbase = (uint64_t)sc->sc_dma.cmd.ptr.addr;
	sc->sc_bar->cshift = shift;
	sc->sc_bar->rbase = (uint64_t)sc->sc_dma.reply.ptr.addr;
	sc->sc_bar->rshift = shift;
	sc->sc_bar->cpbase = (uint64_t)sc->sc_dma.comp.ptr.addr;
	sc->sc_bar->cpshift = shift;
	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

	ensure(vkey_check(sc), "initial check");
	printf(": vkey_attach success\n");

	sc->sc_attached = true;
	return;

fail:
	printf(": vkey_attach failing :(\n");
}

struct vkey_softc *
vkey_lookup(dev_t dev) {
	printf("vkey %d lookup, %d, %d\n", dev, major(dev), minor(dev));
	ensure(major(dev) == 0, "major");

	struct vkey_softc *sc = (void *)device_lookup(&vkey_cd, minor(dev));
	ensure(sc && sc->sc_attached, "sc %p", sc);

	return sc;
fail:
	return NULL;
}

int
vkeyopen(dev_t dev, int mode, int flags, struct proc *p)
{
	struct vkey_softc *sc = vkey_lookup(dev);
	ensure(sc, "open");

	return (0);
fail:
	return ENXIO;
}

int
vkeyclose(dev_t dev, int flag, int mode, struct proc *p)
{
	printf("vkey %d close\n", dev);
	struct vkey_softc *sc = vkey_lookup(dev);
	ensure(sc != NULL, "close");

	// XXX block for pending operations.

	return (0);
fail:
	return 0; // "must not return error."
}

int
vkeywrite(dev_t dev, struct uio *uio, int flags)
{
	return (EOPNOTSUPP);
}

int
vkeyread(dev_t dev, struct uio *uio, int flags)
{
	return (EOPNOTSUPP);
}

void
vkeyioctl_cmd_new(struct vkey_softc *sc, uint64_t cook, const struct vkey_cmd_arg *cmd)
{
	struct vkey_cmd *desc = NULL; // XXX find next empty desc

	struct vkey_cookie *cookie = malloc(sizeof(struct vkey_cookie), 0, M_NOWAIT | M_ZERO);
	assert(cookie != NULL);

	cookie->cookie = cook;
	cookie->time = vkey_time();

	mtx_enter(&sc->sc_mtx);
	RB_INSERT(cmdtree, &sc->sc_commands, cookie);
	mtx_leave(&sc->sc_mtx);

	desc->cookie = cook;

	desc->ptr1 = cmd->vkey_in[0].iov_base;
	desc->ptr2 = cmd->vkey_in[1].iov_base;
	desc->ptr3 = cmd->vkey_in[2].iov_base;
	desc->ptr4 = cmd->vkey_in[3].iov_base;

	desc->len1 = cmd->vkey_in[0].iov_len;
	desc->len2 = cmd->vkey_in[1].iov_len;
	desc->len3 = cmd->vkey_in[2].iov_len;
	desc->len4 = cmd->vkey_in[3].iov_len;

	// XXX STORE BARRIER 

	// desc->owner = DEVICE_OWNER;

	// XXX STORE BARRIER
	
	sc->sc_bar->dbell = 0x5; // XXX index
}

int
vkeyioctl_cmd(struct vkey_softc *sc, struct vkey_cmd_arg *cmd)
{
	struct vkey_cmd cmd_desc;
	uint64_t cookie = atomic_inc_long_nv(&sc->sc_cookiegen);

	vkeyioctl_cmd_new(sc, cookie, cmd);

	(void)cmd_desc;

	// write to command ring
	// wait for notification from completion interrupts
	// read from reply ring
	return 0;
}

int
vkeyioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	printf("vkey %d ioctl\n", dev);
	struct vkey_info_arg *vi;

	struct vkey_softc *sc = (void *)device_lookup(&vkey_cd, minor(dev));
	if (sc == NULL)
		return ENXIO;

	switch (cmd) {
	case VKEYIOC_GET_INFO:
		vi = (void *)data;
		vi->vkey_major = sc->sc_bar->vmaj;
		vi->vkey_major = sc->sc_bar->vmin;
		return 0;
	case VKEYIOC_CMD:
		printf("vkey cmd unhandled\n");
		return vkeyioctl_cmd(sc, (void *)data);
	}
	assert(0 && "vkeyioctl unhandled");
}

void
vkey_intr(void *arg)
{
	// on receive completion
	//  

}
