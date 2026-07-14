// xreal-ctl — a tiny standalone control helper for the XREAL Air glasses family (WP-XR1).
//
// It speaks the glasses' HID CONTROL protocol directly (via hidapi) to switch the display mode
// (2D mono 1920x1080 <-> 3D SBS 3840x1080) and set brightness, WITHOUT a running monado-service.
// This is the FLAT side of the flat<->XR toggle (scripts/xreal-mode.sh): when monado owns the HID
// device it drives these itself, but when the glasses are used as an ordinary head-locked monitor
// nothing else is holding the device, so we need a standalone way to command 2D and back.
//
// The packet framing, message IDs, CRC-32 table, brightness scaling, and the Ultra's control-interface
// number are lifted verbatim from the vendored monado xreal_air driver
// (subprojects/monado/src/xrt/drivers/xreal_air/) so the bytes on the wire are identical to what the
// driver sends. Do NOT run this while monado-service (or another XR driver) holds the HID device —
// only one owner of the 3318:xxxx control interface at a time.
//
// Build:  make -C contrib/xreal   (or: cc -O2 -o xreal-ctl xreal-ctl.c $(pkg-config --cflags --libs hidapi-hidraw))
// Usage:  xreal-ctl mode 2d|3d        set display mode (2d = mono, 3d = SBS stereo)
//         xreal-ctl brightness N       set brightness, N in 0..100 (mapped to the device's 0..7 scale)
//         xreal-ctl detect             print whether a supported device + control interface is present
// Exit codes: 0 ok, 1 device/IO error, 2 usage error.

#include <hidapi/hidapi.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

// ---- constants from xreal_air_interface.h / xreal_air_hmd.h ----
#define XREAL_AIR_VID 0x3318

// Product IDs and the CONTROL interface number for each (target_builder_xreal_air.c: the Ultra uses
// control interface 0, the Air/Air2/Pro use interface 4). We match on any of these.
struct dev_id {
    uint16_t pid;
    int      control_iface;
    const char* name;
};
static const struct dev_id DEVICES[] = {
    {0x0424, 4, "Air"},
    {0x0428, 4, "Air 2"},
    {0x0432, 4, "Air 2 Pro"},
    {0x0426, 0, "Air 2 Ultra"},
};
static const size_t NUM_DEVICES = sizeof(DEVICES) / sizeof(DEVICES[0]);

#define XREAL_AIR_MSG_W_BRIGHTNESS 0x04
#define XREAL_AIR_MSG_W_DISP_MODE  0x08
#define XREAL_AIR_DISPLAY_MODE_2D  0x1
#define XREAL_AIR_DISPLAY_MODE_3D  0x3
#define XREAL_AIR_BRIGHTNESS_MAX   7

#define CONTROL_HEAD        0xFD
#define CONTROL_BUFFER_SIZE 64

