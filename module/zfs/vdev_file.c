/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2011, 2016 by Delphix. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/vdev_file.h>
#include <sys/vdev_impl.h>
#include <sys/vdev_trim.h>
#include <sys/zio.h>
#include <sys/fs/zfs.h>
#include <sys/fm/fs/zfs.h>
#include <sys/abd.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>

/*
 * Virtual device vector for files.
 */

static taskq_t *vdev_file_taskq;

static void
vdev_file_hold(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

static void
vdev_file_rele(vdev_t *vd)
{
	ASSERT(vd->vdev_path != NULL);
}

#ifdef _KERNEL
extern int VOP_GETATTR(struct vnode *vp, vattr_t *vap, int flags, void *x3, void *x4);
#endif

static int
vdev_file_open(vdev_t *vd, uint64_t *psize, uint64_t *max_psize,
    uint64_t *ashift)
{
#if _KERNEL
	static vattr_t vattr;
#endif
	vdev_file_t *vf;
	struct vnode *vp;
	int error = 0;
    struct vnode *rootdir;

    dprintf("vdev_file_open %p\n", vd->vdev_tsd);

	/*
	 * Rotational optimizations only make sense on block devices.
	 */
	vd->vdev_nonrot = B_TRUE;

	/*
	 * Allow TRIM on file based vdevs.  This may not always be supported,
	 * since it depends on your kernel version and underlying filesystem
	 * type but it is always safe to attempt.
	 */
	vd->vdev_has_trim = B_TRUE;

	/*
	 * Disable secure TRIM on file based vdevs.  There is no way to
	 * request this behavior from the underlying filesystem.
	 */
	vd->vdev_has_securetrim = B_FALSE;

	/*
	 * We must have a pathname, and it must be absolute.
	 */
	if (vd->vdev_path == NULL || vd->vdev_path[0] != '/') {
		vd->vdev_stat.vs_aux = VDEV_AUX_BAD_LABEL;
		return (SET_ERROR(EINVAL));
	}

	/*
	 * Reopen the device if it's not currently open.  Otherwise,
	 * just update the physical size of the device.
	 */
#ifdef _KERNEL
	if (vd->vdev_tsd != NULL) {
		ASSERT(vd->vdev_reopening);
		vf = vd->vdev_tsd;
        dprintf("skip to open\n");
		goto skip_open;
	}
#endif

	vf = vd->vdev_tsd = kmem_zalloc(sizeof (vdev_file_t), KM_SLEEP);

	/*
	 * We always open the files from the root of the global zone, even if
	 * we're in a local zone.  If the user has gotten to this point, the
	 * administrator has already decided that the pool should be available
	 * to local zone users, so the underlying devices should be as well.
	 */
	ASSERT(vd->vdev_path != NULL && vd->vdev_path[0] == '/');

    /*
      vn_openat(char *pnamep,
      enum uio_seg seg,
      int filemode,
      int createmode,
      struct vnode **vpp,
      enum create crwhy,
      mode_t umask,
      struct vnode *startvp)
      extern int vn_openat(char *pnamep, enum uio_seg seg, int filemode,
      int createmode, struct vnode **vpp, enum create crwhy,
      mode_t umask, struct vnode *startvp);
    */

    rootdir = getrootdir();

    error = vn_openat(vd->vdev_path + 1,
                      UIO_SYSSPACE,
                      spa_mode(vd->vdev_spa) | FOFFMAX,
                      0,
                      &vp,
                      0,
                      0,
                      rootdir
                      );

	if (error) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
		return (error);
	}

	vf->vf_vnode = vp;
#ifdef _KERNEL
    vf->vf_vid = vnode_vid(vp);

	/*
	 * Make sure it's a regular file.
	 */
	if (!vnode_isreg(vp)) {
        vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
        VN_RELE(vf->vf_vnode);
		vf->vf_vnode = NULL;
		return (SET_ERROR(ENODEV));
	}

	// Hold a long time reference
	vnode_ref(vp);

#endif

#if _KERNEL
skip_open:
	/*
	 * Determine the physical size of the file.
	 */
	vattr.va_mask = AT_SIZE;
    vn_lock(vf->vf_vnode, LK_SHARED | LK_RETRY);
	error = VOP_GETATTR(vf->vf_vnode, &vattr, 0, kcred, NULL);
    VN_UNLOCK(vf->vf_vnode);
#endif
	if (error) {
		vd->vdev_stat.vs_aux = VDEV_AUX_OPEN_FAILED;
        VN_RELE(vf->vf_vnode);
		vf->vf_vnode = NULL;
		return (error);
	}

#ifdef _KERNEL
	*max_psize = *psize = vattr.va_size;
#else
    /* userland's vn_open() will get the device size for us, so we can
     * just look it up - there is argument for a userland VOP_GETATTR to make
     * this function cleaner. */
	*max_psize = *psize = vp->v_size;
#endif
    *ashift = SPA_MINBLOCKSHIFT;

	// Keep the iocount while the file is open
	return (0);
}

static void
vdev_file_close(vdev_t *vd)
{
	vdev_file_t *vf = vd->vdev_tsd;

	if (vd->vdev_reopening || vf == NULL)
		return;

	if (vf->vf_vnode != NULL) {

		vnode_rele(vf->vf_vnode);

	// Also commented out in MacZFS
		//(void) VOP_PUTPAGE(vf->vf_vnode, 0, 0, B_INVAL, kcred, NULL);
		(void) VOP_CLOSE(vf->vf_vnode, spa_mode(vd->vdev_spa), 1, 0,
		    kcred, NULL);
	}

	vd->vdev_delayed_close = B_FALSE;
	kmem_free(vf, sizeof (vdev_file_t));
	vd->vdev_tsd = NULL;
}

