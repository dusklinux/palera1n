/*
 * pongoOS - https://checkra.in
 *
 * Copyright (C) 2019-2023 checkra1n team
 *
 * This file is part of pongoOS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 */
#ifdef USE_LIBUSB
#include <errno.h>
#include <fcntl.h>              // open
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>             // exit, strtoull
#include <string.h>             // strlen, strerror, memcpy, memmove
#include <unistd.h>             // close
#include <sys/mman.h>           // mmap, munmap
#include <time.h>
#include <sys/time.h>
#include <usb_power.h>
#include <sys/stat.h>           // fstst

#include <palerain.h>
#define ERR(...) LOG(LOG_ERROR, __VA_ARGS__)

void io_start(stuff_t *stuff);
void io_stop(stuff_t *stuff);

const char *usb_strerror(usb_ret_t err)
{
    return libusb_error_name(err);
}

usb_ret_t USBControlTransfer(usb_device_handle_t handle, uint8_t bmRequestType, uint8_t bRequest, uint16_t wValue, uint16_t wIndex, uint32_t wLength, void *data, uint32_t *wLenDone)
{
    usb_ret_t r = libusb_control_transfer(handle, bmRequestType, bRequest, wValue, wIndex, data, wLength, 30000);
    if(r < 0) return r;
    if(wLenDone) *wLenDone = r;
    return USB_RET_SUCCESS;
}

usb_ret_t USBBulkUpload(usb_device_handle_t handle, void *data, int len)
{
    static uint32_t maxLen = 0;
    int transferred = 0;
    usb_ret_t r;
    if(maxLen == 0)
    {
        r = libusb_bulk_transfer(handle, 2, data, len, &transferred, 120000);
        if(r == LIBUSB_SUCCESS)
        {
            return transferred == len ? USB_RET_SUCCESS : LIBUSB_ERROR_INTERRUPTED;
        }
        else if(r != LIBUSB_ERROR_NO_MEM)
        {
            return r;
        }
#if defined(__linux__)
        // We only get here on ENOMEM
        FILE *f = fopen("/sys/module/usbcore/parameters/usbfs_memory_mb", "r");
        if(f)
        {
            char str[32]; // More than enough to hold a uint64 in decimal
            size_t s = fread(str, 1, sizeof(str), f);
            fclose(f);
            if(s == 0 || s >= sizeof(str)) return r;
            str[s] = '\0';
            char *end = NULL;
            unsigned long long max = strtoull(str, &end, 0);
            // Using the limit as-is will lead to ENOMEM, so we multiply
            // by half a MB and impose an appropriate max value.
            if(*end == '\n') ++end;
            if(*end != '\0' || max == 0 || max >= 0x2000) return r;
            maxLen = (uint32_t)(max << 19);
        }
        else
#endif
        {
            // Just 1MB by default?
            maxLen = 0x100000;
        }
    }
    // If we get here, we have to chunk our data
    for(int done = transferred; done < len; )
    {
        uint32_t chunk = len - done;
        if(chunk > maxLen) chunk = maxLen;
        transferred = 0;
        r = libusb_bulk_transfer(handle, 2, (unsigned char*)data + done, chunk, &transferred, 120000);
        done += transferred;
        if(r == LIBUSB_SUCCESS) {
            if(transferred == 0) return LIBUSB_ERROR_IO;
            continue;
        }
        if(r != LIBUSB_ERROR_NO_MEM || maxLen <= 0x40) return r;
        maxLen /= 2;
    }
    return LIBUSB_SUCCESS;
}

#ifndef PONGO_SYSFS_ROOT
#define PONGO_SYSFS_ROOT "/sys/bus/usb/devices"
#endif

/* Discovery does not depend on a new hotplug event: Pongo may already be here. */
int pongo_is_connected(void)
{
    libusb_context *ctx = NULL;
    libusb_device **list = NULL;
    int result = libusb_init(&ctx);
    if (result) return result;
    ssize_t count = libusb_get_device_list(ctx, &list);
    result = 0;
    if (count < 0) result = (int)count;
    else for (ssize_t i = 0; i < count; ++i) {
        struct libusb_device_descriptor desc;
        if (libusb_get_device_descriptor(list[i], &desc)) continue;
        if (desc.idVendor == PONGO_USB_VENDOR && desc.idProduct == PONGO_USB_PRODUCT) ++result;
    }
    if (list) libusb_free_device_list(list, 1);
    libusb_exit(ctx);
    if (result > 1) {
        ERR("Multiple PongoOS devices connected; select one device before continuing");
        return LIBUSB_ERROR_BUSY;
    }
    return result;
}

