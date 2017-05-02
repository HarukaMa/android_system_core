/*
 * Copyright (C) 2014 The Android Open Source Project
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

#define TRACE_TAG ADB

#include "sysdeps.h"

#include <fcntl.h>
#include <inttypes.h>
#include <libavb_user/libavb_user.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>

#include "android-base/properties.h"
#include "android-base/stringprintf.h"
#include <log/log_properties.h>

#include "adb.h"
#include "adb_io.h"
#include "adb_unique_fd.h"
#include "fs_mgr.h"
#include "remount_service.h"

#include "fec/io.h"

struct fstab *fstab;

#ifdef ALLOW_ADBD_DISABLE_VERITY
static const bool kAllowDisableVerity = true;
#else
static const bool kAllowDisableVerity = false;
#endif

/* Turn verity on/off */
static bool set_verity_enabled_state(int fd, const char* block_device, const char* mount_point,
                                     bool enable) {
    if (!make_block_device_writable(block_device)) {
        WriteFdFmt(fd, "Could not make block device %s writable (%s).\n",
                   block_device, strerror(errno));
        return false;
    }

    fec::io fh(block_device, O_RDWR);

    if (!fh) {
        WriteFdFmt(fd, "Could not open block device %s (%s).\n", block_device, strerror(errno));
        WriteFdFmt(fd, "Maybe run adb root?\n");
        return false;
    }

    fec_verity_metadata metadata;

    if (!fh.get_verity_metadata(metadata)) {
        WriteFdFmt(fd, "Couldn't find verity metadata!\n");
        return false;
    }

    if (!enable && metadata.disabled) {
        WriteFdFmt(fd, "Verity already disabled on %s\n", mount_point);
        return false;
    }

    if (enable && !metadata.disabled) {
        WriteFdFmt(fd, "Verity already enabled on %s\n", mount_point);
        return false;
    }

    if (!fh.set_verity_status(enable)) {
        WriteFdFmt(fd, "Could not set verity %s flag on device %s with error %s\n",
                   enable ? "enabled" : "disabled",
                   block_device, strerror(errno));
        return false;
    }

    WriteFdFmt(fd, "Verity %s on %s\n", enable ? "enabled" : "disabled", mount_point);
    return true;
}

/* Helper function to get A/B suffix, if any. If the device isn't
 * using A/B the empty string is returned. Otherwise either "_a",
 * "_b", ... is returned.
 *
 * Note that since sometime in O androidboot.slot_suffix is deprecated
 * and androidboot.slot should be used instead. Since bootloaders may
 * be out of sync with the OS, we check both and for extra safety
 * prepend a leading underscore if there isn't one already.
 */
static std::string get_ab_suffix() {
    std::string ab_suffix = android::base::GetProperty("ro.boot.slot_suffix", "");
    if (ab_suffix == "") {
        ab_suffix = android::base::GetProperty("ro.boot.slot", "");
    }
    if (ab_suffix.size() > 0 && ab_suffix[0] != '_') {
        ab_suffix = std::string("_") + ab_suffix;
    }
    return ab_suffix;
}

/* Use AVB to turn verity on/off */
static bool set_avb_verity_enabled_state(int fd, AvbOps* ops, bool enable_verity) {
    std::string ab_suffix = get_ab_suffix();

    bool verity_enabled;
    if (!avb_user_verity_get(ops, ab_suffix.c_str(), &verity_enabled)) {
        WriteFdFmt(fd, "Error getting verity state\n");
        return false;
    }

    if ((verity_enabled && enable_verity) || (!verity_enabled && !enable_verity)) {
        WriteFdFmt(fd, "verity is already %s\n", verity_enabled ? "enabled" : "disabled");
        return false;
    }

    if (!avb_user_verity_set(ops, ab_suffix.c_str(), enable_verity)) {
        WriteFdFmt(fd, "Error setting verity\n");
        return false;
    }

    WriteFdFmt(fd, "Successfully %s verity\n", enable_verity ? "enabled" : "disabled");
    return true;
}

void set_verity_enabled_state_service(int fd, void* cookie) {
    unique_fd closer(fd);
    bool any_changed = false;

    bool enable = (cookie != NULL);
    if (!kAllowDisableVerity) {
        WriteFdFmt(fd, "%s-verity only works for userdebug builds\n",
                   enable ? "enable" : "disable");
    }

    if (!android::base::GetBoolProperty("ro.secure", false)) {
        WriteFdFmt(fd, "verity not enabled - ENG build\n");
        return;
    }
    if (!__android_log_is_debuggable()) {
        WriteFdFmt(fd, "verity cannot be disabled/enabled - USER build\n");
        return;
    }

    // Figure out if we're using VB1.0 or VB2.0 (aka AVB).
    std::string vbmeta_hash = android::base::GetProperty("ro.boot.vbmeta.digest", "");
    if (vbmeta_hash != "") {
        // Yep, the system is using AVB (by contract, androidboot.vbmeta.hash is
        // set by the bootloader when using AVB).
        AvbOps* ops = avb_ops_user_new();
        if (ops == nullptr) {
            WriteFdFmt(fd, "Error getting AVB ops\n");
            return;
        }
        if (set_avb_verity_enabled_state(fd, ops, enable)) {
            any_changed = true;
        }
        avb_ops_user_free(ops);
    } else {
        // Not using AVB - assume VB1.0.

        // read all fstab entries at once from all sources
        fstab = fs_mgr_read_fstab_default();
        if (!fstab) {
            WriteFdFmt(fd, "Failed to read fstab\nMaybe run adb root?\n");
            return;
        }

        // Loop through entries looking for ones that vold manages.
        for (int i = 0; i < fstab->num_entries; i++) {
            if (fs_mgr_is_verified(&fstab->recs[i])) {
                if (set_verity_enabled_state(fd, fstab->recs[i].blk_device,
                                             fstab->recs[i].mount_point, enable)) {
                    any_changed = true;
                }
            }
        }
    }

    if (any_changed) {
        WriteFdFmt(fd, "Now reboot your device for settings to take effect\n");
    }
}
