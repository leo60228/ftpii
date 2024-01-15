/*

Copyright (C) 2008 Joseph Jordan <joe.ftpii@psychlaw.com.au>

This software is provided 'as-is', without any express or implied warranty.
In no event will the authors be held liable for any damages arising from
the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1.The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software in a
product, an acknowledgment in the product documentation would be
appreciated but is not required.

2.Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3.This notice may not be removed or altered from any source distribution.

*/
#include <fat.h>
#include <malloc.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/mutex.h>
#include <ogc/system.h>
#include <sdcard/gcsd.h>
#include <stdio.h>
#include <string.h>
#include <sys/dir.h>
#include <unistd.h>

#include "fs.h"

#define CACHE_PAGES 8
#define CACHE_SECTORS_PER_PAGE 64

VIRTUAL_PARTITION VIRTUAL_PARTITIONS[] = {
    { "SD Gecko A", "/carda", "carda", "carda:/", false, false, &__io_gcsda },
    { "SD Gecko B", "/cardb", "cardb", "cardb:/", false, false, &__io_gcsdb },
};
const u32 MAX_VIRTUAL_PARTITIONS = (sizeof(VIRTUAL_PARTITIONS) / sizeof(VIRTUAL_PARTITION));

VIRTUAL_PARTITION *PA_GCSDA   = VIRTUAL_PARTITIONS + 0;
VIRTUAL_PARTITION *PA_GCSDB   = VIRTUAL_PARTITIONS + 1;

static VIRTUAL_PARTITION *to_virtual_partition(const char *virtual_prefix) {
    u32 i;
    for (i = 0; i < MAX_VIRTUAL_PARTITIONS; i++)
        if (!strcasecmp(VIRTUAL_PARTITIONS[i].alias, virtual_prefix))
            return &VIRTUAL_PARTITIONS[i];
    return NULL;
}

static bool is_gecko(VIRTUAL_PARTITION *partition) {
    return partition == PA_GCSDA || partition == PA_GCSDB;
}

static bool is_fat(VIRTUAL_PARTITION *partition) {
    return is_gecko(partition);
}

bool mounted(VIRTUAL_PARTITION *partition) {
    DIR *dir = opendir(partition->prefix);
    if (dir) {
        closedir(dir);
        return true;
    }
    return false;
}

static bool was_inserted_or_removed(VIRTUAL_PARTITION *partition) {
    if (!partition->disc || partition->geckofail) return false;
    bool already_inserted = partition->inserted || mounted(partition);
    partition->inserted = partition->disc->isInserted();
    return already_inserted != partition->inserted;
}

typedef enum { MOUNTSTATE_START, MOUNTSTATE_SELECTDEVICE, MOUNTSTATE_WAITFORDEVICE } mountstate_t;
static mountstate_t mountstate = MOUNTSTATE_START;
static VIRTUAL_PARTITION *mount_partition = NULL;
static u64 mount_timer = 0;

bool mount(VIRTUAL_PARTITION *partition) {
    if (!partition || mounted(partition)) return false;
    
    bool success = false;
    printf("Mounting %s...", partition->name);
    if (is_fat(partition)) {
        bool retry_gecko = true;
        gecko_retry:
        if (partition->disc->shutdown() & partition->disc->startup()) {
            if (fatMount(partition->mount_point, partition->disc, 0, CACHE_PAGES, CACHE_SECTORS_PER_PAGE)) {
                success = true;
            }
        } else if (is_gecko(partition) && retry_gecko) {
            retry_gecko = false;
            sleep(1);
            goto gecko_retry;
        }
    }
    printf(success ? "succeeded.\n" : "failed.\n");
    if (success && is_gecko(partition)) partition->geckofail = false;

    return success;
}

bool mount_virtual(const char *dir) {
    return mount(to_virtual_partition(dir));
}

bool unmount(VIRTUAL_PARTITION *partition) {
    if (!partition || !mounted(partition)) return false;

    printf("Unmounting %s...", partition->name);
    bool success = false;
    if (is_fat(partition)) {
        fatUnmount(partition->prefix);
        success = true;
    }
    printf(success ? "succeeded.\n" : "failed.\n");

    return success;
}

bool unmount_virtual(const char *dir) {
    return unmount(to_virtual_partition(dir));
}

static u64 device_check_timer = 0;

void check_removable_devices(u64 now) {
    if (now <= device_check_timer) return;

    u32 i;
    for (i = 0; i < MAX_VIRTUAL_PARTITIONS; i++) {
        VIRTUAL_PARTITION *partition = VIRTUAL_PARTITIONS + i;
        if (mount_timer && partition == mount_partition) continue;
        if (was_inserted_or_removed(partition)) {
            if (partition->inserted && !mounted(partition)) {
                printf("Device inserted; ");
                if (!mount(partition) && is_gecko(partition)) {
                    printf("%s failed to automount.  Insertion or removal will not be detected until it is mounted manually.\n", partition->name);
                    printf("Note that inserting an SD Gecko without an SD card in it can be problematic.\n");
                    partition->geckofail = true;
                }
            } else if (!partition->inserted && mounted(partition)) {
                printf("Device removed; ");
                unmount(partition);
            }
        }
    }
    
    device_check_timer = gettime() + secs_to_ticks(2);
}

void process_remount_event() {
    if (mountstate == MOUNTSTATE_START || mountstate == MOUNTSTATE_SELECTDEVICE) {
        mountstate = MOUNTSTATE_SELECTDEVICE;
        mount_partition = NULL;
        printf("\nWhich device would you like to remount? (hold button on controller #1)\n\n");
        printf("           SD Gecko A (Up)\n");
        printf("                  | \n");
        printf("                  | \n");
        printf("                  | \n");
        printf("           SD Gecko B (Down)\n");
    } else if (mountstate == MOUNTSTATE_WAITFORDEVICE) {
        mount_timer = 0;
        mountstate = MOUNTSTATE_START;
        mount(mount_partition);
        mount_partition = NULL;
    }
}

void process_device_select_event(u32 pressed) {
    if (mountstate == MOUNTSTATE_SELECTDEVICE) {
        if (pressed & PAD_BUTTON_UP) mount_partition = PA_GCSDA;
        else if (pressed & PAD_BUTTON_DOWN) mount_partition = PA_GCSDB;
        if (mount_partition) {
            mountstate = MOUNTSTATE_WAITFORDEVICE;
            if (is_fat(mount_partition)) unmount(mount_partition);
            printf("To continue after changing the device hold B on controller #1 or wait 30 seconds.\n");
            mount_timer = gettime() + secs_to_ticks(30);
        }
    }
}

void check_mount_timer(u64 now) {
    if (mount_timer && now > mount_timer) process_remount_event();
}

void initialise_fs() {
}

/*
    Returns a copy of path up to the last '/' character,
    If path does not contain '/', returns "".
    Returns a pointer to internal static storage space that will be overwritten by subsequent calls.
    This function is not thread-safe.
*/
char *dirname(char *path) {
    static char result[PATH_MAX];
    strncpy(result, path, PATH_MAX - 1);
    result[PATH_MAX - 1] = '\0';
    s32 i;
    for (i = strlen(result) - 1; i >= 0; i--) {
        if (result[i] == '/') {
            result[i] = '\0';
            return result;
        }
    }
    return "";
}

/*
    Returns a pointer into path, starting after the right-most '/' character.
    If path does not contain '/', returns path.
*/
char *basename(char *path) {
    s32 i;
    for (i = strlen(path) - 1; i >= 0; i--) {
        if (path[i] == '/') {
            return path + i + 1;
        }
    }
    return path;
}
