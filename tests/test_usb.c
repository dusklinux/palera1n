#include <palerain.h>
#include <usb_power.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int opens, closes, lists, frees, uploads, releases, configurations;
static int transient, denied, descriptor_error, stage_error, devices = 1;
static int clock_tick, clock_value, control_timeout, bulk_timeout, bulk_calls;
static int zero_bulk;
static int mock_clock(clockid_t id, struct timespec *time);
#define clock_gettime mock_clock
#define PONGO_SYSFS_ROOT "/nonexistent/palera1n-usb-test"
#include "../src/usb_libusb.c"
#undef clock_gettime

int p1_log(log_level_t level, const char *file, int line, const char *function, const char *format, ...)
{ return 0; }
int libusb_init(libusb_context **ctx) { *ctx = (void *)1; return 0; }
void libusb_exit(libusb_context *ctx) { assert(ctx == (void *)1); }
ssize_t libusb_get_device_list(libusb_context *ctx, libusb_device ***list)
{
    ++lists;
    *list = calloc((size_t)devices + 1, sizeof(**list));
    for (int i = 0; i < devices; ++i) (*list)[i] = (void *)(uintptr_t)(i + 1);
    return devices;
}
void libusb_free_device_list(libusb_device **list, int unref)
{ assert(unref == 1); ++frees; free(list); }
int libusb_get_device_descriptor(libusb_device *dev, struct libusb_device_descriptor *desc)
{
    if (descriptor_error) return LIBUSB_ERROR_IO;
    memset(desc, 0, sizeof(*desc));
    desc->idVendor = PONGO_USB_VENDOR;
    desc->idProduct = PONGO_USB_PRODUCT;
    return 0;
}
int libusb_get_port_numbers(libusb_device *dev, uint8_t *ports, int length)
{ ports[0] = 1; return 1; }
uint8_t libusb_get_bus_number(libusb_device *dev) { return 1; }
uint8_t libusb_get_device_address(libusb_device *dev) { return 1; }
int libusb_open(libusb_device *dev, libusb_device_handle **handle)
{
    ++opens;
    if (denied) return LIBUSB_ERROR_ACCESS;
    if (transient && opens == 1) return LIBUSB_ERROR_IO;
    *handle = (void *)2;
    return 0;
}
void libusb_close(libusb_device_handle *handle) { assert(handle == (void *)2); ++closes; }
int libusb_set_auto_detach_kernel_driver(libusb_device_handle *handle, int enable) { return 0; }
int libusb_get_configuration(libusb_device_handle *handle, int *config) { *config = 1; return 0; }
int libusb_set_configuration(libusb_device_handle *handle, int config) { ++configurations; return 0; }
int libusb_claim_interface(libusb_device_handle *handle, int interface) { return 0; }
int libusb_release_interface(libusb_device_handle *handle, int interface) { ++releases; return 0; }
int libusb_handle_events_timeout(libusb_context *ctx, struct timeval *time) { return 0; }
const char *libusb_error_name(int error) { return "fixture USB error"; }
int libusb_control_transfer(libusb_device_handle *handle, uint8_t type, uint8_t request,
    uint16_t value, uint16_t index, unsigned char *data, uint16_t length, unsigned int timeout)
{ control_timeout = (int)timeout; return length; }
int libusb_bulk_transfer(libusb_device_handle *handle, unsigned char endpoint,
    unsigned char *data, int length, int *transferred, unsigned int timeout)
{
    bulk_timeout = (int)timeout;
    ++bulk_calls;
    if (zero_bulk) { *transferred = 0; return bulk_calls == 1 ? LIBUSB_ERROR_NO_MEM : 0; }
    *transferred = length;
    return 0;
}
void io_start(stuff_t *stuff) { ++uploads; stuff->result = stage_error; }
static int mock_clock(clockid_t id, struct timespec *time)
{ time->tv_sec = clock_value; time->tv_nsec = 0; clock_value += clock_tick; return 0; }

static void reset(void)
{
    opens = closes = lists = frees = uploads = releases = configurations = 0;
    transient = denied = descriptor_error = stage_error = 0;
    clock_value = 0; clock_tick = 1; devices = 1;
}

static void put(const char *path, const char *value)
{
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs(value, file) >= 0); assert(!fclose(file));
}
static void expect(const char *path, const char *expected)
{
    char value[32]; FILE *file = fopen(path, "r"); assert(file);
    assert(fgets(value, sizeof(value), file)); fclose(file);
    assert(!strcmp(value, expected));
}

int main(void)
{
    reset(); assert(pongo_is_connected() == 1); assert(lists == frees);
    reset(); devices = 2; assert(pongo_is_connected() == LIBUSB_ERROR_BUSY);
    reset(); assert(wait_for_pongo() == 0);
    assert(opens == 1 && uploads == 1 && closes == 1 && releases == 1 && lists == frees);
    assert(configurations == 0); /* Do not reset an already configured device. */
    reset(); transient = 1; assert(wait_for_pongo() == 0);
    assert(opens == 2 && uploads == 1 && closes == 1 && lists == frees);
    reset(); denied = 1; assert(wait_for_pongo() == LIBUSB_ERROR_ACCESS);
    assert(opens == 1 && !uploads && !closes && lists == frees);
    reset(); stage_error = LIBUSB_ERROR_IO; assert(wait_for_pongo() == LIBUSB_ERROR_IO);
    assert(uploads == 1 && opens == 1 && closes == 1); /* No replay after partial upload. */
    reset(); descriptor_error = 1; clock_tick = 40;
    assert(wait_for_pongo() == LIBUSB_ERROR_TIMEOUT); assert(!opens && lists == frees);
    reset(); devices = 2; assert(wait_for_pongo() == LIBUSB_ERROR_BUSY); assert(!opens);
    uint32_t transferred = 0; unsigned char data[256] = {0};
    assert(!USBControlTransfer((void *)2, 0, 0, 0, 0, sizeof(data), data, &transferred));
    assert(control_timeout == 30000 && transferred == sizeof(data));
    zero_bulk = 1; assert(USBBulkUpload((void *)2, data, sizeof(data)) == LIBUSB_ERROR_IO);
    assert(bulk_calls == 2 && bulk_timeout == 120000);

    char root[] = "/tmp/palera1n-power-XXXXXX"; assert(mkdtemp(root));
    char device[512], power[512], control[512], number[512];
    snprintf(device, sizeof(device), "%s/3-2.4", root); assert(!mkdir(device, 0700));
    snprintf(power, sizeof(power), "%s/3-2.4/power", root); assert(!mkdir(power, 0700));
    snprintf(control, sizeof(control), "%s/3-2.4/power/control", root);
    snprintf(number, sizeof(number), "%s/3-2.4/devnum", root);
    put(control, "auto\n"); put(number, "17\n");
    unsigned char ports[] = {2, 4}; usb_power_state state;
    assert(!usb_power_hold(&state, root, 3, ports, 2, 17)); expect(control, "on\n");
    usb_power_restore(&state); expect(control, "auto\n");
    assert(!usb_power_hold(&state, root, 3, ports, 2, 17));
    put(number, "18\n"); usb_power_restore(&state); expect(control, "on\n");
    assert(usb_power_hold(&state, root, 3, ports, 2, 17) == -1);
    unlink(number); unlink(control); rmdir(power); rmdir(device); rmdir(root);
    puts("USB tests passed: discovery, retries, cleanup, timeouts, no replay, runtime power");
    return 0;
}