// ---- CRC-32 (verbatim from xreal_air_hmd.c) ----
static const uint32_t crc32_table[256] = {
    0x00000000, 0x77073096, 0xEE0E612C, 0x990951BA, 0x076DC419, 0x706AF48F, 0xE963A535, 0x9E6495A3, 0x0EDB8832,
    0x79DCB8A4, 0xE0D5E91E, 0x97D2D988, 0x09B64C2B, 0x7EB17CBD, 0xE7B82D07, 0x90BF1D91, 0x1DB71064, 0x6AB020F2,
    0xF3B97148, 0x84BE41DE, 0x1ADAD47D, 0x6DDDE4EB, 0xF4D4B551, 0x83D385C7, 0x136C9856, 0x646BA8C0, 0xFD62F97A,
    0x8A65C9EC, 0x14015C4F, 0x63066CD9, 0xFA0F3D63, 0x8D080DF5, 0x3B6E20C8, 0x4C69105E, 0xD56041E4, 0xA2677172,
    0x3C03E4D1, 0x4B04D447, 0xD20D85FD, 0xA50AB56B, 0x35B5A8FA, 0x42B2986C, 0xDBBBC9D6, 0xACBCF940, 0x32D86CE3,
    0x45DF5C75, 0xDCD60DCF, 0xABD13D59, 0x26D930AC, 0x51DE003A, 0xC8D75180, 0xBFD06116, 0x21B4F4B5, 0x56B3C423,
    0xCFBA9599, 0xB8BDA50F, 0x2802B89E, 0x5F058808, 0xC60CD9B2, 0xB10BE924, 0x2F6F7C87, 0x58684C11, 0xC1611DAB,
    0xB6662D3D, 0x76DC4190, 0x01DB7106, 0x98D220BC, 0xEFD5102A, 0x71B18589, 0x06B6B51F, 0x9FBFE4A5, 0xE8B8D433,
    0x7807C9A2, 0x0F00F934, 0x9609A88E, 0xE10E9818, 0x7F6A0DBB, 0x086D3D2D, 0x91646C97, 0xE6635C01, 0x6B6B51F4,
    0x1C6C6162, 0x856530D8, 0xF262004E, 0x6C0695ED, 0x1B01A57B, 0x8208F4C1, 0xF50FC457, 0x65B0D9C6, 0x12B7E950,
    0x8BBEB8EA, 0xFCB9887C, 0x62DD1DDF, 0x15DA2D49, 0x8CD37CF3, 0xFBD44C65, 0x4DB26158, 0x3AB551CE, 0xA3BC0074,
    0xD4BB30E2, 0x4ADFA541, 0x3DD895D7, 0xA4D1C46D, 0xD3D6F4FB, 0x4369E96A, 0x346ED9FC, 0xAD678846, 0xDA60B8D0,
    0x44042D73, 0x33031DE5, 0xAA0A4C5F, 0xDD0D7CC9, 0x5005713C, 0x270241AA, 0xBE0B1010, 0xC90C2086, 0x5768B525,
    0x206F85B3, 0xB966D409, 0xCE61E49F, 0x5EDEF90E, 0x29D9C998, 0xB0D09822, 0xC7D7A8B4, 0x59B33D17, 0x2EB40D81,
    0xB7BD5C3B, 0xC0BA6CAD, 0xEDB88320, 0x9ABFB3B6, 0x03B6E20C, 0x74B1D29A, 0xEAD54739, 0x9DD277AF, 0x04DB2615,
    0x73DC1683, 0xE3630B12, 0x94643B84, 0x0D6D6A3E, 0x7A6A5AA8, 0xE40ECF0B, 0x9309FF9D, 0x0A00AE27, 0x7D079EB1,
    0xF00F9344, 0x8708A3D2, 0x1E01F268, 0x6906C2FE, 0xF762575D, 0x806567CB, 0x196C3671, 0x6E6B06E7, 0xFED41B76,
    0x89D32BE0, 0x10DA7A5A, 0x67DD4ACC, 0xF9B9DF6F, 0x8EBEEFF9, 0x17B7BE43, 0x60B08ED5, 0xD6D6A3E8, 0xA1D1937E,
    0x38D8C2C4, 0x4FDFF252, 0xD1BB67F1, 0xA6BC5767, 0x3FB506DD, 0x48B2364B, 0xD80D2BDA, 0xAF0A1B4C, 0x36034AF6,
    0x41047A60, 0xDF60EFC3, 0xA867DF55, 0x316E8EEF, 0x4669BE79, 0xCB61B38C, 0xBC66831A, 0x256FD2A0, 0x5268E236,
    0xCC0C7795, 0xBB0B4703, 0x220216B9, 0x5505262F, 0xC5BA3BBE, 0xB2BD0B28, 0x2BB45A92, 0x5CB36A04, 0xC2D7FFA7,
    0xB5D0CF31, 0x2CD99E8B, 0x5BDEAE1D, 0x9B64C2B0, 0xEC63F226, 0x756AA39C, 0x026D930A, 0x9C0906A9, 0xEB0E363F,
    0x72076785, 0x05005713, 0x95BF4A82, 0xE2B87A14, 0x7BB12BAE, 0x0CB61B38, 0x92D28E9B, 0xE5D5BE0D, 0x7CDCEFB7,
    0x0BDBDF21, 0x86D3D2D4, 0xF1D4E242, 0x68DDB3F8, 0x1FDA836E, 0x81BE16CD, 0xF6B9265B, 0x6FB077E1, 0x18B74777,
    0x88085AE6, 0xFF0F6A70, 0x66063BCA, 0x11010B5C, 0x8F659EFF, 0xF862AE69, 0x616BFFD3, 0x166CCF45, 0xA00AE278,
    0xD70DD2EE, 0x4E048354, 0x3903B3C2, 0xA7672661, 0xD06016F7, 0x4969474D, 0x3E6E77DB, 0xAED16A4A, 0xD9D65ADC,
    0x40DF0B66, 0x37D83BF0, 0xA9BCAE53, 0xDEBB9EC5, 0x47B2CF7F, 0x30B5FFE9, 0xBDBDF21C, 0xCABAC28A, 0x53B39330,
    0x24B4A3A6, 0xBAD03605, 0xCDD70693, 0x54DE5729, 0x23D967BF, 0xB3667A2E, 0xC4614AB8, 0x5D681B02, 0x2A6F2B94,
    0xB40BBE37, 0xC30C8EA1, 0x5A05DF1B, 0x2D02EF8D};

static uint32_t crc32_checksum(const uint8_t* buf, uint32_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t i = 0; i < len; i++) {
        const uint8_t t = (crc ^ buf[i]) & 0xFFu;
        crc             = ((crc >> 8u) & 0xFFFFFFu) ^ crc32_table[t];
    }
    return ~crc;
}

