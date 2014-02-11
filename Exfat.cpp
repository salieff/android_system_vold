/*
 * Copyright (C) 2012 The Android Open Source Project
 * Copyright (C) 2013 The CyanogenMod Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/wait.h>

#include <linux/kdev_t.h>

#define LOG_TAG "Vold"

#include <cutils/log.h>
#include <cutils/properties.h>

#include "Exfat.h"

bool Exfat::moduleLoaded = false;

static char EXFAT_FSCK[] = "/system/bin/fsck.exfat";
static char EXFAT_MKFS[] = "/system/bin/mkfs.exfat";
static char INSMOD_PATH[] = "/system/bin/insmod";
static char EXFAT_MODULE[] = "/system/lib/modules/exfat.ko";

extern "C" int logwrap(int argc, const char **argv, int background);

int Exfat::doMount(const char *fsPath, const char *mountPoint,
                 bool ro, bool remount, bool executable,
                 int ownerUid, int ownerGid, int permMask, bool createLost) {
    if (!moduleLoaded)
    {
        int lkm_rc = loadModule();
        if (lkm_rc)
            return lkm_rc;

        moduleLoaded = true;
    }

    int rc;
    unsigned long flags;
    char mountData[255];

    flags = MS_NODEV | MS_NOSUID | MS_DIRSYNC;

    flags |= (executable ? 0 : MS_NOEXEC);
    flags |= (ro ? MS_RDONLY : 0);
    flags |= (remount ? MS_REMOUNT : 0);

    /*
     * Note: This is a temporary hack. If the sampling profiler is enabled,
     * we make the SD card world-writable so any process can write snapshots.
     *
     * TODO: Remove this code once we have a drop box in system_server.
     */
    char value[PROPERTY_VALUE_MAX];
    property_get("persist.sampling_profiler", value, "");
    if (value[0] == '1') {
        SLOGW("The SD card is world-writable because the"
            " 'persist.sampling_profiler' system property is set to '1'.");
        permMask = 0;
    }

    sprintf(mountData,
            "uid=%d,gid=%d,fmask=%o,dmask=%o",
            ownerUid, ownerGid, permMask, permMask);

    rc = mount(fsPath, mountPoint, "exfat", flags, mountData);

    if (rc && errno == EROFS) {
        SLOGE("%s appears to be a read only filesystem - retrying mount RO", fsPath);
        flags |= MS_RDONLY;
        rc = mount(fsPath, mountPoint, "exfat", flags, mountData);
    }

    if (rc == 0 && createLost) {
        char *lost_path;
        asprintf(&lost_path, "%s/LOST.DIR", mountPoint);
        if (access(lost_path, F_OK)) {
            /*
             * Create a LOST.DIR in the root so we have somewhere to put
             * lost cluster chains (fsck_msdos doesn't currently do this)
             */
            if (mkdir(lost_path, 0755)) {
                SLOGE("Unable to create LOST.DIR (%s)", strerror(errno));
            }
        }
        free(lost_path);
    }

    return rc;
}

