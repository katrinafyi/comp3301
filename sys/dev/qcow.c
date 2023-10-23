

/*
 * Copyright (c) 2023 The University of Queensland
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/namei.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/limits.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/ioctl.h>
#include <sys/disklabel.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/conf.h>
#include <sys/dkio.h>
#include <sys/specdev.h>
#include <sys/tree.h>

#include <dev/qcowvar.h>


#define NITEMS(x) (sizeof(x) / sizeof(*(x)))

#define log(msg, ...) \
    printf("%s:%d\t" msg "\n", __func__, __LINE__, ##__VA_ARGS__)

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

#define ensure3(cond, err, msg, ...)\
do {\
	int CONCAT(ensure_err, __LINE__) = error;\
	ensure(cond, msg, ##__VA_ARGS__);\
	error = CONCAT(ensure_err, __LINE__);\
} while (0);

#define modify(variable, macro)\
variable = macro(variable);


#define QCOW_NLEN	(QCOW2_BACKING_FILE_SIZE + 1) /* add nul */

CTASSERT(sizeof(struct qcow2_file_header) == 104);
CTASSERT(sizeof(struct qcow2_l1_entry) == sizeof(uint64_t));
CTASSERT(sizeof(struct qcow2_l2_entry) == sizeof(uint64_t));

struct qcow_softc {
	struct device		 sc_dev;
	RBT_ENTRY(qcow_softc)	 sc_entry;
	struct refcnt		 sc_refs;

	struct disk		 sc_dk;
	size_t			 sc_secsize;
	size_t			 sc_seccount;

	size_t sc_clustersize;

	char			*sc_fname;
	size_t			 sc_fnamelen;
	struct vnode		*sc_vp;
	struct ucred		*sc_ucred;
	int			 sc_rw;

	struct qcow2_file_header sc_header;
	// struct qcow2_l1_entry *sc_l1;
	size_t sc_l1_size;
};

RBT_HEAD(qcow_softcs, qcow_softc);

static inline int
qcow_cmp(const struct qcow_softc *a, const struct qcow_softc *b)
{
	const struct device *da = &a->sc_dev;
	const struct device *db = &b->sc_dev;

	if (da->dv_unit > db->dv_unit)
		return (1);
	if (da->dv_unit < db->dv_unit)
		return (-1);
	return (0);
}

RBT_PROTOTYPE(qcow_softcs, qcow_softc, sc_entry, qcow_cmp);

struct qcow_driver {
	struct qcow_softcs		qd_scs;
	struct rwlock			qd_lock;
};

static struct qcow_driver qd = {
	.qd_scs		= RBT_INITIALIZER(),
	.qd_lock	= RWLOCK_INITIALIZER("qcowdrv")
};

static int	 qcow_getdisklabel(dev_t, struct qcow_softc *,
		     struct disklabel *, int);

void
qcowattach(int num)
{
	/* qd is already set up */
}

static struct qcow_softc *
qcow_find(dev_t dev)
{
	struct device key = {
		.dv_unit = DISKUNIT(dev)
	};

	return (RBT_FIND(qcow_softcs, &qd.qd_scs,
	    (const struct qcow_softc *)&key));
}

static struct qcow_softc *
qcow_create(dev_t dev)
{
	int unit = DISKUNIT(dev);
	struct qcow_softc *sc;
	struct device *dv;

	sc = malloc(sizeof(*sc), M_DEVBUF, M_WAITOK|M_CANFAIL|M_ZERO);
	if (sc == NULL)
		return (NULL);

	refcnt_init(&sc->sc_refs);

	dv = &sc->sc_dev;
	dv->dv_class = DV_DISK;
	dv->dv_unit = unit;
	if (snprintf(dv->dv_xname, sizeof(dv->dv_xname),
	    "qcow%d", dv->dv_unit) >= sizeof(dv->dv_xname))
		panic("%s dv_xname printf", __func__);
	dv->dv_ref = 1;

	sc->sc_dk.dk_name = dv->dv_xname;

	return (sc); /* give ref to the caller */
}

static int
qcow_insert(struct qcow_softc *sc)
{
	if (RBT_INSERT(qcow_softcs, &qd.qd_scs, sc) != NULL)
		return (EBUSY);

	refcnt_take(&sc->sc_refs); /* take one for the tree */
	return (0);
}

