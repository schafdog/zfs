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
 * Copyright (c) 2013-2017, Jorgen Lundman.  All rights reserved.
 */

#include <sys/types.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOBSD.h>
#include <IOKit/IOKitKeys.h>

#include <sys/zfs_ioctl.h>
#include <sys/zfs_znode.h>
#include <sys/zvol.h>

#include <sys/zvolIO.h>

#ifdef ZFS_BOOT
#include <sys/zfs_boot.h>
#include <sys/spa_impl.h>
#endif

#include <sys/ldi_osx.h>

#include <sys/zfs_vnops.h>
#include <sys/taskq.h>

#include <libkern/version.h>

#include <libkern/sysctl.h>


extern "C" {
	extern kern_return_t _start(kmod_info_t *ki, void *data);
	extern kern_return_t _stop(kmod_info_t *ki, void *data);

	__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(net.lundman.zfs, "1.0.0", _start, _stop)
	kmod_start_func_t *_realmain = 0;
	kmod_stop_func_t  *_antimain = 0;
	int _kext_apple_cc = __APPLE_CC__ ;
	extern uint64_t spl_initialised;
};

// Define the superclass.
#define super IOService

OSDefineMetaClassAndStructors(net_lundman_zfs_zvol, IOService)


/*
 * Some left over functions from zfs_osx.c, left as C until cleaned up
 */

extern "C" {

extern SInt32 zfs_active_fs_count;


#ifdef DEBUG
#define	ZFS_DEBUG_STR	" (DEBUG mode)"
#else
#define	ZFS_DEBUG_STR	""
#endif

static char kext_version[64] = ZFS_META_VERSION "-" ZFS_META_RELEASE ZFS_DEBUG_STR;

//struct sysctl_oid_list sysctl__zfs_children;
SYSCTL_DECL(_zfs);
SYSCTL_NODE( , OID_AUTO, zfs, CTLFLAG_RD, 0, "");
SYSCTL_STRING(_zfs, OID_AUTO, kext_version,
			  CTLFLAG_RD | CTLFLAG_LOCKED,
			  kext_version, 0, "ZFS KEXT Version");

#ifdef __APPLE__
extern int
zfs_vfs_sysctl(int *name, __unused u_int namelen, user_addr_t oldp, size_t *oldlenp,
               user_addr_t newp, size_t newlen, __unused vfs_context_t context)
{
#if 0
	int error;
	switch(name[0]) {
	case ZFS_SYSCTL_FOOTPRINT: {
		zfs_footprint_stats_t *footprint;
		size_t copyinsize;
		size_t copyoutsize;
		int max_caches;
		int act_caches;

		if (newp) {
			return (EINVAL);
		}
		if (!oldp) {
			*oldlenp = sizeof (zfs_footprint_stats_t);
			return (0);
		}
		copyinsize = *oldlenp;
		if (copyinsize < sizeof (zfs_footprint_stats_t)) {
			*oldlenp = sizeof (zfs_footprint_stats_t);
			return (ENOMEM);
		}
		footprint = kmem_alloc(copyinsize, KM_SLEEP);

		max_caches = copyinsize - sizeof (zfs_footprint_stats_t);
		max_caches += sizeof (kmem_cache_stats_t);
		max_caches /= sizeof (kmem_cache_stats_t);

		footprint->version = ZFS_FOOTPRINT_VERSION;

		footprint->memory_stats.current = zfs_footprint.current;
		footprint->memory_stats.target = zfs_footprint.target;
		footprint->memory_stats.highest = zfs_footprint.highest;
		footprint->memory_stats.maximum = zfs_footprint.maximum;

		arc_get_stats(&footprint->arc_stats);

		kmem_cache_stats(&footprint->cache_stats[0], max_caches, &act_caches);
		footprint->caches_count = act_caches;
		footprint->thread_count = zfs_threads;

		copyoutsize = sizeof (zfs_footprint_stats_t) +
		              ((act_caches - 1) * sizeof (kmem_cache_stats_t));

		error = ddi_copyout(footprint, oldp, copyoutsize, 0);

		kmem_free(footprint, copyinsize);

		return (error);
	    }

	case ZFS_SYSCTL_CONFIG_DEBUGMSG:
		error = sysctl_int(oldp, oldlenp, newp, newlen, &zfs_msg_buf_enabled);
		return error;

	case ZFS_SYSCTL_CONFIG_zdprintf:
#ifdef ZFS_DEBUG
		error = sysctl_int(oldp, oldlenp, newp, newlen, &zfs_zdprintf_enabled);
#else
		error = ENOTSUP;
#endif
		return error;
	}
#endif
	return (ENOTSUP);
}
#endif /* __APPLE__ */


#include <sys/utsname.h>
#include <string.h>


} // Extern "C"


bool
net_lundman_zfs_zvol::init (OSDictionary* dict)
{
	bool res;

	/* Need an OSSet for open clients */
	_openClients = OSSet::withCapacity(1);
	if (_openClients == NULL) {
		dprintf("client OSSet failed");
		return (false);
	}

	res = super::init(dict);

	//IOLog("ZFS::init\n");
	return res;
}

void
net_lundman_zfs_zvol::free (void)
{
	OSSafeReleaseNULL(_openClients);

	//IOLog("ZFS::free\n");
	super::free();
}

bool
net_lundman_zfs_zvol::isOpen(const IOService *forClient) const
{
	bool ret;
	ret = IOService::isOpen(forClient);
	return (ret);
}