/*
 * count the number of mismatches of zio->io_size and zio->io_abd->abd_size below
 */
_Atomic uint64_t zfs_vdev_file_size_mismatch_cnt = 0;

static void
vdev_file_io_strategy(void *arg)
{
	zio_t *zio = (zio_t *)arg;
	vdev_t *vd = zio->io_vd;
	vdev_file_t *vf = vd->vdev_tsd;
	ssize_t resid;
	void *data;

#ifdef DEBAG
	if (zio->io_abd->abd_size != zio->io_size) {
		zfs_vdev_file_size_mismatch_cnt++;

		// this dprintf can be very noisy
		dprintf("ZFS: %s: trimming zio->io_abd from 0x%x to 0x%llx\n",
		    __func__, zio->io_abd->abd_size, zio->io_size);
	}
#endif
	if (zio->io_type == ZIO_TYPE_READ) {
		ASSERT3S(zio->io_abd->abd_size,>=,zio->io_size);
		data =
			abd_borrow_buf(zio->io_abd, zio->io_abd->abd_size);
	} else {
		ASSERT3S(zio->io_abd->abd_size,>=,zio->io_size);
		data =
			abd_borrow_buf_copy(zio->io_abd, zio->io_abd->abd_size);
	}

	zio->io_error = vn_rdwr(zio->io_type == ZIO_TYPE_READ ?
	    UIO_READ : UIO_WRITE, vf->vf_vnode, data, zio->io_size,
	    zio->io_offset, UIO_SYSSPACE, 0, RLIM64_INFINITY, kcred, &resid);

	zio->io_error = (zio->io_error != 0 ? EIO : 0);

	if (zio->io_error == 0 && resid != 0)
		zio->io_error = SET_ERROR(ENOSPC);

	if (zio->io_type == ZIO_TYPE_READ) {
		if (zio->io_abd->abd_size == zio->io_size) {
			abd_return_buf_copy(zio->io_abd, data, zio->io_size);
		} else {
			VERIFY3S(zio->io_abd->abd_size,>=,zio->io_size);
			abd_return_buf_copy_off(zio->io_abd, data,
			    0, zio->io_size, zio->io_abd->abd_size);
		}
	} else {
		VERIFY3S(zio->io_abd->abd_size,>=,zio->io_size);
		abd_return_buf_off(zio->io_abd, data, 0, zio->io_size,
			zio->io_abd->abd_size);
	}

	zio_delay_interrupt(zio);
}

static void
vdev_file_io_start(zio_t *zio)
{
    vdev_t *vd = zio->io_vd;
    vdev_file_t *vf = vd->vdev_tsd;

    if (zio->io_type == ZIO_TYPE_IOCTL) {

        if (!vdev_readable(vd)) {
            zio->io_error = SET_ERROR(ENXIO);
			zio_interrupt(zio);
            return;
        }

        switch (zio->io_cmd) {
        case DKIOCFLUSHWRITECACHE:
			zio->io_error = VOP_FSYNC(vf->vf_vnode, FSYNC | FDSYNC,
			    kcred, NULL);
            break;
        default:
            zio->io_error = SET_ERROR(ENOTSUP);
        }

		zio_execute(zio);
		return;
	} else if (zio->io_type == ZIO_TYPE_TRIM) {
		struct flock flck;

		ASSERT3U(zio->io_size, !=, 0);
		bzero(&flck, sizeof (flck));
		flck.l_type = F_FREESP;
		flck.l_start = zio->io_offset;
		flck.l_len = zio->io_size;
		flck.l_whence = 0;

		zio->io_error = VOP_SPACE(vf->vf_vnode, F_FREESP, &flck,
		    0, 0, kcred, NULL);

		zio_execute(zio);
		return;
	}

	ASSERT(zio->io_type == ZIO_TYPE_READ || zio->io_type == ZIO_TYPE_WRITE);
	zio->io_target_timestamp = zio_handle_io_delay(zio);

	VERIFY3U(taskq_dispatch(system_taskq, vdev_file_io_strategy, zio,
        TQ_SLEEP), !=, 0);
}


/* ARGSUSED */
static void
vdev_file_io_done(zio_t *zio)
{
}

vdev_ops_t vdev_file_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	NULL,
	vdev_default_xlate,
	VDEV_TYPE_FILE,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

void
vdev_file_init(void)
{
	vdev_file_taskq = taskq_create("vdev_file_taskq", 100, minclsyspri,
	    max_ncpus, INT_MAX, TASKQ_PREPOPULATE | TASKQ_THREADS_CPU_PCT);

	VERIFY(vdev_file_taskq);
}

void
vdev_file_fini(void)
{
	taskq_destroy(vdev_file_taskq);
}

/*
 * From userland we access disks just like files.
 */
#ifndef _KERNEL

vdev_ops_t vdev_disk_ops = {
	vdev_file_open,
	vdev_file_close,
	vdev_default_asize,
	vdev_file_io_start,
	vdev_file_io_done,
	NULL,
	NULL,
	vdev_file_hold,
	vdev_file_rele,
	NULL,
	vdev_default_xlate,
	VDEV_TYPE_DISK,		/* name of this vdev type */
	B_TRUE			/* leaf vdev */
};

#endif
