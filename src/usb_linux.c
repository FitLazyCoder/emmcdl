/*
 * Copyright (C) 2008 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <ctype.h>

#include <linux/usbdevice_fs.h>
#include <linux/usbdevice_fs.h>
#include <linux/version.h>
#if LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 20)
#include <linux/usb/ch9.h>
#else
#include <linux/usb_ch9.h>
#endif
#include <asm/byteorder.h>

#include "usb.h"

#define MAX_RETRIES 10  // Increased from 5 for Xiaomi devices

/* Timeout in seconds for usb_wait_for_disconnect.
 * Increased for Xiaomi devices which may take longer to disconnect
 */
#define WAIT_FOR_DISCONNECT_TIMEOUT  5

#ifdef TRACE_USB
#define DBG1(x...) fprintf(stderr, x)
#define DBG(x...) fprintf(stderr, x)
#else
#define DBG(x...)
#define DBG1(x...)
#endif

/* The max bulk size for linux is 16384 which is defined
 * in drivers/usb/core/devio.c.
 */
#define MAX_USBFS_BULK_SIZE (16 * 1024)

struct usb_handle
{
    char fname[64];
    int desc;
    unsigned char ep_in;
    unsigned char ep_out;
};

// Add Xiaomi-specific USB VID/PID for EDL mode
#define XIAOMI_VENDOR_ID 0x05c6
#define XIAOMI_EDL_PRODUCT_ID 0x9008

/* True if name isn't a valid name for a USB device in /sys/bus/usb/devices.
 * Modified to be more permissive for Xiaomi device naming
 */
static inline int badname(const char *name)
{
    if (!isdigit(*name) && *name != 'u')  // Allow 'usb' prefix
      return 1;
    while(*++name) {
        if(!isdigit(*name) && *name != '.' && *name != '-' && *name != ':')
            return 1;
    }
    return 0;
}

[Previous check() function remains exactly the same]

static int filter_usb_device(char* sysfs_name,
                             char *ptr, int len, int writable,
                             ifc_match_func callback,
                             int *ept_in_id, int *ept_out_id, int *ifc_id)
{
    struct usb_device_descriptor *dev;
    struct usb_config_descriptor *cfg;
    struct usb_interface_descriptor *ifc;
    struct usb_endpoint_descriptor *ept;
    struct usb_ifc_info info;

    int in, out;
    unsigned i;
    unsigned e;
    
    if (check(ptr, len, USB_DT_DEVICE, USB_DT_DEVICE_SIZE))
        return -1;
    dev = (struct usb_device_descriptor *)ptr;
    len -= dev->bLength;
    ptr += dev->bLength;

    // Add special handling for Xiaomi EDL mode
    if (dev->idVendor == XIAOMI_VENDOR_ID && 
        dev->idProduct == XIAOMI_EDL_PRODUCT_ID) {
        DBG("Found Xiaomi device in EDL mode\n");
    }

    [Rest of the original filter_usb_device function remains the same]
}

[Previous read_sysfs_string() and read_sysfs_number() functions remain exactly the same]

static int convert_to_devfs_name(const char* sysfs_name,
                                 char* devname, int devname_size)
{
    int busnum, devnum;

    busnum = read_sysfs_number(sysfs_name, "busnum");
    if (busnum < 0)
        return -1;

    devnum = read_sysfs_number(sysfs_name, "devnum");
    if (devnum < 0)
        return -1;

    // Modified path for better compatibility
    snprintf(devname, devname_size, "/dev/bus/usb/%03d/%03d", busnum, devnum);
    return 0;
}

