/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License, Version 1.0 only
 * (the "License").  You may not use this file except in compliance
 * with the License.
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
 * Copyright 2005 Sun Microsystems, Inc.  All rights reserved.
 * Copyright 2006 Ricardo Correia.  All rights reserved.
 * Use is subject to license terms.
 */

/*	Copyright (c) 1988 AT&T	*/
/*	  All Rights Reserved	*/

#include <stdio.h>
#include <string.h>
#include <mntent.h>
#include <ctype.h> /* for isspace() */
#include <errno.h>
#include <unistd.h>
#include <sys/mnttab.h>
#include <sys/types.h>
#include <sys/vnode.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/fcntl.h>

#define DIFF(xx) ((mrefp->xx != NULL) && \
		  (mgetp->xx == NULL || strcmp(mrefp->xx, mgetp->xx) != 0))

static struct statfs *gsfs = NULL;
static int allfs = 0;
/*
 * We will also query the extended filesystem capabilities API, to lookup
 * other mount options, for example, XATTR. We can not use the MNTNOUSERXATTR
 * option due to VFS rejecting with EACCESS.
 */

#include <sys/attr.h>
typedef struct attrlist attrlist_t;

struct attrBufS {
	u_int32_t       length;
	vol_capabilities_set_t caps;
} __attribute__((aligned(4), packed));


DIR *
fdopendir(int fd)
{
	char fullpath[MAXPATHLEN];

	if (fcntl(fd, F_GETPATH, fullpath) < 0) {
		perror("fcntl");
		return (NULL);
	}
	if (close(fd) < 0) {
		return (NULL);
	}

	return (opendir(fullpath));
}

static int
chdir_block_begin(int newroot_fd)
{
	int cwdfd, error;

	cwdfd = open(".", O_RDONLY | O_DIRECTORY);
	if (cwdfd == -1)
		return (-1);

	if (fchdir(newroot_fd) == -1) {
		error = errno;
		(void) close(cwdfd);
		errno = error;
		return (-1);
	}
	return (cwdfd);
}

static void
chdir_block_end(int cwdfd)
{
	int error = errno;
	(void) fchdir(cwdfd);
	(void) close(cwdfd);
	errno = error;
}

int
openat64(int dirfd, const char *path, int flags, ...)
{
	int cwdfd, filefd;

	if ((cwdfd = chdir_block_begin(dirfd)) == -1)
		return (-1);

	if ((flags & O_CREAT) != 0) {
		va_list ap;
		int mode;

		va_start(ap, flags);
		mode = va_arg(ap, int);
		va_end(ap);

		filefd = open(path, flags, mode);
	} else
		filefd = open(path, flags);

	chdir_block_end(cwdfd);
	return (filefd);
}

int
mkdirat64(int dirfd, const char *path, mode_t mode)
{
	int cwdfd, newdirfd;

	if ((cwdfd = chdir_block_begin(dirfd)) == -1)
		return (-1);

	newdirfd = mkdir(path, mode);

	chdir_block_end(cwdfd);
	return (newdirfd);
}

int
fstatat64(int dirfd, const char *path, struct stat *statbuf, int flag)
{
	int cwdfd, error;

	if ((cwdfd = chdir_block_begin(dirfd)) == -1)
		return (-1);

	if (flag == AT_SYMLINK_NOFOLLOW)
		error = lstat(path, statbuf);
	else
		error = stat(path, statbuf);

	chdir_block_end(cwdfd);
	return (error);
}


static char *
mntopt(char **p)
{
	char *cp = *p;
	char *retstr;

	while (*cp && isspace(*cp))
		cp++;

	retstr = cp;
	while (*cp && *cp != ',')
		cp++;

	if (*cp) {
		*cp = '\0';
		cp++;
	}

	*p = cp;
	return (retstr);
}

