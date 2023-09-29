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

#define NITEMS(x) (sizeof(x) / sizeof(*(x)))

#define log(msg, ...) printf("%s:%d\t" msg "\n", __func__, __LINE__, ##__VA_ARGS__)

#define _STR(x) #x
#define STR(x) _STR(x)

#define ensure2(flag, cond, msg, ...) \
do {\
	*(&(flag)) = (cond);\
	if (!(flag)) {\
		log(msg ": assertion `%s' failed! ", ##__VA_ARGS__, STR(cond));\
		goto fail;\
	}\
} while (0)

#define _CONCAT(x, y) x ## y
#define CONCAT(x, y) _CONCAT(x, y)

#define ensure(cond, msg, ...)\
do {\
	bool CONCAT(ensure_flag, __LINE__);\
	ensure2(CONCAT(ensure_flag, __LINE__), cond, msg, ##__VA_ARGS__);\
} while (0)

static int	vkey_match(struct device *, void *, void *);
static void	vkey_attach(struct device *, struct device *, void *);
static int     vkey_intr(void *arg);

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

enum vkey_ring {
	CMD = 0,
	REPLY = 1,
	COMP = 2,
};

struct vkey_dma {
	unsigned shift;
	unsigned count;
	size_t esize;

	size_t head; // index of next insertion

	// bool *freelist; // in attach

	bus_dmamap_t map; // in attach
	bus_dma_segment_t seg[1]; // in attach
	union {
		volatile struct vkey_cmd *cmds;
		volatile struct vkey_cmd *replies;
		volatile struct vkey_comp *comps;
		char *addr;
	} ptr; // in attach
};

struct vkey_cookie {
	// these two fields are map keys:
	enum vkey_ring type; // 
	uint64_t cookie; // random cookie value

	size_t i; // index within respective ring
	uint64_t time;   // creation time of this cookie

	// SET BY COMPLETION
	bool done; // command only: done wakeup flag.
	uint64_t reply; // command only: cookie of corresponding reply.
	size_t replylen; // command only: total size of reply (possibly exceeding buffer size)
	uint8_t replytype; // command only: type of reply message

	// bus_dma_segment_t segs[4]; // reply only: only ptr and map accessed.
	bus_dmamap_t map; // reply only: buffer for reply.
	bus_dma_segment_t segs[4]; // reply only: segment
	size_t nsegs;
	size_t size; // reply only: size of reply buffer
	RB_ENTRY(vkey_cookie) link;
};

int
vkey_cookie_cmp(struct vkey_cookie *left, struct vkey_cookie *right)
{
	if (left->type != right->type)
		return left->type - right->type;
	return ((int64_t)left->cookie) - ((int64_t)right->cookie);
};

uint64_t
vkey_time()
{
	struct timeval tv;
	getmicrouptime(&tv);
	return tv.tv_sec;
}

RB_HEAD(cookies, vkey_cookie);

RB_PROTOTYPE(cookies, vkey_cookie, link, vkey_cookie_cmp);
RB_GENERATE(cookies, vkey_cookie, link, vkey_cookie_cmp);


const uint64_t reply_cookie = 10000000000000000000U; // 10^19
const uint32_t reply_mask = 1 << 31; 

struct vkey_softc {
	struct device	 sc_dev;
	bool 		 sc_attached;
	struct {
		bus_space_tag_t tag;
		bus_space_handle_t handle;
	} sc_bus[2];

	volatile struct vkey_bar *sc_bar;
	// struct vkey_flags *sc_flags;

	struct mutex sc_mtx;

	struct cookies sc_cookies;

	unsigned long sc_cookiegen;
	unsigned int sc_ncmd;  // number of commands in-flight
	unsigned int sc_nreplyfree; // number of reply descriptors allocated

	// nreply used = ncmd 
	// nreply alloced = ncmd + nreply free
	
	bus_dma_tag_t sc_dmat;
	struct {
		struct vkey_dma cmd;
		struct vkey_dma reply;
		struct vkey_dma comp;
	} sc_dma;

	pci_intr_handle_t sc_ih;
	void *sc_ihc;
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


void
vkey_bar_barrier(struct vkey_softc *sc, int barriers) {
	bus_space_barrier(sc->sc_bus[0].tag, sc->sc_bus[0].handle, 0,
	    sizeof(struct vkey_bar), barriers);
}

bool
vkey_check(struct vkey_softc *sc) {
	volatile struct vkey_flags *errs = &sc->sc_bar->flags;

	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE | BUS_SPACE_BARRIER_READ);
	ensure(!errs->fltb,  "DEVICE FAULT: fault reading from bar");
	ensure(!errs->fltr,  "DEVICE FAULT: fault reading from ring");
	ensure(!errs->drop,  "DEVICE FAULT: insufficient reply buffer");
	ensure(!errs->ovf,   "DEVICE FAULT: owner mismatch or cpdbell wrong");
	ensure(!errs->seq,   "DEVICE FAULT: sequencing error");
	ensure(!errs->hwerr, "DEVICE FAULT: misc hardware error");
	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE | BUS_SPACE_BARRIER_READ);
	return true;
fail:
	log("fault! flags: 0x%x", *(int32_t *)errs);
	sc->sc_attached = false;
	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE | BUS_SPACE_BARRIER_READ);
	return false;
}