int Exfat::check(const char *fsPath) {

    bool rw = true;
    int rc = -1;

    if (access(EXFAT_FSCK, X_OK)) {
        SLOGW("Skipping fs checks, exfatfsck not found.\n");
        return 0;
    }

    do {
        const char *args[3];
        args[0] = EXFAT_FSCK;
        args[1] = "-R";
        args[2] = fsPath;

        rc = logwrap(3, args, 1);

        switch(rc) {
        case 0:
            SLOGI("[Exfat::check] No errors\n");
            return 0;

        case 1:
            SLOGE("[Exfat::check] Device I/O error.\n");
            errno = EIO;
            return -1;

        case 2:
            SLOGE("[Exfat::check] Partition Boot Record currupted.\n");
            errno = EBADF;
            return -1;

        case 3:
            SLOGE("[Exfat::check] Invalid data structure.\n");
            errno = EINVAL;
            return -1;

        case 4:
            SLOGI("[Exfat::check] Filesystem was modified.\n");
            return 0;

        case 5:
            SLOGE("[Exfat::check] Device mounted.\n");
            errno = EBUSY;
            return -1;

        case 6:
            SLOGE("[Exfat::check] Device not mounted.\n");
            errno = ENOENT;
            return -1;

        case 7:
            SLOGE("[Exfat::check] Semaphore error.\n");
            errno = EIO;
            return -1;

        case 8:
            SLOGE("[Exfat::check] Invalid file name.\n");
            errno = EINVAL;
            return -1;

        case 9:
            SLOGE("[Exfat::check] Invalid file ID.\n");
            errno = EINVAL;
            return -1;

        case 10:
            SLOGE("[Exfat::check] Device not found.\n");
            errno = ENODEV;
            return -1;

        case 11:
            SLOGE("[Exfat::check] File exists.\n");
            errno = EEXIST;
            return -1;

        case 12:
            SLOGE("[Exfat::check] Permission error.\n");
            errno = EPERM;
            return -1;

        case 13:
            SLOGE("[Exfat::check] File not opened.\n");
            errno = EIO;
            return -1;

        case 14:
            SLOGE("[Exfat::check] Too many files opened.\n");
            errno = EMFILE;
            return -1;

        case 15:
            SLOGE("[Exfat::check] File system full.\n");
            errno = ENOSPC;
            return -1;

        case 16:
            SLOGE("[Exfat::check] End of file.\n");
            errno = EIO;
            return -1;

        case 17:
            SLOGE("[Exfat::check] Directory busy.\n");
            errno = EBUSY;
            return -1;

        case 18:
            SLOGE("[Exfat::check] Memory allocation failed.\n");
            errno = ENOMEM;
            return -1;

        case 19:
            SLOGE("[Exfat::check] File system size zero.\n");
            errno = EINVAL;
            return -1;

        case 20:
            SLOGE("[Exfat::check] Too few clusters.\n");
            errno = EIO;
            return -1;

        case 21:
            SLOGE("[Exfat::check] Too many clusters.\n");
            errno = EIO;
            return -1;

        case 22:
            SLOGE("[Exfat::check] File system corruption found.\n");
            errno = EBADF;
            return -1;

        case 23:
            SLOGE("[Exfat::check] Device not specified.\n");
            errno = ENODEV;
            return -1;

        case 24:
            SLOGE("[Exfat::check] Unknown options.\n");
            errno = EINVAL;
            return -1;

        default:
            SLOGE("exFAT filesystem check failed (unknown exit code %d).\n", rc);
            errno = EINVAL;
            return -1;
        }
    } while (0);

    return 0;
}

int Exfat::format(const char *fsPath) {
    const char *args[2];
    int rc = -1;

    if (access(EXFAT_MKFS, X_OK)) {
        SLOGE("Unable to format, mkexfatfs not found.");
        return -1;
    }

    args[0] = EXFAT_MKFS;
    args[1] = fsPath;

    rc = logwrap(2, args, 1);

    if (rc == 0) {
        SLOGI("Filesystem (exFAT) formatted OK");
        return 0;
    } else {
        SLOGE("Format (exFAT) failed (unknown exit code %d)", rc);
        errno = EIO;
        return -1;
    }
    return 0;
}

int Exfat::loadModule() {
    const char *args[2];
    int rc = -1;

    if (access(INSMOD_PATH, X_OK)) {
        SLOGE("Unable to load exfat.ko module, insmod not found.");
        return -1;
    }

    if (access(EXFAT_MODULE, R_OK)) {
        SLOGE("Unable to load exfat.ko module, exfat.ko not found.");
        return -1;
    }

    args[0] = INSMOD_PATH;
    args[1] = EXFAT_MODULE;

    rc = logwrap(2, args, 1);

    if (rc == 0) {
        SLOGI("exFAT module loaded OK");
        return 0;
    } else {
        SLOGE("Loading exFAT module failed (unknown exit code %d)", rc);
        errno = EIO;
        return -1;
    }
    return 0;
}