static void
qcow_remove(struct qcow_softc *sc)
{
	RBT_REMOVE(qcow_softcs, &qd.qd_scs, sc);
	refcnt_rele(&sc->sc_refs); /* caller must be holding a ref too */
}

static struct qcow_softc *
qcow_enter(dev_t dev)
{
	struct qcow_softc *sc;

	rw_enter_read(&qd.qd_lock);
	sc = qcow_find(dev);
	if (sc != NULL)
		refcnt_take(&sc->sc_refs);
	rw_exit_read(&qd.qd_lock);

	log("qcow_enter: %p", sc);

	return (sc);
}

static void
qcow_leave(struct qcow_softc *sc)
{
	if (refcnt_rele(&sc->sc_refs)) {
		KASSERT(sc->sc_dev.dv_ref == 1);

		vn_close(sc->sc_vp, sc->sc_rw, sc->sc_ucred, curproc);
		crfree(sc->sc_ucred);

		free(sc->sc_fname, M_DEVBUF, sc->sc_fnamelen);
		free(sc, M_DEVBUF, sizeof(*sc));
	}
}

static int
qcow_disk_open(struct qcow_softc *sc, int part, int mode)
{
	int error;

	error = disk_lock(&sc->sc_dk);
	if (error != 0)
		return (0);

	error = disk_openpart(&sc->sc_dk, part, mode, 1);
	disk_unlock(&sc->sc_dk);

	return (error);
}

int
qcowopen(dev_t dev, int flags, int fmt, struct proc *p)
{
	dev_t part = DISKPART(dev);
	struct qcow_softc *sc;
	int raw = part == RAW_PART && fmt == S_IFCHR;
	int error;

	sc = qcow_enter(dev);
	if (sc == NULL) {
		/* allow opens of /dev/rqcowXc for attach ioctls */
		if (raw)
			return (0);

		return (ENXIO);
	}

	if (ISSET(flags, FWRITE) && !ISSET(sc->sc_rw, FWRITE) && !raw) {
		error = EACCES;
		goto leave;
	}

	if (sc->sc_dk.dk_openmask == 0) {
		error = qcow_getdisklabel(dev, sc, sc->sc_dk.dk_label, 0);
		if (error == EIO || error == ENXIO)
			goto leave;
	}

	error = qcow_disk_open(sc, part, fmt);

leave:
	qcow_leave(sc);

	return (error);
}

int
qcowclose(dev_t dev, int flags, int fmt, struct proc *p)
{
	dev_t part = DISKPART(dev);
	struct qcow_softc *sc;

	sc = qcow_enter(dev);
	if (sc == NULL) {
		KASSERT(part == RAW_PART && fmt == S_IFCHR);
		return (0);
	}

	disk_lock_nointr(&sc->sc_dk);
	disk_closepart(&sc->sc_dk, part, fmt);
	disk_unlock(&sc->sc_dk);
	qcow_leave(sc);

	return (0);
}

int
qcow_rdwr(struct qcow_softc *sc, enum uio_rw rw, size_t offset, size_t len, caddr_t dest, size_t *remaining)
{
	if (!(sc->sc_rw & FWRITE)) {
		ensure(rw != UIO_WRITE, "INVALID: attempt to write to read-only file");
	}
	// XXX take cluster locks maybe? or require those are taken higher up for modifying of tables. 
	return vn_rdwr(rw, sc->sc_vp, dest, len, offset, UIO_SYSSPACE,
	    IO_NOCACHE | IO_SYNC | IO_NOLIMIT, sc->sc_ucred, remaining, curproc);
fail:
	return EROFS;
}

int
qcow_cluster_rdwr(struct qcow_softc *sc, enum uio_rw rw, size_t cluster, caddr_t data, size_t len, size_t *remaining)
{
	size_t off = cluster * sc->sc_clustersize;
	return qcow_rdwr(sc, rw, off, len, data, remaining);
}