char *
hasmntopt(struct mnttab *mnt, char *opt)
{
	char tmpopts[256];
	char *f, *opts = tmpopts;

	if (mnt->mnt_mntopts == NULL)
		return (NULL);
	(void) strlcpy(opts, mnt->mnt_mntopts, 256);
	f = mntopt(&opts);
	for (; *f; f = mntopt(&opts)) {
		if (strncmp(opt, f, strlen(opt)) == 0)
			return (f - tmpopts + mnt->mnt_mntopts);
	}
	return (NULL);
}

static void
optadd(char *mntopts, size_t size, const char *opt)
{

	if (mntopts[0] != '\0')
		strlcat(mntopts, ",", size);
	strlcat(mntopts, opt, size);
}


#include <CoreFoundation/CoreFoundation.h>
#include <CoreServices/CoreServices.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOMedia.h>
#include <IOKit/IOBSD.h>


char * MYCFStringCopyUTF8String(CFStringRef aString)
{
  if (aString == NULL) {
    return NULL;
  }

  CFIndex length = CFStringGetLength(aString);
  CFIndex maxSize =
  CFStringGetMaximumSizeForEncoding(length,
                                    kCFStringEncodingUTF8);
  char *buffer = (char *)malloc(maxSize);
  if (CFStringGetCString(aString, buffer, maxSize,
                         kCFStringEncodingUTF8)) {
    return buffer;
  }
  return NULL;
}

/*
 * Given "/dev/disk6" connect to IOkit and fetch the dataset
 * name "BOOM/lower", and use it instead.
 */
void expand_disk_to_zfs(char *devname, int len)
{
	char *result = NULL;
	CFMutableDictionaryRef matchingDict;
	io_service_t service;
	CFStringRef cfstr;
	char *device;

	if (strncmp(devname, "/dev/disk", 9) != 0)
		return;

	device = &devname[5];

	matchingDict = IOBSDNameMatching(kIOMasterPortDefault, 0, device);
	if (NULL == matchingDict)
		return;

	/*
	 * Fetch the object with the matching BSD node name.
	 * Note that there should only be one match, so
	 * IOServiceGetMatchingService is used instead of
	 * IOServiceGetMatchingServices to simplify the code.
	 */
	service = IOServiceGetMatchingService(kIOMasterPortDefault,
										  matchingDict);

	if (IO_OBJECT_NULL == service) {
		return;
	}

	cfstr = IORegistryEntryCreateCFProperty(service,
		CFSTR("ZFS Dataset"), kCFAllocatorDefault, 0);
	if (cfstr) {
		result = MYCFStringCopyUTF8String(cfstr);
		CFRelease(cfstr);
	}

	IOObjectRelease(service);

	if (result) {
		strlcpy(devname, result, len);
		free(result);
	}
}



