

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


#define QCOW_NLEN	(QCOW2_BACKING_FILE_SIZE + 1) /* add nul */

struct qcow_softc {
	struct device		 sc_dev;
	RBT_ENTRY(qcow_softc)	 sc_entry;
	struct refcnt		 sc_refs;

	struct disk		 sc_dk;
	size_t			 sc_secsize;
	size_t			 sc_seccount;

	char			*sc_fname;
	size_t			 sc_fnamelen;
	struct vnode		*sc_vp;
	struct ucred		*sc_ucred;
	int			 sc_rw;
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

void
qcowstrategy(struct buf *bp)
{
	struct qcow_softc *sc;
	int error;
	int s;

	sc = qcow_enter(bp->b_dev);
	if (sc == NULL) {
		bp->b_error = ENXIO;
		goto fail;
	}
	qcow_leave(sc);

	log("begin strategy. resid=%zu, lblkno=%lld, part=%d",
			bp->b_resid, bp->b_lblkno,
			DISKPART(bp->b_dev));

	off_t off;
	struct partition *p;
	p = &sc->sc_dk.dk_label->d_partitions[DISKPART(bp->b_dev)];
	off = DL_GETPOFFSET(p) * sc->sc_dk.dk_label->d_secsize +
	    (u_int64_t)bp->b_blkno * DEV_BSIZE;

	struct vattr va;
	error = VOP_GETATTR(bp->b_vp, &va, sc->sc_ucred, curproc);
	ensure(error, "VOP_GETATTR returned %d", error);

	off = va.va_size;
	log("size=%lld", off);

	/* XXX do actual qcow IO here */
	bp->b_error = vn_rdwr((bp->b_flags & B_READ) ? UIO_READ : UIO_WRITE,
	    sc->sc_vp, bp->b_data, bp->b_bcount, off, UIO_SYSSPACE,
	    IO_NOCACHE | IO_SYNC | IO_NOLIMIT, sc->sc_ucred, &bp->b_resid, curproc);
	goto done;

fail:
	bp->b_error = EIO;
	bp->b_flags |= B_ERROR;
	bp->b_resid = bp->b_bcount;
done:
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

	error = rw_enter(&qd.qd_lock, RW_WRITE|RW_INTR);
	if (error != 0)
		goto freefname;

	error = qcow_insert(sc);
	if (error != 0)
		goto rollback;

	sc->sc_secsize = 1 << qc->qc_secbits;
	sc->sc_seccount = 1; // XXX TODO: derive seccount from header.

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
	lp->d_nsectors = 100; // XXX

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