int
qcow_header_read(struct qcow_softc *sc)
{
	int error = EIO;
	struct qcow2_file_header *headbuf = NULL;
	struct qcow2_l1_entry *l1buf = NULL;
	// sc->sc_clustersize = sizeof(struct qcow2_file_header);
	headbuf = malloc(sizeof(*headbuf), M_DEVBUF, M_WAITOK | M_ZERO);
	ensure3(headbuf, ENOMEM, "malloc");

	size_t remaining = sizeof(*headbuf);
	error = qcow_rdwr(sc, UIO_READ, 0, remaining, (void *)headbuf, &remaining);
	ensure(!error, "read");
	ensure(remaining == 0, "partial read?");

	error = ENOTSUP;
	ensure(0 == headbuf->backing_file_offset,
			"backing file unsupported");

	ensure(!headbuf->incompatible_features,
			"incompatible features used: %llx", betoh64(headbuf->incompatible_features));

	log("detecting autoclear features = %llu", headbuf->autoclear_features);
	if ((sc->sc_rw & FWRITE) && headbuf->autoclear_features) {
		log("... clearing");
		headbuf->autoclear_features = 0;
		remaining = sizeof(*headbuf);
		error = qcow_rdwr(sc, UIO_WRITE, 0, remaining, (void *)headbuf, &remaining);
		ensure(!error, "autoclear write-back");
	}

	error = kcopy(headbuf, &sc->sc_header, sizeof(sc->sc_header));
	ensure(!error, "kcopy");

	struct qcow2_file_header *h = &sc->sc_header;
	modify(h->magic, betoh32);
	modify(h->version, betoh32);

	modify(h->backing_file_offset, betoh64);
	modify(h->backing_file_size, betoh32);

	modify(h->cluster_bits, betoh32);

	modify(h->size, betoh64);
	modify(h->crypt_method, betoh32);

	modify(h->l1_num_entries, betoh32);
	modify(h->l1_table_offset, betoh64);
	modify(h->refcount_table_offset, betoh64);
	modify(h->refcount_table_clusters, betoh32);
	modify(h->nb_snapshots, betoh32);
	modify(h->snapshots_offset, betoh32);

	// v3
	modify(h->incompatible_features, betoh64);
	modify(h->compatible_features, betoh64);
	modify(h->autoclear_features, betoh64);
	modify(h->refcount_order, betoh32);
	modify(h->header_length, betoh32);

	log("header %x (expected %x)", h->magic, QCOW2_MAGIC);
	ensure(h->magic == QCOW2_MAGIC, "magic number mismatch");
	log("version %d, size %llu", h->version, h->size);
	ensure(h->version == 2 || h->version == 3, "version mismatch");
	log("actual header len %d", h->header_length);
	ensure(h->header_length >= sizeof(struct qcow2_file_header), "header size violation, headersize=%d", h->header_length);
	log("cluster bits %d, size %d", h->cluster_bits, 1 << h->cluster_bits);
	ensure(QCOW2_CLUSTER_BITS_MIN <= h->cluster_bits && h->cluster_bits <= QCOW2_CLUSTER_BITS_MAX, "cluster bits = %u", h->cluster_bits);

	error = ENODEV;
	ensure(h->crypt_method == QCOW2_CRYPT_METHOD_NONE, "driver does not support encryption");

	sc->sc_l1_size = h->l1_num_entries * sizeof(struct qcow2_l1_entry);

	error = 0;
fail:
	// if (error && sc->sc_l1) free(sc->sc_l1, M_DEVBUF, sc->sc_l1_size);
	if (l1buf) free(l1buf, M_DEVBUF, sc->sc_l1_size);
	if (headbuf) free(headbuf, M_DEVBUF, sc->sc_clustersize);
	return error;
}

