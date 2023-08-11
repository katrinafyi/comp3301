/*	$OpenBSD$ */

/*
 * Copyright (c) 2015, 2023 David Gwynne <dlg@uq.edu.au>
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

#include "sys/resource.h"
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/time.h>
#include <sys/zones.h>
#include <sys/resourcevar.h>

#include <sys/socket.h>
#include <sys/mount.h>
#include <sys/syscallargs.h>

#include <sys/types.h>
#include <sys/errno.h>

#include <sys/malloc.h>
#include <sys/pool.h>
#include <sys/rwlock.h>
#include <sys/atomic.h>
#include <sys/tree.h>


/* zusage arithmetic functions. */
void zone_zuzero(struct zusage *);
void zone_zuadd(struct zusage *, const struct zusage *);
void zone_zusub(struct zusage *, const struct zusage *);
void zone_getzusage(struct process *, struct zusage *);

struct zone {
	zoneid_t	 z_id;
	struct refcnt	 z_refs;
	char		*z_name;
	size_t		 z_namelen;

	struct rwlock 	 z_rwlock;	/* lock to protect fields marked [rw]. */
	struct zusage    z_contra;	/* [rw] contra for accurate accounting across zone_enter(2) */  

	RBT_ENTRY(zone)	 z_nm_entry;
	RBT_ENTRY(zone)	 z_id_entry;
};

/*
 * for the interaction of z_refs and z_rwlock, we require that
 * having a reference (i.e. z_refs incremented) is a precondition to
 * acquiring the z_rwlock. additionally, a process must release the
 * z_rwlock before yielding its reference (i.e. decrementing z_refs).
 *
 * this ensures that if z_refs is at the last reference, z_rwlock is 
 * not locked and it is safe to delete.
 */


static struct zone zone_global = {
	.z_id		= 0,
	.z_refs		= REFCNT_INITIALIZER(),
	.z_name		= "global",
	.z_namelen	= sizeof("global"),

	.z_rwlock	= RWLOCK_INITIALIZER("zone_global"),
	.z_contra	= (struct zusage){0},
};

struct zone * const global_zone = &zone_global;

RBT_HEAD(zone_id_tree, zone);
RBT_HEAD(zone_nm_tree, zone);

struct zones {
	struct rwlock		zs_lock;
	struct zone_id_tree	zs_id_tree;
	struct zone_nm_tree	zs_nm_tree;
	struct pool		zs_pool;
};

static struct zones zones = {
	.zs_lock	= RWLOCK_INITIALIZER("zones"),
	.zs_id_tree	= RBT_INITIALIZER(&zones.zs_id_tree),
	.zs_nm_tree	= RBT_INITIALIZER(&zones.zs_nm_tree),
};

static inline int
zone_id_cmp(const struct zone *a, const struct zone *b)
{
	if (a->z_id < b->z_id)
		return (-1);
	if (a->z_id > b->z_id)
		return (1);
	return (0);
}

RBT_PROTOTYPE(zone_id_tree, zone, z_id_entry, zone_id_cmp);

static inline int
zone_nm_cmp(const struct zone *a, const struct zone *b)
{
	return (strcmp(a->z_name, b->z_name));
}

RBT_PROTOTYPE(zone_nm_tree, zone, z_nm_entry, zone_nm_cmp);

int	zone_global_list(zoneid_t **, size_t *);

void
zone_boot(void)
{
	pool_init(&zones.zs_pool, sizeof(struct zone), 0, IPL_NONE, PR_WAITOK,
	    "zonepl", NULL);
	pool_sethardlimit(&zones.zs_pool, MAXZONES - 1,
	    "zones limit reached", 1);

	global_zone->z_namelen = strlen(global_zone->z_name);
	/* the trees own the references */
	RBT_INSERT(zone_id_tree, &zones.zs_id_tree, global_zone);
	RBT_INSERT(zone_nm_tree, &zones.zs_nm_tree, global_zone);
}

