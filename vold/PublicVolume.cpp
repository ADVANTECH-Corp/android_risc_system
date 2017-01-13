/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "fs/Vfat.h"
#include "fs/Ntfs.h"
#include "PublicVolume.h"
#include "Utils.h"
#include "VolumeManager.h"
#include "ResponseCode.h"

#include <base/stringprintf.h>
#include <base/logging.h>
#include <cutils/fs.h>
#include <private/android_filesystem_config.h>

#include <fcntl.h>
#include <stdlib.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <cutils/log.h>

#define CONFIG_UEVENT_PROC

#ifdef CONFIG_UEVENT_PROC
#include <cutils/properties.h>

#define OFFLINE_OTAPROC
#define STARTUP_SERVICE
#define CUSTCFG_UPGRADE
#endif

using android::base::StringPrintf;

namespace android {
namespace vold {

static const char* kFusePath = "/system/bin/sdcard";

static const char* kAsecPath = "/mnt/secure/asec";

PublicVolume::PublicVolume(dev_t device) :
        VolumeBase(Type::kPublic), mDevice(device), mFusePid(0) {
    setId(StringPrintf("public:%u:%u", major(device), minor(device)));
    mDevPath = StringPrintf("/dev/block/vold/%s", getId().c_str());
}

PublicVolume::~PublicVolume() {
}

status_t PublicVolume::readMetadata() {
    status_t res = ReadMetadataUntrusted(mDevPath, mFsType, mFsUuid, mFsLabel);
    notifyEvent(ResponseCode::VolumeFsTypeChanged, mFsType);
    notifyEvent(ResponseCode::VolumeFsUuidChanged, mFsUuid);
    notifyEvent(ResponseCode::VolumeFsLabelChanged, mFsLabel);
    return res;
}

status_t PublicVolume::initAsecStage() {
    std::string legacyPath(mRawPath + "/android_secure");
    std::string securePath(mRawPath + "/.android_secure");

    // Recover legacy secure path
    if (!access(legacyPath.c_str(), R_OK | X_OK)
            && access(securePath.c_str(), R_OK | X_OK)) {
        if (rename(legacyPath.c_str(), securePath.c_str())) {
            PLOG(WARNING) << getId() << " failed to rename legacy ASEC dir";
        }
    }

    if (TEMP_FAILURE_RETRY(mkdir(securePath.c_str(), 0700))) {
        if (errno != EEXIST) {
            PLOG(WARNING) << getId() << " creating ASEC stage failed";
            return -errno;
        }
    }

    BindMount(securePath, kAsecPath);

    return OK;
}

status_t PublicVolume::doCreate() {
    return CreateDeviceNode(mDevPath, mDevice);
}

status_t PublicVolume::doDestroy() {
    return DestroyDeviceNode(mDevPath);
}

status_t PublicVolume::doMount() {
    // TODO: expand to support mounting other filesystems
    int ntfs = 0;
    readMetadata();
    if (mFsType != "vfat" && mFsType != "ntfs") {
		LOG(ERROR) << getId() << " unsupported filesystem " << mFsType;
        	return -EIO;
    }

    if (vfat::Check(mDevPath)) {
	/* try the NTFS filesystem */
        if (!Ntfs::check(mDevPath)) {
                ntfs = 1;
        } else {
                LOG(ERROR) << getId() << " failed filesystem check ";
                return -EIO;
        }

    }

    // Use UUID as stable name, if available
    std::string stableName = getId();
    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }

    mRawPath = StringPrintf("/mnt/media_rw/%s", stableName.c_str());

    mFuseDefault = StringPrintf("/mnt/runtime/default/%s", stableName.c_str());
    mFuseRead = StringPrintf("/mnt/runtime/read/%s", stableName.c_str());
    mFuseWrite = StringPrintf("/mnt/runtime/write/%s", stableName.c_str());

#ifdef OFFLINE_OTAPROC
    std::string updatePath(mRawPath + "/OTA/update.zip");
    int update_trigger = property_get_bool("sys.update.trigger", false);
#endif
    
#ifdef STARTUP_SERVICE
    std::string startupPath(mRawPath + "/startup/start_up.sh");
    int startup_trigger = property_get_bool("sys.startup.trigger", false);
#endif
    
#ifdef CUSTCFG_UPGRADE
    std::string custPath(mRawPath + "/cust/cust_update.zip");
    int cust_trigger = property_get_bool("sys.cust.trigger", false);
#endif

