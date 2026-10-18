#include <palerain.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

static int fixture_clock(clockid_t id, struct timespec *time);
#define clock_gettime fixture_clock
#define RAMDISK_UNCOMPRESSED_SIZE "16"
#define KPF_UNCOMPRESSED_SIZE "16"
#include "../src/pongo_helper.c"
#undef clock_gettime

unsigned int verbose;
uint64_t palerain_flags;
char xargs_cmd[0x270] = "xargs";
char palerain_flags_cmd[0x30] = "palera1n_flags 0x2";
int dfuhelper_thr_running;
pthread_t dfuhelper_thread;
uint8_t checkra1n_kpf_pongo_lzma[1], ramdisk_dmg_lzma[1], binpack_dmg[1];
uint32_t checkra1n_kpf_pongo_lzma_len = 1, ramdisk_dmg_lzma_len = 1, binpack_dmg_len = 1;
niarelap_file_t *kpf = &checkra1n_kpf_pongo_lzma, *ramdisk = &ramdisk_dmg_lzma, *overlay = &binpack_dmg;
niarelap_file_t **kpf_to_upload = &kpf, **ramdisk_to_upload = &ramdisk, **overlay_to_upload = &overlay;
static bool found, spinning;
static int controls, bulk_calls, fail_first, fail_bulk, busy, now, clock_step;
static int handoff_log, boot_commands, disconnect_boot;

void thr_cleanup(void *value) { *(int *)value = 0; }
bool get_spin(void) { return spinning; }
bool set_spin(bool value) { return spinning = value; }
bool get_found_pongo(void) { return found; }
bool set_found_pongo(bool value) { return found = value; }
int wait_for_pongo(void) { return 0; }
int p1_log(log_level_t level, const char *file, int line, const char *function, const char *format, ...)
{ if (strstr(format, "Kernel handoff sent")) ++handoff_log; return 0; }
const char *usb_strerror(usb_ret_t error) { return "fixture"; }
static int fixture_clock(clockid_t id, struct timespec *time)
{ time->tv_sec = now; time->tv_nsec = 0; now += clock_step; return 0; }
usb_ret_t USBControlTransfer(usb_device_handle_t handle, uint8_t type, uint8_t request,
    uint16_t value, uint16_t index, uint32_t length, void *data, uint32_t *done)
{
    ++controls;
    if (fail_first && controls == 1) return LIBUSB_ERROR_IO;
    if (type == 0x21 && request == 3 && !strncmp(data, "bootx", 5)) {
        ++boot_commands;
        if (disconnect_boot) return LIBUSB_ERROR_NO_DEVICE;
    }
    if (type == 0xa1 && request == 2) *(uint8_t *)data = busy ? 1 : 0;
    if (done) *done = 0;
    return 0;
}
usb_ret_t USBBulkUpload(usb_device_handle_t handle, void *data, int length)
{ ++bulk_calls; return fail_bulk ? LIBUSB_ERROR_IO : 0; }

static void reset_fixture(void)
{
    found = device_has_booted = false;
    spinning = true; palerain_flags = palerain_option_rootless;
    controls = bulk_calls = fail_first = fail_bulk = busy = now = clock_step = 0;
    handoff_log = boot_commands = disconnect_boot = 0;
}

int main(void)
{
    stuff_t stuff = {.handle = (void *)2};
    reset_fixture(); fail_first = 1; io_start(&stuff);
    assert(stuff.result == LIBUSB_ERROR_IO && !device_has_booted && !handoff_log && !bulk_calls && !spinning);
    reset_fixture(); fail_first = 1;
    assert(issue_pongo_command(stuff.handle, "bootx") == LIBUSB_ERROR_IO && !boot_commands);
    reset_fixture(); fail_bulk = 1; io_start(&stuff);
    assert(stuff.result == LIBUSB_ERROR_IO && bulk_calls == 1 && !boot_commands && !handoff_log);
    reset_fixture(); busy = 1; clock_step = 40; io_start(&stuff);
    assert(stuff.result == LIBUSB_ERROR_TIMEOUT && !handoff_log && !bulk_calls);
    reset_fixture(); disconnect_boot = 1; io_start(&stuff);
    assert(stuff.result == 0 && device_has_booted && boot_commands == 1 && handoff_log == 1 && bulk_calls == 3);
    reset_fixture(); palerain_flags |= palerain_option_pongo_full; io_start(&stuff);
    assert(stuff.result == 0 && !boot_commands && !handoff_log && bulk_calls == 3);
    puts("Pongo tests passed: command/upload failures, timeout, expected boot disconnect, image-only mode");
    return 0;
}
