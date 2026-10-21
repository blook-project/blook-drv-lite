/*
 * blook-drv-lite - test mapper ABI (shared by drvlite-mapper.sys and
 * drvlite-mapper-cli.exe).
 *
 * This is a *test harness*, not part of the production load model: it is a
 * normal test-signed driver that exposes one device so the e2e suite can
 * manual-map the lite driver. The production path maps the image from a
 * vulnerable-driver primitive (blook-kdu) with no device at all.
 */
#ifndef BLOOK_DRVLITE_MAPPER_ABI_H
#define BLOOK_DRVLITE_MAPPER_ABI_H

#define DRVLITE_MAPPER_DEVICE_NAME "\\Device\\DrvliteMapper"
#define DRVLITE_MAPPER_SYMLINK_NAME "\\??\\DrvliteMapper"
#define DRVLITE_MAPPER_WIN32_NAME "\\\\.\\DrvliteMapper"
#define DRVLITE_MAPPER_DEVICE_TYPE 0x8A00

#define DRVLITE_MAPPER_CTL_CODE(device_type, function, method, access) \
    (((device_type) << 16) | ((access) << 14) | ((function) << 2) | (method))

/* METHOD_NEITHER (0): the driver probes/copies the caller buffers. */
#define IOCTL_DRVLITE_MAPPER_MAP \
    DRVLITE_MAPPER_CTL_CODE(DRVLITE_MAPPER_DEVICE_TYPE, 0x801, 0, 3)
#define IOCTL_DRVLITE_MAPPER_UNMAP \
    DRVLITE_MAPPER_CTL_CODE(DRVLITE_MAPPER_DEVICE_TYPE, 0x802, 0, 3)

#pragma pack(push, 8)

/*
 * Input layout:
 *   drvlite_map_request
 *   image bytes              (at image_offset)
 *   bootstrap bytes          (at bootstrap_offset)
 */
typedef struct drvlite_map_request {
    unsigned int image_size;
    unsigned int bootstrap_size;
    unsigned int flags; /* 1 = keep image mapped (always today) */
    unsigned int reserved;
    unsigned long long image_offset;
    unsigned long long bootstrap_offset;
} drvlite_map_request;

typedef struct drvlite_map_response {
    unsigned int status; /* NTSTATUS */
    unsigned int method; /* 1 = executable pool, 2 = contiguous */
    unsigned long long mapped_base;
    unsigned int image_size;
    unsigned int reserved;
} drvlite_map_response;

typedef struct drvlite_unmap_request {
    unsigned long long mapped_base;
} drvlite_unmap_request;

typedef struct drvlite_unmap_response {
    unsigned int status; /* NTSTATUS */
    unsigned int reserved;
} drvlite_unmap_response;

#pragma pack(pop)

#endif /* BLOOK_DRVLITE_MAPPER_ABI_H */