    setInternalPath(mRawPath);
    if (getMountFlags() & MountFlags::kVisible) {
        setPath(StringPrintf("/storage/%s", stableName.c_str()));
    } else {
        setPath(mRawPath);
    }

    if (fs_prepare_dir(mRawPath.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mFuseDefault.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mFuseRead.c_str(), 0700, AID_ROOT, AID_ROOT) ||
            fs_prepare_dir(mFuseWrite.c_str(), 0700, AID_ROOT, AID_ROOT)) {
        PLOG(ERROR) << getId() << " failed to create mount points";
        return -errno;
    }

    if ( ntfs ) {
    	if (Ntfs::doMount(mDevPath, mRawPath, false, false, false,
		 AID_MEDIA_RW, AID_MEDIA_RW, 0007, true)) {
	PLOG(ERROR) << getId() << "failed to mount via NTFS " << mDevPath;
	}
    } else if (vfat::Mount(mDevPath, mRawPath, false, false, false,
            AID_MEDIA_RW, AID_MEDIA_RW, 0007, true)) {
        PLOG(ERROR) << getId() << " failed to mount " << mDevPath;
        return -EIO;
    }

#ifdef OFFLINE_OTAPROC
    if (update_trigger == 1) {
        LOG(VERBOSE) << "[OTAPROC] Another OTA process is on going";
    } else {
        LOG(VERBOSE) << "[OTAPROC] Check OTA update package : " << updatePath;
        
        if (!access(updatePath.c_str(), F_OK)) {
            LOG(VERBOSE) << "[OTAPROC] Success";

            property_set("sys.update.storage", stableName.c_str());
            property_set("sys.update.path", updatePath.c_str());            
            property_set("sys.update.trigger", "1");
            goto uevent_handled;
        } else {
            LOG(VERBOSE) << "[OTAPROC] Bypass";
        }
    }
#endif

#ifdef STARTUP_SERVICE
    if (startup_trigger == 1) {
        LOG(VERBOSE) << "[STARTUP] Another startup process is on going";
    } else {
        LOG(VERBOSE) << "[STARTUP] Check startup update package : " << startupPath;
        
        if (!access(startupPath.c_str(), F_OK)) {
            LOG(VERBOSE) << "[STARTUP] Success";

            property_set("sys.startup.path", startupPath.c_str());          
            property_set("sys.startup.storage", stableName.c_str());
            property_set("sys.startup.trigger", "1");
            goto uevent_handled;
        } else {
            LOG(VERBOSE) << "[STARTUP] Bypass";
        }
    }
#endif

#ifdef CUSTCFG_UPGRADE
    if (cust_trigger == 1) {
        LOG(VERBOSE) << "[CUST] Another cust upgrade process is on going";
    } else {
        LOG(VERBOSE) << "[CUST] Check cust upgrade package : " << custPath;
        
        if (!access(custPath.c_str(), F_OK)) {
            LOG(VERBOSE) << "[CUST] Success";

            property_set("sys.cust.path", custPath.c_str());        
            property_set("sys.cust.storage", stableName.c_str());
            property_set("sys.cust.trigger", "1");
            goto uevent_handled;
        } else {
            LOG(VERBOSE) << "[CUST] Bypass";
        }
    }
#endif

uevent_handled:
    if (getMountFlags() & MountFlags::kPrimary) {
        initAsecStage();
    }

    if (!(getMountFlags() & MountFlags::kVisible)) {
        // Not visible to apps, so no need to spin up FUSE
        return OK;
    }

    dev_t before = GetDevice(mFuseWrite);