int
sys_zone_create(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_create_args /* {
		const char *zonename;
	} */ *uap = v;
	struct zone *zone;
	char zonename[MAXZONENAMELEN];
	size_t zonenamelen;
	int rv;

	*retval = -1;

	if (p->p_p->ps_zone != global_zone || suser(p) != 0)
		return (EPERM);

	rv = copyinstr(SCARG(uap, zonename), zonename, sizeof(zonename),
	    &zonenamelen);
	if (rv != 0)
		return (rv);

	zone = pool_get(&zones.zs_pool, PR_WAITOK | PR_LIMITFAIL);
	if (zone == NULL)
		return (ERANGE);

	refcnt_init(&zone->z_refs); /* starts at 1 */
	zone->z_namelen = zonenamelen;
	zone->z_name = malloc(zone->z_namelen, M_DEVBUF, M_WAITOK);
	memcpy(zone->z_name, zonename, zone->z_namelen);

	rw_init(&zone->z_rwlock, zone->z_name);
	zone_zuzero(&zone->z_contra);

	rv = rw_enter(&zones.zs_lock, RW_WRITE|RW_INTR);
	if (rv != 0)
		goto free;

	if (RBT_INSERT(zone_nm_tree, &zones.zs_nm_tree, zone) != NULL) {
		rw_exit(&zones.zs_lock);
		rv = EEXIST;
		goto free;
	}

	do {
		zone->z_id = arc4random_uniform(MAXZONEIDS);
	} while (RBT_INSERT(zone_id_tree, &zones.zs_id_tree, zone) != NULL);

	*retval = zone->z_id;
	rw_exit(&zones.zs_lock);

	return (0);

free:
	free(zone->z_name, M_DEVBUF, zone->z_namelen);
	pool_put(&zones.zs_pool, zone);
	return (rv);
}

static struct zone *
zone_lookup(zoneid_t z)
{
	struct zone key;
	struct zone *zone;

	key.z_id = z;

	rw_enter_read(&zones.zs_lock);
	zone = RBT_FIND(zone_id_tree, &zones.zs_id_tree, &key);
	if (zone != NULL)
		zone_ref(zone);
	rw_exit_read(&zones.zs_lock);

	return (zone);
}

struct zone *
zone_ref(struct zone *zone)
{
	refcnt_take(&zone->z_refs);
	return (zone);
}

/**
 * Perform zone_ref for a fork(2) operation, incrementing the statistics.
 * The caller SHOULD have a refcnt to the zone.
 */
void
zone_addfork(struct zone *zone)
{
	rw_enter_write(&zone->z_rwlock);
	zone->z_contra.zu_forks++;
	rw_exit_write(&zone->z_rwlock);
}


void
zone_unref(struct zone *zone)
{
	int last = refcnt_rele(&zone->z_refs);
	if (last)
		panic("%s: last zone %p reference released", __func__, zone);
}

int
zone_visible(struct process *self, struct process *target)
{
	struct zone *zone = self->ps_zone;

	return (zone == global_zone || zone == target->ps_zone);
}

zoneid_t
zone_id(const struct zone *zone)
{
	return (zone->z_id);
}

int
sys_zone_destroy(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_destroy_args /* {
		zoneid_t z;
	} */ *uap = v;
	struct zone key;
	struct zone *zone;
	int rv;

	*retval = -1;
	if (p->p_p->ps_zone != global_zone || suser(p) != 0)
		return (EPERM);

	key.z_id = SCARG(uap, z);

	rw_enter_write(&zones.zs_lock);
	zone = RBT_FIND(zone_id_tree, &zones.zs_id_tree, &key);
	if (zone == NULL) {
		rv = ESRCH;
		goto fail;
	}

	if (!refcnt_rele_last(&zone->z_refs)) {
		rv = EBUSY;
		goto fail;
	}
	rw_assert_unlocked(&zone->z_rwlock);

	RBT_REMOVE(zone_nm_tree, &zones.zs_nm_tree, zone);
	RBT_REMOVE(zone_id_tree, &zones.zs_id_tree, zone);
	rw_exit_write(&zones.zs_lock);

	free(zone->z_name, M_DEVBUF, zone->z_namelen);
	pool_put(&zones.zs_pool, zone);

	*retval = 0;
	return (0);

fail:
	rw_exit_write(&zones.zs_lock);
	return (rv);
}