void
qcowstrategy(struct buf *bp)
{
	struct qcow_softc *sc;
	int error;
	int s;

	struct qcow2_l1_entry *l1buf = NULL;
	struct qcow2_l2_entry *l2buf = NULL;
	caddr_t clusterbuf = NULL;

	ensure(bp->b_resid <= bp->b_bcount,
			"INVALID: size of operation is smaller than buffer?!");

	sc = qcow_enter(bp->b_dev);
	if (sc == NULL) {
		bp->b_error = ENXIO;
		goto fail;
	}
	qcow_leave(sc);

	int64_t i = -1;
	caddr_t datap = bp->b_data;

copycluster:
	i++;
	ensure(i < 1000, "surely not");
	log("subop %lli: resid=%zu, bcount=%zu, lblkno=%lld, part=%d",
			i, bp->b_resid, bp->b_bcount, bp->b_lblkno, DISKPART(bp->b_dev));

	ensure(bp->b_flags & B_READ, "only read is supported right now");

	struct qcow2_file_header *h = &sc->sc_header;
	l1buf = malloc(sc->sc_l1_size, M_DEVBUF, M_WAITOK | M_ZERO);
	l2buf = malloc(sc->sc_clustersize, M_DEVBUF, M_WAITOK | M_ZERO);
	clusterbuf = malloc(sc->sc_clustersize, M_DEVBUF, M_WAITOK | M_ZERO);
	ensure3(l1buf, EIO, "malloc for l1 table");
	ensure3(l2buf, EIO, "malloc for l2 table");
	ensure(h->l1_table_offset % sc->sc_clustersize == 0, "l1 must begin at cluster offset.");

	size_t remaining = sc->sc_l1_size;
	log("l1 offset=%llu, nentries=%u, size=%zu", h->l1_table_offset, h->l1_num_entries, sc->sc_l1_size);
	error = qcow_rdwr(sc, UIO_READ, h->l1_table_offset, sc->sc_l1_size, (void *)l1buf, &remaining);
	ensure(!error, "qcow_rdwr for l1");
	ensure(remaining == 0, "partial read?");

	uint64_t entries_per_l2_table = sc->sc_clustersize / sizeof(struct qcow2_l2_entry);
	uint64_t vbytes_per_l2_table = entries_per_l2_table * sc->sc_clustersize;
	uint64_t vbytes_maximum =  h->l1_num_entries * vbytes_per_l2_table;
	log("l1size is big enough for %llu bytes", vbytes_maximum);
	ensure(h->size <- vbytes_maximum, "l1 table is too small!");

	// XXX logging could be deleted... BUT! MAKE SURE TO FIX BITS
	for (unsigned i = 0; i < h->l1_num_entries; i++) {
		log("l1 entry %u val = %016llx (before reverse)", i, l1buf[i].val);
		modify(l1buf[i].val, betoh64);
		log("l1 entry %u val = %016llx", i, l1buf[i].val);
		// XXX calculate, somehow, the range of virtual addresses under each l1 entry (and hence each l2 table.)
		// note: l2 size = cluster size

		// one l2 entry defines the location of a cluster which assigns "cluster size" vbytes
		// a l2 table has "cluster size / l2 entry size" entries
		// a l1 table entry has one l2 table.

		uint64_t off0 = vbytes_per_l2_table * i;
		uint64_t off1 = off0 + vbytes_per_l2_table - 1;

		size_t off = QCOW2_L1E_OFFSET_MASK & l1buf[i].val;
		size_t bit = QCOW2_L1E_BIT_MASK & l1buf[i].val;
		bit >>= 63;
		log("l1 entry %u (up to %llx=%llu): offset=%zx, bit=%zx",
				i, off1, off1,  off, bit);
	}


	off_t offset;
	struct partition *p;
	p = &sc->sc_dk.dk_label->d_partitions[DISKPART(bp->b_dev)];
	offset = DL_GETPOFFSET(p) * sc->sc_dk.dk_label->d_secsize +
	    (u_int64_t)bp->b_blkno * DEV_BSIZE;
	// offset is a VIRTUAL address!

	uint64_t cluster_size = 1 << h->cluster_bits;
	uint64_t l2_entries = cluster_size / sizeof(uint64_t);
	// index within l2/l1 tables
	uint64_t l2_index = (offset / cluster_size) % l2_entries;
	uint64_t l1_index = (offset / cluster_size) / l2_entries;

	log("targeting virtual offset: %llx", offset);
	log("... l1_index=%llx, l1_offset=%llx", l1_index,
			h->l1_table_offset + sizeof(struct qcow2_l1_entry) * l1_index);

	uint64_t l2_table_offset = QCOW2_L1E_OFFSET_MASK & l1buf[l1_index].val;
	log("... l2_table_offset=%llx. l2_index=%llx, l2_offset=%llx",
			l2_table_offset, l2_index,
			l2_table_offset + sizeof(struct qcow2_l2_entry) * l2_index);
	remaining = sc->sc_clustersize;

	ensure(0 != l2_table_offset, "SHORT CIRCUIT: l2 table is unallocated");
	remaining = sc->sc_clustersize;
	error = qcow_rdwr(sc, UIO_READ, l2_table_offset, remaining, (void *)l2buf, &remaining);
	ensure(!error, "l2 table read");
	ensure(remaining == 0, "short 1");

	struct qcow2_l2_entry l2_entry = l2buf[l2_index];
	modify(*(uint64_t *)&l2_entry, betoh64);
	error = ENOTSUP;
	ensure(!(QCOW2_L2E_ISCOMPRESSED & l2_entry.val), "unsup: l2 entry is compressed");

	uint64_t cluster_offset = QCOW2_L2E_DESC_OFFSET & l2_entry.val;
	log("... cluster_offset=%llx", cluster_offset);
	if (0 == cluster_offset && (QCOW2_L2E_ISSINGULAR & l2_entry.val)) {
		ensure(0 != cluster_offset, "SHORT CIRCUIT: l2 table is unallocated");
	}
	if (QCOW2_L2E_DESC_ALLZERO & l2_entry.val) {
		ensure(0, "SHORT CIRCUIT: cluster is all zeros.");
	}

	remaining = sc->sc_clustersize;
	error = qcow_rdwr(sc, UIO_READ, cluster_offset, remaining, (void *)clusterbuf, &remaining);
	ensure(!error, "cluster read");
	ensure(remaining == 0, "short 2");

	size_t copysize = MIN(bp->b_resid, sc->sc_clustersize);
	error = kcopy(clusterbuf, datap, copysize);
	ensure(!error, "kcopy = %d", error);
	bp->b_resid -= copysize;
	datap += copysize;

	log("remaining bytes = %zu", bp->b_resid);
	if (bp->b_resid > 0) {
		goto copycluster;
	}

	struct stat stat;
	log("b_proc=%p, curproc=%p", bp->b_proc, curproc);
	error = vn_stat(sc->sc_vp, &stat, curproc);
	// error = VOP_GETATTR(bp->b_vp, &stat, sc->sc_ucred, curproc);
	ensure(!error, "vn_stat returned %d", error);

	offset = stat.st_size;
	log("size=%lld", offset);

	/* XXX do actual qcow IO here */
	// bp->b_error = vn_rdwr((bp->b_flags & B_READ) ? UIO_READ : UIO_WRITE,
	//     sc->sc_vp, bp->b_data, bp->b_bcount, off, UIO_SYSSPACE,
	//     IO_NOCACHE | IO_SYNC | IO_NOLIMIT, sc->sc_ucred, &bp->b_resid, curproc);
	
	goto done;

fail:
	bp->b_error = EIO;
	bp->b_flags |= B_ERROR;
	bp->b_resid = bp->b_bcount;
done:
	if (clusterbuf) free(clusterbuf, M_DEVBUF, sc->sc_clustersize);
	if (l2buf) free(l2buf, M_DEVBUF, sc->sc_clustersize);
	if (l1buf) free(l1buf, M_DEVBUF, sc->sc_l1_size);
	s = splbio();
	biodone(bp);
	splx(s);
}