typedef struct {
    libusb_context *ctx;
    libusb_device **list;
    libusb_device_handle *handle;
    bool claimed;
#ifdef __linux__
    usb_power_state power;
#endif
} pongo_connection;

static void close_connection(void *opaque)
{
    pongo_connection *connection = opaque;
    if (connection->handle) {
        if (connection->claimed) libusb_release_interface(connection->handle, 0);
        libusb_close(connection->handle);
        connection->handle = NULL;
        connection->claimed = false;
    }
#ifdef __linux__
    usb_power_restore(&connection->power);
#endif
    if (connection->list) {
        libusb_free_device_list(connection->list, 1);
        connection->list = NULL;
    }
}

static void cleanup_connection(void *opaque)
{
    pongo_connection *connection = opaque;
    close_connection(connection);
    libusb_exit(connection->ctx);
}

static bool retry_open(int error)
{
    return error == LIBUSB_ERROR_NO_DEVICE || error == LIBUSB_ERROR_IO ||
           error == LIBUSB_ERROR_BUSY || error == LIBUSB_ERROR_TIMEOUT ||
           error == LIBUSB_ERROR_INTERRUPTED || error == LIBUSB_ERROR_NOT_FOUND;
}

int wait_for_pongo(void)
{
    pongo_connection connection = {0};
    int result = libusb_init(&connection.ctx);
    if (result) { ERR("libusb_init: %s", libusb_error_name(result)); return result; }
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    int last_error = 0;
    pthread_cleanup_push(cleanup_connection, &connection);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - start.tv_sec >= 120) {
            result = LIBUSB_ERROR_TIMEOUT;
            ERR("Timed out waiting for a usable PongoOS USB connection");
            break;
        }
        ssize_t count = libusb_get_device_list(connection.ctx, &connection.list);
        if (count < 0) { result = (int)count; break; }
        libusb_device *device = NULL;
        for (ssize_t i = 0; i < count; ++i) {
            struct libusb_device_descriptor desc;
            if (libusb_get_device_descriptor(connection.list[i], &desc)) continue;
            if (desc.idVendor == PONGO_USB_VENDOR && desc.idProduct == PONGO_USB_PRODUCT) {
                if (device) { result = LIBUSB_ERROR_BUSY; break; }
                device = connection.list[i];
            }
        }
        if (result == LIBUSB_ERROR_BUSY) { ERR("Multiple PongoOS devices connected"); break; }
        if (device) {
#ifdef __linux__
            unsigned char ports[7];
            int depth = libusb_get_port_numbers(device, ports, sizeof(ports));
            if (depth > 0 && usb_power_hold(&connection.power, PONGO_SYSFS_ROOT,
                    libusb_get_bus_number(device), ports, (size_t)depth,
                    libusb_get_device_address(device)) && errno != ENOENT && errno != ENODEV)
                LOG(LOG_WARNING, "Cannot hold PongoOS USB runtime power: %s", strerror(errno));
#endif
            result = libusb_open(device, &connection.handle);
            if (!result) result = libusb_set_auto_detach_kernel_driver(connection.handle, 1);
            if (result == LIBUSB_ERROR_NOT_SUPPORTED && connection.handle) result = 0;
            int configuration = 0;
            if (!result) result = libusb_get_configuration(connection.handle, &configuration);
            if (!result && configuration != 1) result = libusb_set_configuration(connection.handle, 1);
            if (!result) result = libusb_claim_interface(connection.handle, 0);
            if (!result) {
                connection.claimed = true;
                stuff_t stuff = {.dev = device, .handle = connection.handle};
                io_start(&stuff);
                result = stuff.result;
                break; /* Never replay a partially uploaded command sequence. */
            }
            if (result != last_error) {
                LOG(LOG_WARNING, "PongoOS USB open: %s%s", libusb_error_name(result),
                    retry_open(result) ? "; retrying discovery" : "");
                last_error = result;
            }
            if (!retry_open(result)) break;
        }
        close_connection(&connection);
        result = 0;
        struct timeval pause = {.tv_sec = 0, .tv_usec = 200000};
        result = libusb_handle_events_timeout(connection.ctx, &pause);
        if (result == LIBUSB_ERROR_INTERRUPTED) result = 0;
        if (result) break;
    }
    pthread_cleanup_pop(1);
    return result;
}

#else
/* ISO C forbids an empty translation unit */
extern char** environ;
#endif
