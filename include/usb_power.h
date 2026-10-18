#ifndef PALERA1N_USB_POWER_H
#define PALERA1N_USB_POWER_H
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    char directory[4096];
    unsigned int address;
    bool changed;
} usb_power_state;

/* Hold runtime PM only for this enumerated device, never the whole controller. */
int usb_power_hold(usb_power_state *state, const char *root, unsigned int bus,
                   const unsigned char *ports, size_t depth, unsigned int address);
void usb_power_restore(usb_power_state *state);
#endif