int
qcowread(dev_t dev, struct uio *uio, int flags)
{
	return (physio(qcowstrategy, dev, B_READ, minphys, uio));
}

int
qcowwrite(dev_t dev, struct uio *uio, int flags)
{
	return (physio(qcowstrategy, dev, B_WRITE, minphys, uio));
}

static int
qcow_attach(dev_t dev, int flag, const struct qcow_attach *qc, struct proc *p)
{
	struct qcow_softc *sc;
	int part = DISKPART(dev);
	struct vnode *vp;
	char fname[PATH_MAX];
	size_t fnamelen;
	struct nameidata nd;
	int rw;
	int error;

	/*
	 * we can't be here without this being true, but it's
	 * nice to be sure.
	 */
	if (part != RAW_PART || !vfinddev(dev, VCHR, &vp))
		return (ENOTTY);
	if (!ISSET(flag, FWRITE))
		return (EBADF);

	/* check secbits */

	error = rw_enter(&qd.qd_lock, RW_WRITE|RW_INTR);
	if (error != 0)
		return (error);

	sc = qcow_find(dev);
	rw_exit(&qd.qd_lock);
	if (sc != NULL)
		return (EBUSY);

	error = copyinstr(qc->qc_file, fname, sizeof(fname), &fnamelen);
	if (error != 0)
		return (error);

	NDINIT(&nd, 0, 0, UIO_SYSSPACE, fname, p);
	nd.ni_unveil = UNVEIL_READ;
	rw = FREAD;
	if (!qc->qc_readonly) {
		nd.ni_unveil |= UNVEIL_WRITE;
		rw |= FWRITE;
	}
	error = vn_open(&nd, rw, 0);
	if (error != 0)
		return (error);

	vp = nd.ni_vp;
	VOP_UNLOCK(vp);
	if (vp->v_type != VREG) {
		error = EOPNOTSUPP;
		goto close;
	}

sc = qcow_create(dev);
	if (sc == NULL) {
		error = ENOMEM;
		goto close;
	}

	sc->sc_fname = malloc(fnamelen, M_DEVBUF, M_WAITOK|M_CANFAIL);
	if (sc->sc_fname == NULL) {
		error = ENOMEM;
		goto destroy;
	}

	memcpy(sc->sc_fname, fname, fnamelen);
	sc->sc_fnamelen = fnamelen;
	sc->sc_vp = vp;
	sc->sc_ucred = crhold(p->p_ucred);
	sc->sc_rw = rw;

	log("qcow attach: %s", sc->sc_fname);

	error = qcow_header_read(sc);
	if (error) {
		log("error %d at qcow_header_read", error);
		goto close;
	}

	sc->sc_clustersize = 1 << sc->sc_header.cluster_bits;

	sc->sc_secsize = 1 << qc->qc_secbits;
	sc->sc_seccount = sc->sc_header.size / sc->sc_secsize; // XXX TODO: derive seccount from header.
	if (!(sc->sc_seccount >= 1)) {
		log("error: requested sector size is larger than virtual disk size?");
		log("... sector size = %zu, virtual size = %llu", sc->sc_secsize, sc->sc_header.size);
		error = ENODEV;
		goto freefname;
	}

	error = rw_enter(&qd.qd_lock, RW_WRITE|RW_INTR);
	if (error != 0)
		goto freefname;

	error = qcow_insert(sc);
	if (error != 0)
		goto rollback;


	disk_attach(&sc->sc_dev, &sc->sc_dk);

	rw_exit(&qd.qd_lock);

	return (error);

rollback:
	rw_exit(&qd.qd_lock);
freefname:
	free(sc->sc_fname, M_DEVBUF, sc->sc_fnamelen);
destroy:
	free(sc, M_DEVBUF, sizeof(*sc));
close:
	vn_close(vp, rw, p->p_ucred, p);
	return (error);
}

