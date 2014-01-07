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
        args[1] = fsPath;
        args[2] = NULL;

        rc = logwrap(3, args, 1);

        switch(rc) {
        case 0:
            SLOGI("exFAT filesystem check completed OK.\n");
            return 0;
        case 1:
            SLOGI("exFAT filesystem check completed, errors corrected OK.\n");
            return 0;
        case 2:
            SLOGE("exFAT filesystem check completed, errors corrected, need reboot.\n");
            return 0;
        case 4:
            SLOGE("exFAT filesystem errors left uncorrected.\n");
            return 0;
        case 8:
            SLOGE("exfatfsck operational error.\n");
            errno = EIO;
            return -1;
        default:
            SLOGE("exFAT filesystem check failed (unknown exit code %d).\n", rc);
            errno = EIO;
            return -1;
        }
    } while (0);

    return 0;
}

int Exfat::format(const char *fsPath) {
    const char *args[3];
    int rc = -1;

    if (access(EXFAT_MKFS, X_OK)) {
        SLOGE("Unable to format, mkexfatfs not found.");
        return -1;
    }

    args[0] = EXFAT_MKFS;
    args[1] = fsPath;
    args[2] = NULL;

    rc = logwrap(3, args, 1);

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
    const char *args[3];
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
    args[2] = NULL;

    rc = logwrap(3, args, 1);

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