struct vkey_dma *
vkey_dma(struct vkey_softc *sc, enum vkey_ring ring)
{
	switch (ring) {
		case CMD:
			return &sc->sc_dma.cmd;
		case REPLY:
			return &sc->sc_dma.reply;
		case COMP:
			return &sc->sc_dma.comp;
	}
}

void
vkey_dmamap_sync(struct vkey_softc *sc, enum vkey_ring ring, long index, int syncs)
{
	size_t size;
	struct vkey_dma *dma = vkey_dma(sc, ring);
	if (index < 0) {
		index = 0;
		size = dma->map->dm_mapsize;
	} else {
		size = ring != COMP ? sizeof(struct vkey_cmd) : sizeof(struct vkey_comp);
		index *= size;
	}

	bus_dmamap_sync(sc->sc_dmat, dma->map, index, size, syncs);
}


// const unsigned shift = 2;
// const unsigned count = 1 << shift; // number of descriptors

bool
vkey_ring_init(struct vkey_softc *sc, const char *name, struct vkey_dma *dma, size_t descsize) {
	int error;
	bool created = false, alloced = false, mapped = false, loaded = false;

	bool iscomp = dma == vkey_dma(sc, COMP);
	dma->shift = iscomp ? 2 : 1; // XXX THE PLACE WHERE IT HAPPENS
	dma->count = 1 << dma->shift;
	dma->esize = descsize;
	size_t size = dma->count * descsize;
	// dma->size = count * size;

	ensure(!dma->map, "dmamap double create");
	error = bus_dmamap_create(sc->sc_dmat, size,
			1, size, 0, 
			BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW | BUS_DMA_64BIT,
			&dma->map);
	bus_dmamap_t map = dma->map;
	ensure2(created, map && !error, "dmamap %s", name);

	int nsegs;
	ensure(dma->ptr.addr == NULL, "double assignment");
	error = bus_dmamem_alloc(sc->sc_dmat, size, 0, 0, dma->seg, 1, &nsegs, BUS_DMA_WAITOK | BUS_DMA_ZERO);
	ensure(!error, "dmamem alloc");
	ensure2(alloced, nsegs == 1, "dmamem alloc");

	// XXX IMPORTANT: the dm_seg fields cannot be used here. this requires a separate segment field.
	error = bus_dmamem_map(sc->sc_dmat, dma->seg, 1, size, &dma->ptr.addr, BUS_DMA_WAITOK);
	ensure2(mapped, !error && dma->ptr.addr, "dmamem map");

	// XXX THEN, load will assign a paddr for the DMA and store /this/ inside the dm_seg.
	error = bus_dmamap_load(sc->sc_dmat, map, dma->ptr.addr, size, NULL, BUS_DMA_WAITOK);
	ensure2(loaded, !error, "dmamap load");
	ensure(map->dm_mapsize == size, "mapsize");

	for (unsigned i = 0; i < dma->count; i++) {
		if (iscomp) {
			dma->ptr.comps[i].owner = DEVICE;
		} else {
			dma->ptr.cmds[i].owner = iscomp ? DEVICE : HOST;
		}
	}
	bus_dmamap_sync(sc->sc_dmat, dma->map, 0, size, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	log("ring allocated for %s of size %zu (count=%u) at kaddr %p and paddr %p", name, size, dma->count, dma->ptr.addr, map->dm_segs);

	return true;
fail:
	if (loaded) bus_dmamap_unload(sc->sc_dmat, map);
	if (mapped) bus_dmamem_unmap(sc->sc_dmat, dma->ptr.addr, map->dm_mapsize);
	if (alloced) bus_dmamem_free(sc->sc_dmat, map->dm_segs, map->dm_nsegs);
	if (created) bus_dmamap_destroy(sc->sc_dmat, map);
	return false;
}

bool
vkey_rings(struct vkey_softc *sc) {
	ensure(vkey_ring_init(sc, "cmd", &sc->sc_dma.cmd, sizeof(struct vkey_cmd)), "cmd");
	ensure(vkey_ring_init(sc, "reply", &sc->sc_dma.reply, sizeof(struct vkey_cmd)), "reply");
	ensure(vkey_ring_init(sc, "comp", &sc->sc_dma.comp, sizeof(struct vkey_comp)), "comp");

	for (unsigned i = 0; i < sc->sc_dma.comp.count; i++) {
		sc->sc_dma.comp.ptr.comps[i].owner = DEVICE;
	}

	return true;
fail:
	return false;
}

long
vkey_ring_usable(struct vkey_softc *sc, enum vkey_ring ring)
{
	ensure(ring != COMP, "COMP ring disallowed");
	struct vkey_dma *dma = vkey_dma(sc, ring);

	size_t n = ring == CMD ? sc->sc_ncmd : sc->sc_nreplyfree + sc->sc_ncmd;
	ensure(n < dma->count, "empty desc in ring %d not found, THIS SHOULD BE GUARDED BY ncmd/nreply. too many requests in flight?", ring);

	size_t h = dma->head;
	dma->head++;
	dma->head %= dma->count;
	return h;
fail:
	return -1;
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
	RB_INIT(&sc->sc_cookies);
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
	ensure(vkey_rings(sc), "rings");

	vkey_dmamap_sync(sc, CMD, -1, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	vkey_dmamap_sync(sc, REPLY, -1, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);
	vkey_dmamap_sync(sc, COMP, -1, BUS_DMASYNC_PREREAD | BUS_DMASYNC_PREWRITE);

	sc->sc_bar->cbase = (uint64_t)sc->sc_dma.cmd.map->dm_segs[0].ds_addr;
	sc->sc_bar->cshift = sc->sc_dma.cmd.shift;
	sc->sc_bar->rbase = (uint64_t)sc->sc_dma.reply.map->dm_segs[0].ds_addr;
	sc->sc_bar->rshift = sc->sc_dma.reply.shift;
	sc->sc_bar->cpbase = (uint64_t)sc->sc_dma.comp.map->dm_segs[0].ds_addr;
	sc->sc_bar->cpshift = sc->sc_dma.comp.shift;
	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

	vkey_dmamap_sync(sc, CMD, -1, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	vkey_dmamap_sync(sc, REPLY, -1, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	vkey_dmamap_sync(sc, COMP, -1, BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);


	error = pci_intr_map_msix(pa, 0, &sc->sc_ih);
	ensure(!error, "pci_intr_map");

	sc->sc_ihc = pci_intr_establish(pa->pa_pc, sc->sc_ih, IPL_BIO, vkey_intr, sc, sc->sc_dev.dv_xname);
	ensure(sc->sc_ihc, "intr_establish");

	ensure(vkey_check(sc), "initial check");
	log(": vkey_attach success");

	sc->sc_attached = true;
	return;

fail:
	printf(": vkey_attach failing :(\n");
}

struct vkey_softc *
vkey_lookup(dev_t dev) {
	// printf("vkey %d lookup, %d, %d\n", dev, major(dev), minor(dev));
	ensure(major(dev) == 101, "major");

	struct vkey_softc *sc = (void *)device_lookup(&vkey_cd, minor(dev));
	// ensure(sc && sc->sc_attached, "sc %p", sc);

	return sc && sc->sc_attached ? sc : NULL;
fail:
	return NULL;
}

int
vkeyopen(dev_t dev, int mode, int flags, struct proc *p)
{
	struct vkey_softc *sc = vkey_lookup(dev);
	if (!sc) return ENXIO;

	ensure(vkey_check(sc), "check");

	return (0);
fail:
	return ENXIO;
}

int
vkeyclose(dev_t dev, int flag, int mode, struct proc *p)
{
	log("vkey %d close\n", dev);
	struct vkey_softc *sc = vkey_lookup(dev);
	ensure(sc != NULL, "close");

	mtx_enter(&sc->sc_mtx);
	while (sc->sc_ncmd > 0) {
		// XXX don't forget to wakeup when decrementing ncmd
		ensure(0 == msleep_nsec(&sc->sc_ncmd, &sc->sc_mtx, PCATCH | PRIBIO, "vkeyclose sc_ncmd", INFSLP),
				"awoken");
	}
	mtx_leave(&sc->sc_mtx);

	ensure(vkey_check(sc), "check");

	return (0);
fail:
	log("error during vkey close. squashing...");
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

const size_t defaultreplysize = 16 * 1024 * sizeof(char);

struct vkey_cookie *
vkey_ring_alloc(struct vkey_softc *sc, enum vkey_ring ring, uint64_t cook, size_t replysize, bus_dmamap_t map)
{
	int error = 0;
	bool created = false, alloced = false, loaded = false, incremented = false;
	struct vkey_cookie *cookie = NULL;

	ensure(ring == CMD || ring == REPLY, "invalid ring in alloc, cannot make cookie for completions");
	struct vkey_dma *dma = vkey_dma(sc, ring);

	int n = ring == CMD ? sc->sc_ncmd : sc->sc_ncmd + sc->sc_nreplyfree;
	ensure(n < dma->count, "over-allocating in ring %d", ring);

	cookie = malloc(sizeof(struct vkey_cookie), M_DEVBUF, M_ZERO | M_NOWAIT);
	ensure(cookie, "malloc");

	// committed to allocation
	int index = vkey_ring_usable(sc, ring);
	ensure2(incremented, index >= 0, "BIG FAIL. usable");

	cook += ring == REPLY ? reply_cookie : 0;

	cookie->type = ring;
	cookie->cookie = cook;
	cookie->time = vkey_time();
	cookie->i = index;
	cookie->replylen = 6666666666666666;
	log("allocated cookie %llu, index %d in ring %d", cook, index, ring);

	if (ring == REPLY) {
		ensure(replysize > 0, "replysize");

		// error = bus_dmamap_create(sc->sc_dmat, replysize, 4, replysize, 0, BUS_DMA_ALLOCNOW | BUS_DMA_NOWAIT, &cookie->map);
		// ensure2(created, !error, "create");
		cookie->map = map;

		int nitems = 0;
		error = bus_dmamem_alloc(sc->sc_dmat, replysize, 0, 0,
				cookie->segs, NITEMS(cookie->segs), &nitems,
				BUS_DMA_NOWAIT);
		ensure2(alloced, !error, "alloc");
		cookie->nsegs = nitems;
		cookie->size = replysize;

		error = bus_dmamap_load_raw(sc->sc_dmat, cookie->map, cookie->segs, cookie->nsegs, replysize, BUS_DMA_NOWAIT);
		ensure2(loaded, !error, "load_raw");

		volatile struct vkey_cmd *reply = sc->sc_dma.reply.ptr.replies + index;

		ensure(reply->owner == HOST, "owner incorrect");

		reply->type = -1;
		reply->cookie = cook;
		reply->len1 = reply->len2 = reply->len3 = reply->len4 = 0;
		if (0 < cookie->map->dm_nsegs) {
			reply->len1 = cookie->map->dm_segs[0].ds_len;
			reply->ptr1 = (void *)cookie->map->dm_segs[0].ds_addr;
		}
		if (1 < cookie->map->dm_nsegs) {
			reply->len2 = cookie->map->dm_segs[1].ds_len;
			reply->ptr2 = (void *)cookie->map->dm_segs[1].ds_addr;
		}
		if (2 < cookie->map->dm_nsegs) {
			reply->len3 = cookie->map->dm_segs[2].ds_len;
			reply->ptr3 = (void *)cookie->map->dm_segs[2].ds_addr;
		}
		if (3 < cookie->map->dm_nsegs) {
			reply->len4 = cookie->map->dm_segs[3].ds_len;
			reply->ptr4 = (void *)cookie->map->dm_segs[3].ds_addr;
		}

		vkey_dmamap_sync(sc, REPLY, index, BUS_DMASYNC_PREWRITE);
		reply->owner = DEVICE;
		vkey_dmamap_sync(sc, REPLY, index, BUS_DMASYNC_PREWRITE);

		vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);
		log("DBELL: reply %d", index);
		sc->sc_bar->dbell = reply_mask | index;
		vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

		vkey_dmamap_sync(sc, REPLY, index, BUS_DMASYNC_POSTWRITE);
	}

	RB_INSERT(cookies, &sc->sc_cookies, cookie);
	return cookie;
fail: 
	if (incremented) {
		sc->sc_dma.reply.head--;
		sc->sc_dma.reply.head %= sc->sc_dma.reply.count;
	}
	if (loaded) bus_dmamap_unload(sc->sc_dmat, cookie->map);
	if (alloced) bus_dmamem_free(sc->sc_dmat, cookie->segs, cookie->nsegs);
	if (created) bus_dmamap_destroy(sc->sc_dmat, cookie->map);
	if (cookie) free(cookie, M_DEVBUF, 0);
	return NULL;
}

void
vkey_debug(struct vkey_softc *sc)
{
	// REQUIRES MUTEX
	struct vkey_cookie *c;
	log("VKEY DEBUG. ncmd=%u, nreplyfree=%u, nreplyalloc=%u",
			sc->sc_ncmd, sc->sc_nreplyfree, sc->sc_ncmd + sc->sc_nreplyfree);
	log("heads: cmd=%zu, reply=%zu, comp=%zu", sc->sc_dma.cmd.head, sc->sc_dma.reply.head, sc->sc_dma.comp.head);
	log("COOKIES:");

	unsigned ncmd = 0, nreply = 0;
	RB_FOREACH(c, cookies, &sc->sc_cookies) {
		log("  type=%d, cookie=%llu, index=%zu, [cmd: reply=%llu, replytype=%d, replylen=%zu], [reply: size=%zu]",
				c->type, c->cookie, c->i, c->reply, c->replytype, c->replylen, c->size);
		if (c->type == CMD) ncmd++;
		if (c->type == REPLY) nreply++;
	}
	log("tree ncmd=%u, nreply=%u", ncmd, nreply);
}

bool
vkey_reply_recycle(struct vkey_softc *sc, struct vkey_cookie *reply, bool mustdestroy)
{
	// XXX ASSUMES: reply owner is held by HOST and within ncmd
	ensure(reply, "reply_recycle");

	// XXX if this is a special reply buffer, reclaim it.
	if (reply && (reply->size != defaultreplysize || mustdestroy)) {
		log("destroying reply buffer of size %zu", reply->size);
		if (reply->size != defaultreplysize) log("... due to oversize");
		if (mustdestroy) log("... due to forced destroy");

		RB_REMOVE(cookies, &sc->sc_cookies, reply);

		bus_dmamap_unload(sc->sc_dmat, reply->map);
		bus_dmamem_free(sc->sc_dmat, reply->segs, reply->nsegs);
		bus_dmamap_destroy(sc->sc_dmat, reply->map);

		free(reply, M_DEVBUF, sizeof(*reply));
		reply = NULL;
	}

	// if we read a reply, we will recycle the reply buffer back to the
	// head of the ring for re-use. this is not needed if we didn't read a reply
	// since the buffer will remain in its position ready to use.
	if (reply) {
		RB_REMOVE(cookies, &sc->sc_cookies, reply);

		// release previous reply.
		sc->sc_ncmd--;

		// invalidate old reply descriptor
		volatile struct vkey_cmd *rep = sc->sc_dma.reply.ptr.replies + reply->i;
		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_POSTREAD);
		rep->cookie = -1;
		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_PREWRITE);

		// move cookie into new ring position.
		long i2 = vkey_ring_usable(sc, REPLY);
		ensure(i2 >= 0, "BIG FAIL. usable");

		log("recycling REPLY ring from %zu to %ld", reply->i, i2);
		reply->i = i2;
		reply->cookie = reply_cookie + sc->sc_cookiegen++;
		log("... new cookie %llu", reply->cookie);
		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_POSTREAD);

		volatile struct vkey_cmd *rep2 = sc->sc_dma.reply.ptr.replies + reply->i;
		rep2->cookie = reply->cookie;
		rep2->len1 = rep->len1;
		rep2->len2 = rep->len2;
		rep2->len3 = rep->len3;
		rep2->len4 = rep->len4;
		rep2->ptr1 = rep->ptr1;
		rep2->ptr2 = rep->ptr2;
		rep2->ptr3 = rep->ptr3;
		rep2->ptr4 = rep->ptr4;

		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_POSTREAD);

		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);
		rep2->owner = DEVICE;
		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

		vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);
		log("DBELL: reply %zu", reply->i);
		sc->sc_bar->dbell = reply_mask | reply->i;
		vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

		vkey_dmamap_sync(sc, REPLY, reply->i, BUS_DMASYNC_POSTWRITE);

		// give descriptor back to this command. to be yielded later.
		sc->sc_ncmd++; 

		RB_INSERT(cookies, &sc->sc_cookies, reply);
	}
	return !!reply;