void
statfs2mnttab(struct statfs *sfs, struct mnttab *mp)
{
	static char mntopts[MNTMAXSTR];
	long flags;

	mntopts[0] = '\0';

	flags = sfs->f_flags;
#define	OPTADD(opt)	optadd(mntopts, sizeof(mntopts), (opt))
	if (flags & MNT_RDONLY)
		OPTADD(MNTOPT_RO);
	else
		OPTADD(MNTOPT_RW);
	if (flags & MNT_NOSUID)
#ifdef __FreeBSD__
		OPTADD(MNTOPT_NOSUID);
#elif defined(__APPLE__)
		OPTADD(MNTOPT_NOSETUID);
#endif
	else
		OPTADD(MNTOPT_SETUID);
	if (flags & MNT_UPDATE)
		OPTADD(MNTOPT_REMOUNT);
	if (flags & MNT_NOATIME)
		OPTADD(MNTOPT_NOATIME);
	else
		OPTADD(MNTOPT_ATIME);
		{
			struct attrBufS attrBuf;
			attrlist_t      attrList;

			memset(&attrList, 0, sizeof(attrList));
			attrList.bitmapcount = ATTR_BIT_MAP_COUNT;
			attrList.volattr = ATTR_VOL_INFO|ATTR_VOL_CAPABILITIES;

			if (getattrlist(sfs->f_mntonname, &attrList, &attrBuf,
							sizeof(attrBuf), 0) == 0)  {

				if (attrBuf.caps[VOL_CAPABILITIES_INTERFACES] &
					VOL_CAP_INT_EXTENDED_ATTR) {
					OPTADD(MNTOPT_XATTR);
				} else {
					OPTADD(MNTOPT_NOXATTR);
				} // If EXTENDED
			} // if getattrlist
		}
	if (flags & MNT_NOEXEC)
		OPTADD(MNTOPT_NOEXEC);
	else
		OPTADD(MNTOPT_EXEC);
	if (flags & MNT_NODEV)
		OPTADD(MNTOPT_NODEVICES);
	else
		OPTADD(MNTOPT_DEVICES);
	if (flags & MNT_DONTBROWSE)
		OPTADD(MNTOPT_NOBROWSE);
	else
		OPTADD(MNTOPT_BROWSE);
	if (flags & MNT_IGNORE_OWNERSHIP)
		OPTADD(MNTOPT_NOOWNERS);
	else
		OPTADD(MNTOPT_OWNERS);

#undef	OPTADD

	// If a disk is /dev/diskX, lets see if it has "zfs_dataset_name"
	// set, and if so, use it instead, for mount matching.
	expand_disk_to_zfs(sfs->f_mntfromname, sizeof(sfs->f_mntfromname));



	mp->mnt_special = sfs->f_mntfromname;
	mp->mnt_mountp = sfs->f_mntonname;
	mp->mnt_fstype = sfs->f_fstypename;
	mp->mnt_mntopts = mntopts;
	mp->mnt_fssubtype = sfs->f_fssubtype;

}

static int
statfs_init(void)
{
	struct statfs *sfs;
	int error;

	if (gsfs != NULL) {
		free(gsfs);
		gsfs = NULL;
	}
	allfs = getfsstat(NULL, 0, MNT_NOWAIT);
	if (allfs == -1)
		goto fail;
	gsfs = malloc(sizeof(gsfs[0]) * allfs * 2);
	if (gsfs == NULL)
		goto fail;
	allfs = getfsstat(gsfs, (long)(sizeof(gsfs[0]) * allfs * 2),
                      MNT_NOWAIT);
	if (allfs == -1)
		goto fail;
	sfs = realloc(gsfs, allfs * sizeof(gsfs[0]));
	if (sfs != NULL)
		gsfs = sfs;
	return (0);
fail:
	error = errno;
	if (gsfs != NULL)
		free(gsfs);
	gsfs = NULL;
	allfs = 0;
	return (error);
}

int
getmntany(FILE *fd __unused, struct mnttab *mgetp, struct mnttab *mrefp)
{
	int i, error;

	error = statfs_init();
	if (error != 0)
		return (error);

	for (i = 0; i < allfs; i++) {
		statfs2mnttab(&gsfs[i], mgetp);
		if (mrefp->mnt_special != NULL && mgetp->mnt_special != NULL &&
		    strcmp(mrefp->mnt_special, mgetp->mnt_special) != 0) {
			continue;
		}
		if (mrefp->mnt_mountp != NULL && mgetp->mnt_mountp != NULL &&
		    strcmp(mrefp->mnt_mountp, mgetp->mnt_mountp) != 0) {
			continue;
		}
		if (mrefp->mnt_fstype != NULL && mgetp->mnt_fstype != NULL &&
		    strcmp(mrefp->mnt_fstype, mgetp->mnt_fstype) != 0) {
			continue;
		}
		return (0);
	}
	return (-1);
}

int
getmntent(FILE *fp, struct mnttab *mp)
{
	static int index = -1;
	int error = 0;

	if (index < 0) {
		error = statfs_init();
	}

	if (error != 0)
		return (error);

	index++;

	// If we have finished "reading" the mnttab, reset it to
	// start from the beginning, and return EOF.
	if (index >= allfs) {
		index = -1;
		return -1;
	}

	statfs2mnttab(&gsfs[index], mp);
	return (0);
}