int
sys_zone_enter(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_enter_args /* {
		zoneid_t z;
	} */ *uap = v;
	struct zone *newzone;
	struct zusage zu;

	*retval = -1;
	if (p->p_p->ps_zone != global_zone || suser(p) != 0)
		return (EPERM);

	newzone = zone_lookup(SCARG(uap, z));
	if (newzone == NULL)
		return (ESRCH);

	if (atomic_cas_ptr(&p->p_p->ps_zone, global_zone, newzone) !=
	    global_zone) {
		zone_unref(newzone);
		return (EPERM);
	}
	/* we're giving the zone_lookup ref to this process */

	zone_getzusage(p->p_p, &zu);

	/* 
	 * the moved process's current stats are added to the global zone
	 * and decremented from the newzone. this maintains correct bookkeeping 
	 * because they cancel to zero.
	 */
	rw_enter_write(&global_zone->z_rwlock);
	rw_enter_write(&newzone->z_rwlock);

	zone_zuadd(&global_zone->z_contra, &zu);
	newzone->z_contra.zu_enters++;
	zone_zusub(&newzone->z_contra, &zu);

	rw_exit_write(&newzone->z_rwlock);
	rw_exit_write(&global_zone->z_rwlock);

	zone_unref(global_zone); /* drop gz ref */

	*retval = 0;
	return (0);
}

int
zone_global_list(zoneid_t **zsp, size_t *nzsp)
{
	struct zone *zone;
	zoneid_t *zs;
	size_t nzs;
	size_t i = 0;

	/* sneaking info off the pool is naughty */
	nzs = zones.zs_pool.pr_nout + 1; /* count the gz */
	if (nzs > *nzsp)
		return (ERANGE);

	zs = mallocarray(nzs, sizeof(*zs), M_TEMP, M_WAITOK);

	RBT_FOREACH(zone, zone_id_tree, &zones.zs_id_tree)
		zs[i++] = zone->z_id;

	KASSERT(i == nzs);

	*zsp = zs;
	*nzsp = nzs;

	return (0);
}

int
sys_zone_list(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_list_args /* {
		zone_t *zs;
		size_t *nzs;
	} */ *uap = v;

	struct zone *zone;
	zoneid_t *zs;
	zoneid_t *zsp = SCARG(uap, zs);
	size_t nzs;
	size_t *nzsp = SCARG(uap, nzs);
	int rv;

	*retval = -1;

	zone = p->p_p->ps_zone;

	rv = copyin(nzsp, &nzs, sizeof(nzs));
	if (rv != 0)
		return (rv);

	if (zone == global_zone) {
		rw_enter_read(&zones.zs_lock);
		rv = zone_global_list(&zs, &nzs);
		rw_exit_read(&zones.zs_lock);
		if (rv != 0)
			return (rv);

		rv = copyout(zs, zsp, nzs * sizeof(zoneid_t));
		free(zs, M_TEMP, nzs * sizeof(zoneid_t));
		if (rv != 0)
			return (rv);
	} else {
		if (nzs < 1)
			return (ERANGE);
		nzs = 1;
		rv = copyout(&zone->z_id, zsp, sizeof(zoneid_t));
		if (rv != 0)
			return (rv);
	}

	rv = copyout(&nzs, nzsp, sizeof(nzs));
	if (rv != 0)
		return (rv);

	*retval = 0;
	return (0);
}