fail:
	log("INVALID STATE: failed to recycle!");
	return false;
}

int
vkeyioctl_cmd(struct vkey_softc *sc, struct proc *p, struct vkey_cmd_arg *arg, size_t *bounce)
{
	int ret = EIO;
	int error = -1;
	bool mutexed = false, created = false, loaded = false, incremented = false, replymapped = false, completed = false;
	bool recycled = true;
	struct vkey_cookie *cmd = NULL, *reply = NULL;
	bus_dmamap_t replymap = NULL;

	// prevent over-bouncing
	size_t bouncesize = *bounce;
	*bounce = 0;

	struct uio cmduio, replyuio;

	cmduio.uio_offset = 0;
	cmduio.uio_iov = arg->vkey_in;
	cmduio.uio_iovcnt = NITEMS(arg->vkey_in);
	cmduio.uio_resid = 0;
	for (unsigned i = 0; i < NITEMS(arg->vkey_in); i++)
		cmduio.uio_resid += arg->vkey_in[i].iov_len;
	cmduio.uio_rw = UIO_WRITE; // write to device
	cmduio.uio_segflg = UIO_USERSPACE;
	cmduio.uio_procp = p;

	replyuio.uio_offset = 0;
	replyuio.uio_iov = arg->vkey_out;
	replyuio.uio_iovcnt = NITEMS(arg->vkey_out);
	replyuio.uio_resid = 0;
	for (unsigned i = 0; i < NITEMS(arg->vkey_out); i++)
		replyuio.uio_resid += arg->vkey_out[i].iov_len;
	replyuio.uio_rw = UIO_READ; // XXX SURELY NOT
	replyuio.uio_segflg = UIO_USERSPACE;
	replyuio.uio_procp = p;

	bus_dmamap_t uiomap = NULL;
	error = bus_dmamap_create(sc->sc_dmat, cmduio.uio_resid, 4, cmduio.uio_resid, 0, BUS_DMA_ALLOCNOW | BUS_DMA_64BIT | BUS_DMA_WAITOK, &uiomap);
	ensure2(created, !error, "bus_dmamap_create");

	error = bus_dmamap_load_uio(sc->sc_dmat, uiomap, &cmduio, BUS_DMA_WAITOK | BUS_DMA_WRITE);
	ensure2(loaded, !error, "load_uio");

	// *************** MUTEX ENTER  ***************
	mtx_enter(&sc->sc_mtx);
	mutexed = true;

	uint64_t cook = sc->sc_cookiegen++;
	ensure(cook < reply_cookie, "cookie counter overflow! maybe the system has been on for too long...");

	log("cookie: %llu, type: %u, cmdlen: %zu", cook, arg->vkey_cmd, cmduio.uio_resid);
	while (true) {
		log("ncmd=%u, nreplyfree=%u", sc->sc_ncmd, sc->sc_nreplyfree);
		while (sc->sc_ncmd >= sc->sc_dma.cmd.count) {
			// XXX don't forget to wakeup when decrementing ncmd
			ensure(0 == msleep_nsec(&sc->sc_ncmd, &sc->sc_mtx, PCATCH | PRIBIO, "vkey sc_ncmd", INFSLP),
					"awoken");
		}
		ensure(sc->sc_ncmd < sc->sc_dma.cmd.count, "BIG FAILURE. spin lock invariant failed");

		ensure(sc->sc_nreplyfree >= 0, "invariant failure!");
		if (sc->sc_nreplyfree == 0) {
			// at this point, due to cmd wait loop and invariant we can be sure there is
			// space for a new reply buffer.
			ensure(sc->sc_nreplyfree + sc->sc_ncmd < sc->sc_dma.reply.count, "BIG FAIL");

			// INSUFFICIENT replies. assign new cookie using dmamap made while unlocked.
			if (!replymap) {
				mtx_leave(&sc->sc_mtx);
				mutexed = false;

				log("allocating new reply buffer of size %zu", bouncesize);
				error = bus_dmamap_create(sc->sc_dmat, bouncesize, 4, bouncesize, 0,
						BUS_DMA_WAITOK | BUS_DMA_ALLOCNOW, &replymap);
				ensure(!error, "create");

				mtx_enter(&sc->sc_mtx);
				mutexed = true;
				// retry loop. we need to verify all the counter variables in one unbroken run.
				continue;
			} else {
				struct vkey_cookie *reply = vkey_ring_alloc(sc, REPLY, cook, bouncesize, replymap);
				ensure(reply, "reply alloc");

				replymap = NULL; // MOVE replymap into cookie
				sc->sc_nreplyfree++;
				log("allocated new reply. now, nreplyfree=%u", sc->sc_nreplyfree);
			}
		} 
		// enough replies. maintain lock and break.
		break;
	}

	// claim cmd descriptor
	cmd = vkey_ring_alloc(sc, CMD, cook, 0, NULL);
	ensure(cmd, "cmd cookie alloc");
	log("index: %zu", cmd->i);
	sc->sc_ncmd++;
	sc->sc_nreplyfree--;
	log("ncmd=%u, nreplyfree=%u", sc->sc_ncmd, sc->sc_nreplyfree);

	ensure(sc->sc_nreplyfree >= 0, "invariant failure!");
	incremented = true;

	// WE SHOULD NOT FAIL FROM HERE UNTIL AFTER WRITING DMA

	vkey_debug(sc);

	// we can always write, the device should be smart enough to handle concurrent write/completion/reply?

	volatile struct vkey_cmd *desc = sc->sc_dma.cmd.ptr.cmds + cmd->i;

	vkey_dmamap_sync(sc, CMD, cmd->i, BUS_DMASYNC_POSTREAD);

	ensure(desc->owner == HOST, "attempt to write on non host-owned descriptor");

	desc->cookie = cmd->cookie;

	// there are always 4.
	desc->len1 = uiomap->dm_segs[0].ds_len;
	desc->len2 = uiomap->dm_segs[1].ds_len;
	desc->len3 = uiomap->dm_segs[2].ds_len;
	desc->len4 = uiomap->dm_segs[3].ds_len;
	desc->ptr1 = (void *)uiomap->dm_segs[0].ds_addr;
	desc->ptr2 = (void *)uiomap->dm_segs[1].ds_addr;
	desc->ptr3 = (void *)uiomap->dm_segs[2].ds_addr;
	desc->ptr4 = (void *)uiomap->dm_segs[3].ds_addr;
	desc->type = arg->vkey_cmd;

	bus_dmamap_sync(sc->sc_dmat, uiomap, 0, uiomap->dm_mapsize, BUS_DMASYNC_PREWRITE);
	vkey_dmamap_sync(sc, CMD, cmd->i, BUS_DMASYNC_PREWRITE);
	vkey_dmamap_sync(sc, REPLY, -1, BUS_DMASYNC_PREREAD);
	vkey_dmamap_sync(sc, COMP, -1, BUS_DMASYNC_PREREAD);

	desc->owner = DEVICE;

	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);
	log("DBELL: %zu", cmd->i);
	sc->sc_bar->dbell = cmd->i;
	vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

	vkey_dmamap_sync(sc, REPLY, -1, BUS_DMASYNC_POSTREAD);
	vkey_dmamap_sync(sc, CMD, cmd->i, BUS_DMASYNC_POSTWRITE);
	bus_dmamap_sync(sc->sc_dmat, uiomap, 0, uiomap->dm_mapsize, BUS_DMASYNC_POSTWRITE);

	do {
		error = msleep_nsec(&cmd->done, &sc->sc_mtx, PCATCH | PRIBIO, "vkey done wait", INFSLP);
		if (error)
			ensure(!error, "sleep disturbed with code %d", error);
	} while (cmd->done == false);
	completed = true;

	vkey_debug(sc);

	log("received reply on cookie %llu", cmd->reply);
	struct vkey_cookie key = { .cookie = cmd->reply, .type = REPLY };
	reply = RB_FIND(cookies, &sc->sc_cookies, &key);
	// ensure(reply, "reply cookie not found in tree. maybe no reply?");
	log("reply cookie: %p", reply);

	mtx_leave(&sc->sc_mtx);
	mutexed = false;

	if (cmd->replylen > reply->size) {
		*bounce = cmd->replylen;
		ensure(cmd->replylen <= reply->size, "reply size %zu exceeds driver buffer size %zu", cmd->replylen, reply->size);
	}

	caddr_t replyptr;
	error = bus_dmamem_map(sc->sc_dmat, reply->segs, reply->nsegs, reply->size, &replyptr, BUS_DMA_WAITOK);
	ensure2(replymapped, !error, "reply dmamem_map");

	bus_dmamap_sync(sc->sc_dmat, reply->map, 0, reply->size, BUS_DMASYNC_POSTREAD);

	size_t oldresid = replyuio.uio_resid;
	if (!(arg->vkey_flags & VKEY_FLAG_TRUNC_OK)) {
		ret = EFBIG;
		ensure(oldresid >= cmd->replylen, "reply too big! reply is %zu but user buffer is only %zu", cmd->replylen, oldresid);
		ret = EIO;
	}

	log("moving %zu bytes into a buffer of size %zu", cmd->replylen, oldresid);
	error = uiomove(replyptr, cmd->replylen, &replyuio);
	ensure(!error, "uiomove faulted");
	size_t written = oldresid - replyuio.uio_resid;
	log("... wrote %zu bytes", written);

	arg->vkey_reply = cmd->replytype;
	arg->vkey_rlen = cmd->replylen;

	ret = 0;

	// XXX if has reply data, obtain from reply cookie.
	// XXX free reply cookie
	// XXX write to user. use uiomove with buf=vkaddr of reply buffer, and uio describing the user-given iovecs. check for oversize.
	
	log("success :3");