    if (!(mFusePid = fork())) {
        if (getMountFlags() & MountFlags::kPrimary) {
            if (execl(kFusePath, kFusePath,
                    "-u", "1023", // AID_MEDIA_RW
                    "-g", "1023", // AID_MEDIA_RW
                    "-U", std::to_string(getMountUserId()).c_str(),
                    "-w",
                    mRawPath.c_str(),
                    stableName.c_str(),
                    NULL)) {
                PLOG(ERROR) << "Failed to exec";
            }
        } else {
            if (execl(kFusePath, kFusePath,
                    "-u", "1023", // AID_MEDIA_RW
                    "-g", "1023", // AID_MEDIA_RW
                    "-U", std::to_string(getMountUserId()).c_str(),
                    mRawPath.c_str(),
                    stableName.c_str(),
                    NULL)) {
                PLOG(ERROR) << "Failed to exec";
            }
        }

        LOG(ERROR) << "FUSE exiting";
        _exit(1);
    }

    if (mFusePid == -1) {
        PLOG(ERROR) << getId() << " failed to fork";
        return -errno;
    }

    while (before == GetDevice(mFuseWrite)) {
        LOG(VERBOSE) << "Waiting for FUSE to spin up...";
        usleep(50000); // 50ms
    }

    return OK;
}

status_t PublicVolume::doUnmount() {
    if (mFusePid > 0) {
        kill(mFusePid, SIGTERM);
        TEMP_FAILURE_RETRY(waitpid(mFusePid, nullptr, 0));
        mFusePid = 0;
    }

    ForceUnmount(kAsecPath);

    ForceUnmount(mFuseDefault);
    ForceUnmount(mFuseRead);
    ForceUnmount(mFuseWrite);
    ForceUnmount(mRawPath);

    rmdir(mFuseDefault.c_str());
    rmdir(mFuseRead.c_str());
    rmdir(mFuseWrite.c_str());
    rmdir(mRawPath.c_str());

    mFuseDefault.clear();
    mFuseRead.clear();
    mFuseWrite.clear();
    mRawPath.clear();

#ifdef CONFIG_UEVENT_PROC
    // Use UUID as stable name, if available
    std::string stableName = getId();

    if (!mFsUuid.empty()) {
        stableName = mFsUuid;
    }

#ifdef OFFLINE_OTAPROC
    char update_storage[PROPERTY_VALUE_MAX];
    
    property_get("sys.update.storage", update_storage, "");

    if (!strcmp(update_storage, stableName.c_str())) {
        LOG(VERBOSE) << "[OTAPROC] doUnmount, clear update properties";
        
        property_set("sys.update.path", "");
        property_set("sys.update.storage", "");     
        property_set("sys.update.trigger", "0");        
    }
#endif

#ifdef STARTUP_SERVICE
    char startup_storage[PROPERTY_VALUE_MAX];

    property_get("sys.startup.storage", startup_storage, "");

    if (!strcmp(startup_storage, stableName.c_str())) {
        LOG(VERBOSE) << "[STARTUP] doUnmount, clear update properties";
    
        property_set("sys.startup.path", "");
        property_set("sys.startup.storage", "");    
        property_set("sys.startup.trigger", "0");       
    }
#endif

#ifdef CUSTCFG_UPGRADE
    char cust_storage[PROPERTY_VALUE_MAX];

    property_get("sys.cust.storage", cust_storage, "");

    if (!strcmp(cust_storage, stableName.c_str())) {
        LOG(VERBOSE) << "[CUST] doUnmount, clear update properties";
    
        property_set("sys.cust.path", "");
        property_set("sys.cust.storage", "");   
        property_set("sys.cust.trigger", "0");      
    }
#endif
#endif //CONFIG_UEVENT_PROC

    return OK;
}

status_t PublicVolume::doFormat(const std::string& fsType) {
    if (fsType == "vfat" || fsType == "auto") {
        if (WipeBlockDevice(mDevPath) != OK) {
            LOG(WARNING) << getId() << " failed to wipe";
        }
        if (vfat::Format(mDevPath, 0)) {
            LOG(ERROR) << getId() << " failed to format";
            return -errno;
        }
    } else {
        LOG(ERROR) << "Unsupported filesystem " << fsType;
        return -EINVAL;
    }

    return OK;
}

}  // namespace vold
}  // namespace android