int
sys_zone_name(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_name_args /* {
		zoneid_t z;
		char *name;
		size_t namelen;
	} */ *uap = v;
	struct zone *zone;
	zoneid_t z;
	int rv;

	*retval = -1;

	zone = p->p_p->ps_zone;
	z = SCARG(uap, z);

	if (zone == global_zone) {
		zone = zone_lookup(z);
		if (zone == NULL)
			return (ESRCH);
	} else if (zone->z_id != z)
		return (ESRCH);
	else
		zone_ref(zone);

	rv = copyoutstr(zone->z_name,
	    SCARG(uap, name), SCARG(uap, namelen), NULL);
	zone_unref(zone);
	if (rv != 0)
		return (EFAULT);

	*retval = 0;
	return (0);
}

int
sys_zone_id(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_id_args /* {
		const char *zonename;
	} */ *uap = v;
	char zonename[MAXZONENAMELEN];
	size_t zonenamelen;
	struct zone *zone;
	struct zone key = { .z_name = zonename };
	int rv;

	zone = p->p_p->ps_zone;

	/* NULL zone name means current zone */
	if (SCARG(uap, zonename) == NULL) {
		*retval = zone->z_id;
		return (0);
	}

	*retval = -1;

	rv = copyinstr(SCARG(uap, zonename), zonename,
	    sizeof(zonename), &zonenamelen);
	if (rv != 0)
		return (rv);

	/* short cuts for non-gz */
	if (zone != global_zone) {
		if (strcmp(zone->z_name, zonename) != 0)
			return (ESRCH);

		*retval = zone->z_id;
		return (0);
	}

	rv = rw_enter(&zones.zs_lock, RW_READ|RW_INTR);
	if (rv != 0)
		return (rv);

	zone = RBT_FIND(zone_nm_tree, &zones.zs_nm_tree, &key);
	if (zone == NULL)
		rv = ESRCH;
	else
		*retval = zone->z_id;

	rw_exit(&zones.zs_lock);

	return (rv);
}

void
zone_zuzero(struct zusage *zu)
{
	memset(zu, 0, sizeof(*zu));
}

void
zone_zuadd(struct zusage *zu, const struct zusage *zu2)
{
	timeradd(&zu2->zu_utime, &zu->zu_utime, &zu->zu_utime); 
	timeradd(&zu2->zu_stime, &zu->zu_stime, &zu->zu_stime); 

	zu->zu_minflt += zu2->zu_minflt;
	zu->zu_majflt += zu2->zu_majflt;
	zu->zu_nswaps += zu2->zu_nswaps;
	zu->zu_inblock += zu2->zu_inblock;
	zu->zu_oublock += zu2->zu_oublock;
	zu->zu_msgsnd += zu2->zu_msgsnd;
	zu->zu_msgrcv += zu2->zu_msgrcv;
	zu->zu_nvcsw += zu2->zu_nvcsw;
	zu->zu_nivcsw += zu2->zu_nivcsw;
	zu->zu_enters += zu2->zu_enters;
	zu->zu_forks += zu2->zu_forks;
	zu->zu_nprocs += zu2->zu_nprocs;
}


void
zone_zusub(struct zusage *zu, const struct zusage *zu2)
{
	timersub(&zu2->zu_utime, &zu->zu_utime, &zu->zu_utime); 
	timersub(&zu2->zu_stime, &zu->zu_stime, &zu->zu_stime); 

	zu->zu_minflt -= zu2->zu_minflt;
	zu->zu_majflt -= zu2->zu_majflt;
	zu->zu_nswaps -= zu2->zu_nswaps;
	zu->zu_inblock -= zu2->zu_inblock;
	zu->zu_oublock -= zu2->zu_oublock;
	zu->zu_msgsnd -= zu2->zu_msgsnd;
	zu->zu_msgrcv -= zu2->zu_msgrcv;
	zu->zu_nvcsw -= zu2->zu_nvcsw;
	zu->zu_nivcsw -= zu2->zu_nivcsw;
	zu->zu_enters -= zu2->zu_enters;
	zu->zu_forks -= zu2->zu_forks;
	zu->zu_nprocs -= zu2->zu_nprocs;
}