bool
net_lundman_zfs_zvol::handleOpen(IOService *client,
    IOOptionBits options, void *arg)
{
	bool ret = true;

	dprintf("");
	//IOLog("ZFSPool %s\n", __func__);

	/* XXX IOService open() locks for arbitration around handleOpen */
	//lockForArbitration();
	_openClients->setObject(client);
	ret = _openClients->containsObject(client);
	//unlockForArbitration();

	return (ret);
//	return (IOService::handleOpen(client, options, NULL));
}

bool
net_lundman_zfs_zvol::handleIsOpen(const IOService *client) const
{
	bool ret;

	dprintf("");
	//IOLog("ZFSPool %s\n", __func__);

	/* XXX IOService isOpen() locks for arbitration around handleIsOpen */
	//lockForArbitration();
	ret = _openClients->containsObject(client);
	//unlockForArbitration();

	return (ret);
//	return (IOService::handleIsOpen(client));
}

void
net_lundman_zfs_zvol::handleClose(IOService *client,
    IOOptionBits options)
{
	dprintf("");
	//IOLog("ZFSPool %s\n", __func__);

	/* XXX IOService close() locks for arbitration around handleClose */
	//lockForArbitration();
	if (_openClients->containsObject(client) == false) {
		dprintf("not open");
	}
	/* Remove client from set */
	_openClients->removeObject(client);
	//unlockForArbitration();

//	IOService::handleClose(client, options);
}

IOService*
net_lundman_zfs_zvol::probe (IOService* provider, SInt32* score)
{
    IOService *res = super::probe(provider, score);
    //IOLog("ZFS::probe\n");
    return res;
}

static void zfs_start_continue(void *);

bool
net_lundman_zfs_zvol::start (IOService *provider)
{
    bool res = super::start(provider);

    IOLog("ZFS: Loading module ... \n");

	(void)thread_create(NULL, 0, zfs_start_continue, (void *)this, 0, 0, 0, 92);

	/* registerService() allows zconfigd to match against the service */
	this->registerService();

    /*
     * hostid is left as 0 on OSX, and left to be set if developers wish to
     * use it. If it is 0, we will hash the hardware.uuid into a 32 bit
     * value and set the hostid.
     */
    if (!zone_get_hostid(NULL)) {
      uint32_t myhostid = 0;
      IORegistryEntry *ioregroot =  IORegistryEntry::getRegistryRoot();
      if(ioregroot) {
        //IOLog("ioregroot is '%s'\n", ioregroot->getName(gIOServicePlane));
        IORegistryEntry *macmodel = ioregroot->getChildEntry(gIOServicePlane);
        if(macmodel) {
          //IOLog("macmodel is '%s'\n", macmodel->getName(gIOServicePlane));
          OSObject *ioplatformuuidobj;
          //ioplatformuuidobj = ioregroot->getProperty("IOPlatformUUID", gIOServicePlane, kIORegistryIterateRecursively);
          ioplatformuuidobj = macmodel->getProperty(kIOPlatformUUIDKey);
          if(ioplatformuuidobj) {
            OSString *ioplatformuuidstr = OSDynamicCast(OSString, ioplatformuuidobj);
            //IOLog("IOPlatformUUID is '%s'\n", ioplatformuuidstr->getCStringNoCopy());

            myhostid = fnv_32a_str(ioplatformuuidstr->getCStringNoCopy(),
                                   FNV1_32A_INIT);

            sysctlbyname("kern.hostid", NULL, NULL, &myhostid, sizeof(myhostid));
            printf("ZFS: hostid set to %08x from UUID '%s'\n",
                   myhostid, ioplatformuuidstr->getCStringNoCopy());
          }
        }
      }
    }

	return res;
}

static void zfs_start_continue(void *this_arg)
{
	while(spl_initialised == 0) {
		printf("ZFS: waiting for SPL init\n");
		delay(hz >> 1);
	}

    sysctl_register_oid(&sysctl__zfs);
    sysctl_register_oid(&sysctl__zfs_kext_version);

	/* Init LDI */
	int error = 0;
	error = ldi_init(NULL);
	if (error) {
		IOLog("%s ldi_init error %d\n", __func__, error);
		sysctl_unregister_oid(&sysctl__zfs_kext_version);
		sysctl_unregister_oid(&sysctl__zfs);
		thread_exit();
		/* XXX Needs to fail ZFS start */
	}

	/*
	 * Initialize /dev/zfs, this calls spa_init->dmu_init->arc_init-> etc
	 */
	zfs_ioctl_osx_init();

	///sysctl_register_oid(&sysctl__debug_maczfs);
	//sysctl_register_oid(&sysctl__debug_maczfs_stalk);

    zfs_vfsops_init();

    /*
     * When is the best time to start the system_taskq? It is strictly
     * speaking not used by SPL, but by ZFS. ZFS should really start it?
     */
    system_taskq_init();


#ifdef ZFS_BOOT
	zfs_boot_init((IOService *)this_arg);
#endif

	thread_exit();
}

void
net_lundman_zfs_zvol::stop (IOService *provider)
{
#ifdef ZFS_BOOT
	zfs_boot_fini();
#endif

    IOLog("ZFS: Attempting to unload ...\n");

    super::stop(provider);


    system_taskq_fini();

    zfs_ioctl_osx_fini();
    zfs_vfsops_fini();

	ldi_fini();

    sysctl_unregister_oid(&sysctl__zfs_kext_version);
    sysctl_unregister_oid(&sysctl__zfs);
    IOLog("ZFS: Unloaded module\n");

	/*
	 * There is no way to ensure all threads have actually got to the
	 * thread_exit() call, before we exit here (and XNU unloads all
	 * memory for the KEXT). So we increase the odds of that happening
	 * by delaying a little bit before we return to XNU. Quite possibly
	 * the worst "solution" but Apple has not given any good options.
	 */
	delay(hz*5);
}