fail:
	if (mutexed) {
		mtx_leave(&sc->sc_mtx);
		mutexed = false;
	}
	if (replymapped) bus_dmamem_unmap(sc->sc_dmat, replyptr, reply->size);

	if (!completed) {
		log("... WARNING: command abandoned. not cleaning reply yet.");
	}
	if (completed) {
		mtx_enter(&sc->sc_mtx);
		recycled = vkey_reply_recycle(sc, reply, !!*bounce);
		mtx_leave(&sc->sc_mtx);
	}
	if (incremented) {
		// return to pool if fully completed.
		mtx_enter(&sc->sc_mtx);
		if (completed) {
			sc->sc_ncmd--;
			if (recycled) sc->sc_nreplyfree++;
		}
		wakeup(&sc->sc_ncmd);
		mtx_leave(&sc->sc_mtx);
	}
	if (reply) {
		// bus_dmamap_unload(sc->sc_dmat, reply->map);
		// bus_dmamem_free(sc->sc_dmat, reply->segs, reply->nsegs);
		// bus_dmamap_destroy(sc->sc_dmat, reply->map);
		// free(reply, M_DEVBUF, 0);
	}
	if (cmd) {
		mtx_enter(&sc->sc_mtx);
		RB_REMOVE(cookies, &sc->sc_cookies, cmd);
		mtx_leave(&sc->sc_mtx);
		free(cmd, M_DEVBUF, sizeof(*cmd));
	}
	if (replymap) bus_dmamap_destroy(sc->sc_dmat, replymap);
	if (loaded) bus_dmamap_unload(sc->sc_dmat, uiomap);
	if (created) bus_dmamap_destroy(sc->sc_dmat, uiomap);
	log("ncmd=%u, nreplyfree=%u", sc->sc_ncmd, sc->sc_nreplyfree);
	log("return with error=%d", ret);
	return ret;
}