/**
 * Convert a single process's rusage into a partial zusage (without enters, forks, and nprocs).
 */
void
zone_rusage_to_zusage(const struct rusage *ru, struct zusage *zu)
{
	zone_zuzero(zu);
	zu->zu_utime = ru->ru_utime; 
	zu->zu_stime = ru->ru_stime;

	zu->zu_minflt = ru->ru_minflt;
	zu->zu_majflt = ru->ru_majflt;
	zu->zu_nswaps = ru->ru_nswap; /* typo in spec? */
	zu->zu_inblock = ru->ru_inblock;
	zu->zu_oublock = ru->ru_oublock;
	zu->zu_msgsnd = ru->ru_msgsnd;
	zu->zu_msgrcv = ru->ru_msgrcv;
	zu->zu_nvcsw = ru->ru_nvcsw;
	zu->zu_nivcsw = ru->ru_nivcsw;
	zu->zu_enters = 0;
	zu->zu_forks = 0;
	zu->zu_nprocs = 0;
}


void
zone_getzusage(struct process *pr, struct zusage *zup)
{
	struct rusage ru;
	struct rusage *rup = &ru;

	/* the following is copied from kern_resource.c:dogetrusage */
	struct proc *q;
	
	/* start with the sum of dead threads, if any */
	if (pr->ps_ru != NULL)
		*rup = *pr->ps_ru;
	else
		memset(rup, 0, sizeof(*rup));

	/* add on all living threads */
	TAILQ_FOREACH(q, &pr->ps_threads, p_thr_link) {
		ruadd(rup, &q->p_ru);
		tuagg(pr, q);
	}

	calcru(&pr->ps_tu, &rup->ru_utime, &rup->ru_stime, NULL);

	zone_rusage_to_zusage(&ru, zup);
}

int
sys_zone_stats(struct proc *p, void *v, register_t *retval)
{
	struct sys_zone_stats_args /* {
	  zoneid_t z;
	  struct zusage *zu;
	  size_t *zulen;
	} */ *uap = v;
	struct zone *zone;
	zoneid_t z;
	int rv;

	*retval = -1;

	zone = p->p_p->ps_zone;
	z = SCARG(uap, z);

	/* if process is gz, we may lookup others. */
	if (zone == global_zone) {
		zone = zone_lookup(z);
		if (zone == NULL)
			return (ESRCH);
	} else if (zone->z_id != z)
		/* if not global and not equal, we do not have permission. */
		return (ESRCH);
	else
		zone_ref(zone);

	/* now, zone is the one we're interested in and we have a ref. query it. */

	struct zusage zu, zu2;

	struct process *pr;
	rw_enter_write(&zone->z_rwlock);
	zu = zone->z_contra;
	rw_exit_write(&zone->z_rwlock);
	KASSERT(zu.zu_nprocs == 0);

	/* this is probably fast enough, since ps(1) does this internally as well */
	LIST_FOREACH(pr, &allprocess, ps_list) {
		if (pr->ps_flags & PS_SYSTEM)
			continue;
		if (zone != global_zone && pr->ps_zone != zone)
			continue;
		zone_getzusage(pr, &zu2);
		zone_zuadd(&zu, &zu2);
		zu.zu_nprocs++;
	}
	LIST_FOREACH(pr, &zombprocess, ps_list) {
		if (pr->ps_flags & PS_SYSTEM)
			continue;
		if (zone != global_zone && pr->ps_zone != zone)
			continue;
		zone_getzusage(pr, &zu2);
		zone_zuadd(&zu, &zu2);
	}

	rv = copyout(&zu, SCARG(uap, zu), sizeof(zu));
	zone_unref(zone);
	if (rv != 0)
		return (rv);

	*retval = 0;
	return (0);
}

RBT_GENERATE(zone_id_tree, zone, z_id_entry, zone_id_cmp);
RBT_GENERATE(zone_nm_tree, zone, z_nm_entry, zone_nm_cmp);