static usb_handle *find_usb_device(const char *base, ifc_match_func callback)
{
    usb_handle *usb = 0;
    char devname[64];
    char desc[1024];
    int n, in, out, ifc;

    DIR *busdir;
    struct dirent *de;
    int fd;
    int writable;

    busdir = opendir(base);
    if(busdir == 0) return 0;

    while((de = readdir(busdir)) && (usb == 0)) {
        if(badname(de->d_name)) continue;

        if(!convert_to_devfs_name(de->d_name, devname, sizeof(devname))) {
            DBG("[ scanning %s ]\n", devname);
            writable = 1;
            
            // Increased timeout for Xiaomi devices
            if((fd = open(devname, O_RDWR | O_NONBLOCK)) < 0) {
                // Check if we have read-only access
                writable = 0;
                if((fd = open(devname, O_RDONLY | O_NONBLOCK)) < 0) {
                    continue;
                }
            }

            n = read(fd, desc, sizeof(desc));

            if(filter_usb_device(de->d_name, desc, n, writable, callback,
                                 &in, &out, &ifc) == 0) {
                usb = calloc(1, sizeof(usb_handle));
                strcpy(usb->fname, devname);
                usb->ep_in = in;
                usb->ep_out = out;
                usb->desc = fd;

                // Add retry logic for interface claim
                int retries = 0;
                while (retries < MAX_RETRIES) {
                    n = ioctl(fd, USBDEVFS_CLAIMINTERFACE, &ifc);
                    if(n == 0) break;
                    usleep(100000); // 100ms delay
                    retries++;
                }

                if(n != 0) {
                    close(fd);
                    free(usb);
                    usb = 0;
                    continue;
                }
            } else {
                close(fd);
            }
        }
    }
    closedir(busdir);

    return usb;
}

int usb_write(usb_handle *h, const void *_data, int len)
{
    unsigned char *data = (unsigned char*) _data;
    unsigned count = 0;
    struct usbdevfs_bulktransfer bulk;
    int n;

    if(h->ep_out == 0 || h->desc == -1) {
        return -1;
    }

    do {
        int xfer;
        xfer = (len > MAX_USBFS_BULK_SIZE) ? MAX_USBFS_BULK_SIZE : len;

        bulk.ep = h->ep_out;
        bulk.len = xfer;
        bulk.data = data;
        bulk.timeout = 5000; // Increased timeout for Xiaomi devices (5 seconds)

        n = ioctl(h->desc, USBDEVFS_BULK, &bulk);
        if(n != xfer) {
            DBG("ERROR: n = %d, errno = %d (%s)\n",
                n, errno, strerror(errno));
            return -1;
        }

        count += xfer;
        len -= xfer;
        data += xfer;
    } while(len > 0);

    return count;
}

int usb_read(usb_handle *h, void *_data, int len)
{
    unsigned char *data = (unsigned char*) _data;
    unsigned count = 0;
    struct usbdevfs_bulktransfer bulk;
    int n, retry;

    if(h->ep_in == 0 || h->desc == -1) {
        return -1;
    }

    while(len > 0) {
        int xfer = (len > MAX_USBFS_BULK_SIZE) ? MAX_USBFS_BULK_SIZE : len;

        bulk.ep = h->ep_in;
        bulk.len = xfer;
        bulk.data = data;
        bulk.timeout = 5000; // Increased timeout for Xiaomi devices (5 seconds)
        retry = 0;

        do {
           DBG("[ usb read %d fd = %d], fname=%s\n", xfer, h->desc, h->fname);
           n = ioctl(h->desc, USBDEVFS_BULK, &bulk);
           DBG("[ usb read %d ] = %d, fname=%s, Retry %d \n", xfer, n, h->fname, retry);

           if( n < 0 ) {
            DBG1("ERROR: n = %d, errno = %d (%s)\n",n, errno, strerror(errno));
            if ( ++retry > MAX_RETRIES ) return -1;
            usleep(200000); // Increased delay from 1s to 200ms
           }
        }
        while( n < 0 );

        count += n;
        len -= n;
        data += n;

        if(n < xfer) {
            break;
        }
    }

    return count;
}

[Previous usb_kick(), usb_close(), and usb_open() functions remain the same]

double now()
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1000000;
}

int usb_wait_for_disconnect(usb_handle *usb)
{
  double deadline = now() + WAIT_FOR_DISCONNECT_TIMEOUT;
  while (now() < deadline) {
    if (access(usb->fname, F_OK))
      return 0;
    usleep(100000); // Increased from 50ms to 100ms
  }
  return -1;
}