static int
qcow_detach(struct qcow_softc *sc, dev_t dev, int flag, unsigned int force)
{
	struct vnode *vp;
	int part = DISKPART(dev);
	int error;

	if (part != RAW_PART || !vfinddev(dev, VCHR, &vp))
		return (ENOTTY);
	if (!ISSET(flag, FWRITE))
		return (EBADF);

	error = rw_enter(&qd.qd_lock, RW_WRITE|RW_INTR);
	if (error != 0)
		return (error);

	if (!force) {
		struct disk *dk = &sc->sc_dk;
		int pmask = (1 << part);

		error = disk_lock(dk);
		if (error != 0)
			goto leave;

		if (ISSET(dk->dk_copenmask, ~pmask) || dk->dk_bopenmask)
			error = EBUSY;

		disk_unlock(&sc->sc_dk);

		if (error != 0)
			goto leave;
	}

	qcow_remove(sc);
	rw_exit(&qd.qd_lock);

	disk_gone(qcowopen, sc->sc_dev.dv_unit);
	disk_detach(&sc->sc_dk);

	return (0);

leave:
	rw_exit(&qd.qd_lock);
	return (error);
}

int
qcowioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	log("ioctl");
	struct qcow_softc *sc;
	struct qcow_fname *fnameargs = NULL;
	struct disklabel *lp;

	int error = 0;

	if (cmd == QCOWIOCATTACH)
		return (qcow_attach(dev, flag, (struct qcow_attach *)data, p));

	/* everything else needs an attached disk */
	sc = qcow_enter(dev);
	if (sc == NULL)
		return (ENXIO);

	switch (cmd) {
	case QCOWIOCFNAME:
		fnameargs = (void *)data;
		if (sc->sc_fnamelen == 0)
			error = ENOENT;
		if (error) break;
		log("ioc fname: %s (%zu)", sc->sc_fname, sc->sc_fnamelen);

		error = EFBIG;
		ensure(sc->sc_fnamelen <= sizeof(fnameargs->qc_name), "name output buffer small!");

		error = kcopy(sc->sc_fname, fnameargs->qc_name, sc->sc_fnamelen);
		if (error) break;

		error = 0;
		break;
	case QCOWIOCSTAT:
		log("qcow stat");
		error = vn_stat(sc->sc_vp, (struct stat *)data, p);
		break;

	case QCOWIOCDETACH:
		error = qcow_detach(sc, dev, flag, *(unsigned int *)data);
		break;

	case DIOCRLDINFO:
		lp = malloc(sizeof(*lp), M_TEMP, M_WAITOK);
		qcow_getdisklabel(dev, sc, lp, 0);
		*(sc->sc_dk.dk_label) = *lp;
		free(lp, M_TEMP, sizeof(*lp));
		break;

	case DIOCGPDINFO:
		qcow_getdisklabel(dev, sc, (struct disklabel *)data, 1);
		break;

	case DIOCGDINFO:
		*(struct disklabel *)data = *(sc->sc_dk.dk_label);
		break;

	case DIOCGPART:
		((struct partinfo *)data)->disklab = sc->sc_dk.dk_label;
		((struct partinfo *)data)->part =
		    &sc->sc_dk.dk_label->d_partitions[DISKPART(dev)];
		break;

	case DIOCWDINFO:
	case DIOCSDINFO:
		if (!ISSET(flag, FWRITE)) {
			error = EBADF;
			break;
		}

		error = disk_lock(&sc->sc_dk);
		if (error != 0)
			break;

		error = setdisklabel(sc->sc_dk.dk_label,
		    (struct disklabel *)data, sc->sc_dk.dk_openmask);
		if (error == 0) {
			if (cmd == DIOCWDINFO)
				error = writedisklabel(DISKLABELDEV(dev),
				    qcowstrategy, sc->sc_dk.dk_label);
		}

		disk_unlock(&sc->sc_dk);
		break;

	default:
		error = ENOTTY;
		break;
	}