// Build + send a CONTROL payload (mirrors send_payload_to_control in xreal_air_hmd.c).
static int send_control(hid_device* dev, uint16_t msgid, const uint8_t* data, uint8_t len) {
    uint8_t        payload[CONTROL_BUFFER_SIZE];
    memset(payload, 0, sizeof(payload));
    const uint16_t packet_len  = (uint16_t)(17 + len);
    const uint16_t payload_len = (uint16_t)(5 + packet_len);

    payload[0] = CONTROL_HEAD;
    payload[5] = (uint8_t)(packet_len & 0xFFu);
    payload[6] = (uint8_t)((packet_len >> 8u) & 0xFFu);
    // payload[7..14] timestamp — zeroed above.
    payload[15] = (uint8_t)(msgid & 0xFFu);
    payload[16] = (uint8_t)((msgid >> 8u) & 0xFFu);
    // payload[17..21] reserved — zeroed above.
    if (len)
        memcpy(payload + 22, data, len);

    const uint32_t checksum = crc32_checksum(payload + 5, packet_len);
    payload[1]              = (uint8_t)(checksum & 0xFFu);
    payload[2]              = (uint8_t)((checksum >> 8u) & 0xFFu);
    payload[3]              = (uint8_t)((checksum >> 16u) & 0xFFu);
    payload[4]              = (uint8_t)((checksum >> 24u) & 0xFFu);

    const int wrote = hid_write(dev, payload, payload_len);
    if (wrote < 0) {
        fprintf(stderr, "xreal-ctl: hid_write failed: %ls\n", hid_error(dev));
        return -1;
    }
    return 0;
}

// Open the CONTROL interface of the first supported device present. Returns matched device index, or
// -1. On success *out is the open handle.
static int open_control(hid_device** out, const struct dev_id** matched) {
    struct hid_device_info* devs = hid_enumerate(XREAL_AIR_VID, 0x0);
    for (struct hid_device_info* cur = devs; cur; cur = cur->next) {
        for (size_t i = 0; i < NUM_DEVICES; i++) {
            if (cur->product_id == DEVICES[i].pid && cur->interface_number == DEVICES[i].control_iface) {
                hid_device* h = hid_open_path(cur->path);
                if (!h) {
                    fprintf(stderr, "xreal-ctl: found %s but could not open its control interface (%s).\n"
                                    "           Is the 70-xreal.rules udev rule installed? Is monado holding the device?\n",
                            DEVICES[i].name, cur->path);
                    hid_free_enumeration(devs);
                    return -1;
                }
                *out     = h;
                *matched = &DEVICES[i];
                hid_free_enumeration(devs);
                return (int)i;
            }
        }
    }
    hid_free_enumeration(devs);
    fprintf(stderr, "xreal-ctl: no supported XREAL device found (VID 3318). Is it plugged in?\n");
    return -1;
}

static void usage(void) {
    fprintf(stderr, "usage: xreal-ctl mode 2d|3d | brightness 0..100 | detect\n");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 2;
    }

    if (hid_init() != 0) {
        fprintf(stderr, "xreal-ctl: hid_init failed\n");
        return 1;
    }

    int rc = 1;

    if (strcmp(argv[1], "detect") == 0) {
        hid_device*          dev = NULL;
        const struct dev_id* m   = NULL;
        if (open_control(&dev, &m) >= 0) {
            printf("XREAL %s present (control interface open)\n", m->name);
            hid_close(dev);
            rc = 0;
        } else
            rc = 1;
    } else if (strcmp(argv[1], "mode") == 0) {
        if (argc < 3) { usage(); hid_exit(); return 2; }
        uint8_t mode;
        if (strcmp(argv[2], "2d") == 0)
            mode = XREAL_AIR_DISPLAY_MODE_2D;
        else if (strcmp(argv[2], "3d") == 0)
            mode = XREAL_AIR_DISPLAY_MODE_3D;
        else { usage(); hid_exit(); return 2; }

        hid_device*          dev = NULL;
        const struct dev_id* m   = NULL;
        if (open_control(&dev, &m) >= 0) {
            rc = send_control(dev, XREAL_AIR_MSG_W_DISP_MODE, &mode, 1) == 0 ? 0 : 1;
            if (rc == 0)
                printf("XREAL %s: display mode -> %s\n", m->name, mode == XREAL_AIR_DISPLAY_MODE_3D ? "3D SBS (3840x1080)" : "2D mono (1920x1080)");
            hid_close(dev);
        }
    } else if (strcmp(argv[1], "brightness") == 0) {
        if (argc < 3) { usage(); hid_exit(); return 2; }
        char* end = NULL;
        long  n   = strtol(argv[2], &end, 10);
        if (end == argv[2] || *end != '\0' || n < 0 || n > 100) {
            fprintf(stderr, "xreal-ctl: brightness must be an integer 0..100\n");
            hid_exit();
            return 2;
        }
        // Map 0..100 to the device's 0..7 raw scale (matches monado unscale_brightness rounding).
        const uint8_t raw = (uint8_t)((double)n / 100.0 * XREAL_AIR_BRIGHTNESS_MAX + 0.5);

        hid_device*          dev = NULL;
        const struct dev_id* m   = NULL;
        if (open_control(&dev, &m) >= 0) {
            rc = send_control(dev, XREAL_AIR_MSG_W_BRIGHTNESS, &raw, 1) == 0 ? 0 : 1;
            if (rc == 0)
                printf("XREAL %s: brightness -> %ld/100 (raw %u/7)\n", m->name, n, raw);
            hid_close(dev);
        }
    } else {
        usage();
        hid_exit();
        return 2;
    }

    hid_exit();
    return rc;
}
