// List and remove pairings on a Logitech Bolt receiver (046d:c548) via HID++ 1.0.
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDManager.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VID 0x046D
#define PID 0xC548

static IOHIDDeviceRef dev;
static uint8_t inbuf[64];
#define QMAX 256
static uint8_t q[QMAX][32];
static int qlen[QMAX];
static int qhead = 0, qtail = 0;

static void on_input(void *ctx, IOReturn r, void *sender, IOHIDReportType type,
                     uint32_t id, uint8_t *report, CFIndex len) {
    (void)ctx; (void)r; (void)sender; (void)type; (void)id;
    int n = len > 32 ? 32 : (int)len;
    memcpy(q[qtail], report, n);
    qlen[qtail] = n;
    qtail = (qtail + 1) % QMAX;
}

static void dump(const char *tag, const uint8_t *b, int n) {
    printf("%s", tag);
    for (int i = 0; i < n; i++) printf(" %02X", b[i]);
    printf("\n");
}

static int verbose = 0;

// Returns reply length, -1 on HID++ error (code in *err), 0 on timeout.
static int request(int longmsg, uint8_t sub, uint8_t addr, const uint8_t *p, int plen,
                   int match_p0, uint8_t *out, int *err) {
    uint8_t buf[20] = {0};
    int len = longmsg ? 20 : 7;
    buf[0] = longmsg ? 0x11 : 0x10;
    buf[1] = 0xFF;
    buf[2] = sub;
    buf[3] = addr;
    memcpy(buf + 4, p, plen);
    if (verbose) dump(">>", buf, len);
    qhead = qtail;
    IOReturn r = IOHIDDeviceSetReport(dev, kIOHIDReportTypeOutput, buf[0], buf, len);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "SetReport failed: 0x%08X\n", r);
        return 0;
    }
    for (int t = 0; t < 40; t++) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
        while (qhead != qtail) {
            uint8_t *m = q[qhead];
            int n = qlen[qhead];
            qhead = (qhead + 1) % QMAX;
            if (verbose) dump("<<", m, n);
            if (n < 5 || m[1] != 0xFF) continue;
            if (m[2] == 0x8F && m[3] == sub && m[4] == addr) {
                if (err) *err = m[5];
                return -1;
            }
            if (m[2] == sub && m[3] == addr && (!match_p0 || m[4] == p[0])) {
                memcpy(out, m, n);
                return n;
            }
        }
    }
    return 0;
}

static const char *kind_name(int k) {
    static const char *names[] = {"unknown", "keyboard", "mouse", "numpad", "presenter",
                                  "remote", "trackball", "touchpad", "tablet", "gamepad",
                                  "joystick", "headset"};
    return (k >= 0 && k < 12) ? names[k] : "?";
}

static int open_receiver(void) {
    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    CFMutableDictionaryRef match = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    int vid = VID, pid = PID, up = 0xFF00;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &vid);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &pid);
    CFNumberRef nu = CFNumberCreate(NULL, kCFNumberIntType, &up);
    CFDictionarySetValue(match, CFSTR(kIOHIDVendorIDKey), nv);
    CFDictionarySetValue(match, CFSTR(kIOHIDProductIDKey), np);
    CFDictionarySetValue(match, CFSTR(kIOHIDPrimaryUsagePageKey), nu);
    IOHIDManagerSetDeviceMatching(mgr, match);
    IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    CFSetRef set = IOHIDManagerCopyDevices(mgr);
    if (!set || CFSetGetCount(set) == 0) {
        fprintf(stderr, "Bolt receiver HID++ interface not found\n");
        return 0;
    }
    CFIndex cnt = CFSetGetCount(set);
    IOHIDDeviceRef *devs = calloc(cnt, sizeof(*devs));
    CFSetGetValues(set, (const void **)devs);
    dev = devs[0];
    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "IOHIDDeviceOpen failed: 0x%08X\n", r);
        return 0;
    }
    IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    IOHIDDeviceRegisterInputReportCallback(dev, inbuf, sizeof(inbuf), on_input, NULL);
    return 1;
}

static int list_slots(void) {
    uint8_t out[32];
    int err = 0, paired = 0;
    // register 0x02: number of connected devices
    uint8_t z[3] = {0};
    int n = request(0, 0x81, 0x02, z, 3, 0, out, &err);
    if (n > 0) printf("receiver reports %d device(s) currently connected\n", out[5]);
    // register 0xB5: 0x50+slot = pairing info, 0x60+slot = device name
    for (int slot = 1; slot <= 6; slot++) {
        uint8_t p[3] = {(uint8_t)(0x50 + slot), 0, 0};
        err = 0;
        n = request(0, 0x83, 0xB5, p, 3, 1, out, &err);
        if (n == -1) {
            printf("slot %d: empty (err 0x%02X)\n", slot, err);
            continue;
        }
        if (n == 0) {
            printf("slot %d: no reply\n", slot);
            continue;
        }
        if (verbose) dump("   info", out, n);
        int kind = out[5] & 0x0F;
        int wpid = (out[7] << 8) | out[6];
        char name[32] = "";
        uint8_t pn[3] = {(uint8_t)(0x60 + slot), 0x01, 0};
        uint8_t o2[32];
        int n2 = request(0, 0x83, 0xB5, pn, 3, 1, o2, &err);
        if (n2 > 6) {
            int l = o2[6];
            if (l > 13) l = 13;
            memcpy(name, o2 + 7, l);
            name[l] = 0;
        }
        printf("slot %d: PAIRED  kind=%s  wpid=%04X  serial=%02X%02X%02X%02X  name=\"%s\"\n", slot,
               kind_name(kind), wpid, out[8], out[9], out[10], out[11], name);
        paired++;
    }
    printf("total paired: %d\n", paired);
    return paired;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s [-v] list | unpair <slot>\n", argv[0]);
        return 2;
    }
    int ai = 1;
    if (!strcmp(argv[ai], "-v")) { verbose = 1; ai++; }
    if (!open_receiver()) return 1;
    if (!strcmp(argv[ai], "list")) {
        list_slots();
        return 0;
    }
    if (!strcmp(argv[ai], "unpair") && ai + 1 < argc) {
        int slot = atoi(argv[ai + 1]);
        if (slot < 1 || slot > 6) { fprintf(stderr, "slot must be 1-6\n"); return 2; }
        uint8_t p[2] = {0x03, (uint8_t)slot}; // register 0xC1, action 0x03 = unpair
        uint8_t out[32];
        int err = 0;
        int n = request(1, 0x82, 0xC1, p, 2, 0, out, &err);
        if (n > 0) printf("slot %d: unpair acknowledged\n", slot);
        else if (n == -1) printf("slot %d: unpair failed, HID++ error 0x%02X\n", slot, err);
        else printf("slot %d: no reply\n", slot);
        // drain the receiver's follow-up notifications (shown with -v)
        for (int t = 0; t < 10; t++) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
            while (qhead != qtail) {
                if (verbose) dump("<< notif", q[qhead], qlen[qhead]);
                qhead = (qhead + 1) % QMAX;
            }
        }
        return n > 0 ? 0 : 1;
    }
    fprintf(stderr, "unknown command\n");
    return 2;
}
