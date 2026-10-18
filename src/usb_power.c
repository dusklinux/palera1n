#ifdef __linux__
#include <usb_power.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

static int read_attribute(const char *dir, const char *name, char *value, size_t size)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int result = fgets(value, size, f) ? 0 : -1;
    fclose(f);
    return result;
}

static int write_control(const char *dir, const char *value)
{
    char path[8192];
    snprintf(path, sizeof(path), "%s/power/control", dir);
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int result = fputs(value, f) == EOF ? -1 : 0;
    if (fclose(f)) result = -1;
    return result;
}

int usb_power_hold(usb_power_state *state, const char *root, unsigned int bus,
                   const unsigned char *ports, size_t depth, unsigned int address)
{
    memset(state, 0, sizeof(*state));
    if (!depth || depth > 7) { errno = EINVAL; return -1; }
    int pos = snprintf(state->directory, sizeof(state->directory), "%s/%u-%u", root, bus, ports[0]);
    if (pos < 0 || (size_t)pos >= sizeof(state->directory)) { errno = ENAMETOOLONG; return -1; }
    for (size_t i = 1; i < depth; ++i) {
        int n = snprintf(state->directory + pos, sizeof(state->directory) - pos, ".%u", ports[i]);
        if (n < 0 || (size_t)n >= sizeof(state->directory) - pos) { errno = ENAMETOOLONG; return -1; }
        pos += n;
    }
    state->address = address;
    char value[32];
    unsigned int actual;
    if (read_attribute(state->directory, "devnum", value, sizeof(value)) ||
        sscanf(value, "%u", &actual) != 1 || actual != address) { errno = ENODEV; return -1; }
    if (read_attribute(state->directory, "power/control", value, sizeof(value))) return -1;
    if (!strncmp(value, "auto", 4)) {
        if (write_control(state->directory, "on\n")) return -1;
        state->changed = true;
    }
    return 0;
}

void usb_power_restore(usb_power_state *state)
{
    if (!state->changed) return;
    char value[32];
    unsigned int actual;
    /* A reboot may have put a different USB device at this same port. */
    if (!read_attribute(state->directory, "devnum", value, sizeof(value)) &&
        sscanf(value, "%u", &actual) == 1 && actual == state->address)
        (void)write_control(state->directory, "auto\n");
    state->changed = false;
}
#else
typedef int usb_power_linux_only;
#endif