fail:
	qcow_leave(sc);

	return (error);
}

daddr_t
qcowsize(dev_t dev)
{
	/* we don't support swapping to qcow. */
	return (-1);
}

int
qcowdump(dev_t dev, daddr_t blkno, caddr_t va, size_t size)
{
	/* we don't support dumping to qcow. */
	return (ENXIO);
}

static int
qcow_getdisklabel(dev_t dev, struct qcow_softc *sc, struct disklabel *lp,
    int spoofonly)
{
	memset(lp, 0, sizeof(*lp));

	/* disk geometry (i hate this stuff) */

	/* # of bytes per sector */
	lp->d_secsize = sc->sc_secsize;

	/* # of data sectors per track */
	lp->d_nsectors = 1; // XXX

	/* # of tracks per cylinder */
	lp->d_ntracks = 1;

	/* # of data sectors per cylinder */
	lp->d_secpercyl = lp->d_ntracks * lp->d_nsectors;

	/* # of data cylinders per unit */
	lp->d_ncylinders = sc->sc_seccount / lp->d_secpercyl;

	/* # of data sectors (low part) */
	/* lp->d_secperunit = ??; */

	lp->d_type = DTYPE_VND;
	strncpy(lp->d_typename, "QCOW2 File", sizeof(lp->d_typename));
	strncpy(lp->d_packname, "s4529458", sizeof(lp->d_packname));
	DL_SETDSIZE(lp, sc->sc_seccount);
	lp->d_version = 1;

	lp->d_magic = DISKMAGIC;
	lp->d_magic2 = DISKMAGIC;
	lp->d_checksum = dkcksum(lp);

	return (readdisklabel(DISKLABELDEV(dev), qcowstrategy, lp, spoofonly));
}

RBT_GENERATE(qcow_softcs, qcow_softc, sc_entry, qcow_cmp);