int
vkeyioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	printf("vkey %d ioctl\n", dev);
	struct vkey_info_arg *vi;
	size_t bounce = defaultreplysize;
	unsigned i = 0;

	struct vkey_softc *sc = (void *)device_lookup(&vkey_cd, minor(dev));
	if (sc == NULL)
		return ENXIO;

	int ret = EINVAL;
	switch (cmd) {
	case VKEYIOC_GET_INFO:
		vi = (void *)data;
		vi->vkey_major = sc->sc_bar->vmaj;
		vi->vkey_major = sc->sc_bar->vmin;
		ret = 0;
		break;
	case VKEYIOC_CMD:
		while (bounce != 0) {
			ret = vkeyioctl_cmd(sc, p, (void *)data, &bounce);
			ensure(!bounce, "blocked bounce. reply oversize, requires %zu bytes", bounce);
			if (bounce) {
				log("bouncing! to size %zu", bounce);
			}
			i++;
			ensure(i <= 5, "aborting excessive bouncing");
		}
		break;
	}
fail:
	return ret;
}

static int
vkey_intr(void *arg)
{
	unsigned nprocessed = 0;
	bool mutexed = false;
	bool failed = true;
	long h = -1;

	struct vkey_softc *sc = arg;
	log("vkey_intr enter, h=%zu", sc->sc_dma.comp.head);

	ensure(vkey_check(sc), "check");

	failed = false;
	for (nprocessed = 0; !failed; nprocessed++) {
		failed = true;
		h = sc->sc_dma.comp.head;

		vkey_dmamap_sync(sc, COMP, h, BUS_DMASYNC_POSTREAD);

		volatile struct vkey_comp *comp = sc->sc_dma.comp.ptr.comps + h;
		if (comp->owner != HOST) {
			// finished processing completions for now
			log("stopped processing at owner=%x", comp->owner);
			// ensure(nprocessed > 0, "processed zero completions. interrupt fired too early?");
			failed = false;
			break;
		}
		sc->sc_dma.comp.head++;
		sc->sc_dma.comp.head %= sc->sc_dma.comp.count;

		log("processing completion (%u) index %zu, type %d, cmd %llu, reply %llu, replylen=%u, owner=%x",
				nprocessed, h, comp->type, comp->cmd_cookie, comp->reply_cookie,
				comp->msglen, comp->owner);

		mtx_enter(&sc->sc_mtx);
		mutexed = true;

		struct vkey_cookie key = { .cookie = comp->cmd_cookie, .type = CMD };
		struct vkey_cookie *cmd = RB_FIND(cookies, &sc->sc_cookies, &key);

		key = (struct vkey_cookie) { .cookie = comp->reply_cookie, .type = REPLY };
		struct vkey_cookie *reply = RB_FIND(cookies, &sc->sc_cookies, &key);

		if (comp->reply_cookie == 0 && comp->msglen == 0) {
			log("... completion without reply");
		} else {
			ensure(reply, "reply not found when expected (INVALID STATE)");
			log("... reply cookie %llu index %zu", reply->cookie, reply->i);

			if (cmd) {
				// if a command exists, it takes ownership of the reply.
				cmd->replytype = comp->type;
				cmd->replylen = comp->msglen;
				cmd->reply = comp->reply_cookie;
				cmd->done = true;
				wakeup(&cmd->done);
			} else {
 				log("... completion cmd not found. command abandoned?");
			}
		}
		mtx_leave(&sc->sc_mtx);
		mutexed = false;

		if (!cmd && reply) {
 			log("... destroying reply");

			mtx_enter(&sc->sc_mtx);
 			bool recycled = vkey_reply_recycle(sc, reply, false);
 			sc->sc_ncmd--;
 			if (recycled) sc->sc_nreplyfree++;
			mtx_leave(&sc->sc_mtx);
		}

		failed = false;
fail:
		if (h >= 0) {
			vkey_dmamap_sync(sc, COMP, h, BUS_DMASYNC_POSTREAD);
			vkey_dmamap_sync(sc, COMP, h, BUS_DMASYNC_PREWRITE);
			comp->owner = DEVICE;
			// vkey_dmamap_sync(sc, COMP, -1, BUS_DMASYNC_PREWRITE | BUS_DMASYNC_PREREAD);

			vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);
			log("... CPDBELL: %zu", h);
			sc->sc_bar->cpdbell = h;
			vkey_bar_barrier(sc, BUS_SPACE_BARRIER_WRITE);

			vkey_dmamap_sync(sc, COMP, h, BUS_DMASYNC_POSTWRITE);
			vkey_dmamap_sync(sc, COMP, h, BUS_DMASYNC_PREREAD);
		}
	}

	log("vkey_intr leave, h=%zu (processed %u) (failing=%d)", sc->sc_dma.comp.head, nprocessed, failed);
	if (mutexed) mtx_leave(&sc->sc_mtx);
	return 0;
	// !!! DO NOT return rings to HOST owner here. let ioctl do that to ensure it has read.
}
