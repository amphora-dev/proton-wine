/*
 * aarch64-native Amphora WSI bridge (APK-packaged as libamphora_wsi.so).
 *
 * Source of truth also lives in proton-wine dlls/wineandroid.drv/amphora_wsi_bridge.c.
 * Box64 guests cannot dlopen this aarch64 .so; Amphora LD_PRELOADs
 * nativeLibraryDir/libamphora_wsi.so so the ctor serves the unix present bridge.
 *
 * Wine's x86_64 sock-proxy ANW cannot be passed through Box64 into host
 * vkCreateAndroidSurfaceKHR (vtable is guest-ABI). This helper lives in the
 * box64 process as a native ARM64 .so: it rebuilds an aarch64 ANativeWindow
 * over the same Amphora client sock and creates the Android surface on THAT
 * hwnd ANW (no ImageReader blit hop) so PE QueuePresent DEQUEUE/QUEUEs the
 * dedicated Amphora window Surface directly.
 *
 * knife13: CreateSwapchain imports sock-proxy AHBs as VkImages via
 * VK_ANDROID_external_memory_android_hardware_buffer (Winlator vk_image
 * route). No GraphicBuffer/VkLayer/runtime hooks. Wine CreateSwapchain calls amphora_ahb_sc_*.
 */
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>
#include <stdio.h>
#include <vulkan/vulkan.h>

#ifndef VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR
#define VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR 1000008000
#endif
typedef struct VkAndroidSurfaceCreateInfoKHR_bridge {
    VkStructureType sType;
    const void *pNext;
    VkFlags flags;
    void *window;
} VkAndroidSurfaceCreateInfoKHR_bridge;
typedef VkResult (*PFN_bridge_vkCreateAndroidSurfaceKHR)(VkInstance, const VkAndroidSurfaceCreateInfoKHR_bridge *,
                                                         const VkAllocationCallbacks *, VkSurfaceKHR *);

#define LOG_TAG "WineAndroidWsi"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define ANDROID_NATIVE_MAKE_CONSTANT(a,b,c,d) \
    (((unsigned)(a)<<24)|((unsigned)(b)<<16)|((unsigned)(c)<<8)|(unsigned)(d))
#define ANDROID_NATIVE_WINDOW_MAGIC ANDROID_NATIVE_MAKE_CONSTANT('_','w','n','d')
#define ANDROID_NATIVE_BUFFER_MAGIC ANDROID_NATIVE_MAKE_CONSTANT('_','b','f','r')

#define AMPHORA_BUF_DEQUEUE 1
#define AMPHORA_BUF_QUEUE   2
#define AMPHORA_BUF_CANCEL  3
#define AMPHORA_BUF_QUERY   4
#define AMPHORA_BUF_PERFORM 5
#define AMPHORA_BUF_SET_SWAP 6
#define NB_CACHED_BUFFERS 8

enum {
    NATIVE_WINDOW_WIDTH = 0,
    NATIVE_WINDOW_HEIGHT = 1,
    NATIVE_WINDOW_FORMAT = 2,
    NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS = 3,
    NATIVE_WINDOW_SET_USAGE = 0,
    NATIVE_WINDOW_CONNECT = 1,
    NATIVE_WINDOW_DISCONNECT = 2,
    NATIVE_WINDOW_SET_BUFFER_COUNT = 4,
    NATIVE_WINDOW_SET_BUFFERS_GEOMETRY = 5,
    NATIVE_WINDOW_SET_BUFFERS_TRANSFORM = 6,
    NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP = 7,
    NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS = 8,
    NATIVE_WINDOW_SET_BUFFERS_FORMAT = 9,
    NATIVE_WINDOW_SET_SCALING_MODE = 10,
    NATIVE_WINDOW_API_CONNECT = 13,
    NATIVE_WINDOW_API_DISCONNECT = 14,
    NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS = 15
};

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

struct android_native_base_t {
    int magic;
    int version;
    void *reserved[4];
    void (*incRef)(struct android_native_base_t *base);
    void (*decRef)(struct android_native_base_t *base);
};

/* Modern AOSP nativebase layout (usage_deprecated + layerCount + uint64 usage).
 * Old Wine layout aliased layerCount as reserved[0]; handle offset is the same,
 * but version/magic checks and field names must match the platform ANWB. */
struct ANativeWindowBuffer {
    struct android_native_base_t common;
    int width, height, stride, format, usage_deprecated;
    uintptr_t layerCount;
    void *reserved[1];
    const native_handle_t *handle;
    uint64_t usage;
    void *reserved_proc[8 - (sizeof(uint64_t) / sizeof(void *))];
};

struct ANativeWindow {
    struct android_native_base_t common;
    uint32_t flags;
    int minSwapInterval, maxSwapInterval;
    float xdpi, ydpi;
    intptr_t oem[4];
    int (*setSwapInterval)(struct ANativeWindow *window, int interval);
    int (*dequeueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer);
    int (*lockBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*queueBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*query)(const struct ANativeWindow *window, int what, int *value);
    int (*perform)(struct ANativeWindow *window, int operation, ...);
    int (*cancelBuffer_DEPRECATED)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer);
    int (*dequeueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fenceFd);
    int (*queueBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
    int (*cancelBuffer)(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fenceFd);
};

/* knife12e: AHB-import swapchain owns GPU binding. ANW dequeue only has to
 * carry the sock-proxy AHB (and a synthetic ANWB so (amphora_buf*)anwb works).
 * Do NOT convert AHB→GraphicBuffer or guess OEM ANWB layout — ICD never
 * dequeues these if AHB_SC CreateSwapchain succeeds. */
struct amphora_buf {
    struct ANativeWindowBuffer buffer; /* first member: AHB_SC casts anwb→buf */
    void *ahb;                         /* NDK AHardwareBuffer* we own (recv/CLONE) */
    native_handle_t *nh;               /* only if AHB path unavailable */
    int buffer_id;
    int generation;
    int width, height, stride, format, usage;
    int ref;
};

struct amphora_win {
    struct ANativeWindow win;
    int sock;
    int ref;
    pthread_mutex_t lock;
    struct amphora_buf *bufs[NB_CACHED_BUFFERS];
    int req_w, req_h; /* 0 = unset; from SET_BUFFERS_* dimensions */
};

static int write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) return -1;
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int recv_with_fds(int fd, void *buf, size_t len, int *out_fds, int max_fds, int *n_fds)
{
    char *p = buf;
    size_t left = len;
    int got = 0;
    if (n_fds) *n_fds = 0;
    while (left) {
        struct msghdr msg;
        struct iovec iov;
        char control[CMSG_SPACE(sizeof(int) * 64)];
        struct cmsghdr *cmsg;
        ssize_t n;
        memset(&msg, 0, sizeof(msg));
        iov.iov_base = p;
        iov.iov_len = left;
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        n = recvmsg(fd, &msg, 0);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (!n) return -1;
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
                int cnt = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                int i;
                for (i = 0; i < cnt && got < max_fds; i++)
                    out_fds[got++] = ((int *)CMSG_DATA(cmsg))[i];
            }
        }
        p += n; left -= (size_t)n;
    }
    if (n_fds) *n_fds = got;
    return 0;
}

static void close_nh(native_handle_t *nh)
{
    int i;
    if (!nh) return;
    for (i = 0; i < nh->numFds; i++) if (nh->data[i] >= 0) close(nh->data[i]);
    free(nh);
}


/* Adreno Mapper5 rejects raw SCM_RIGHTS-rebuilt native_handles (no SnapHandle).
 * Import via AHardwareBuffer_createFromHandle(CLONE) so vkCreateImage works. */
enum { AHB_CREATE_FROM_HANDLE_CLONE = 2 };
typedef struct {
    uint32_t width, height, layers, format;
    uint64_t usage;
    uint32_t stride, rfu0;
    uint64_t rfu1;
} amphora_ahb_desc;
typedef int (*pfn_ahb_create_from_handle)(const amphora_ahb_desc *, const native_handle_t *,
                                          int32_t, void **);
typedef const native_handle_t *(*pfn_ahb_get_native_handle)(void *);
typedef void (*pfn_ahb_release)(void *);
typedef int (*pfn_ahb_recv)(int, void **);
typedef int (*pfn_ahb_lock)(void *buffer, uint64_t usage, int32_t fence,
                            const void *rect, void **outVirt);
typedef int (*pfn_ahb_unlock)(void *buffer, int32_t *fence);

static pfn_ahb_create_from_handle g_ahb_create_from_handle;
static pfn_ahb_get_native_handle g_ahb_get_native_handle;
static pfn_ahb_release g_ahb_release;
static pfn_ahb_recv g_ahb_recv;
static pfn_ahb_lock g_ahb_lock;
static pfn_ahb_unlock g_ahb_unlock;
static int g_ahb_resolved;
static int g_guest_client_queue_n; /* hwnd-sized client QUEUE count (knife7/8) */
static int g_guest_fence_log_n;    /* first ~20 client fence_in logs (knife8) */

/* knife8: sync_wait from libsync.so, else poll(fd) up to 3000ms. */
typedef int (*pfn_sync_wait)(int fence, int timeout_ms);
static pfn_sync_wait g_sync_wait;
static int g_sync_resolved;

static void amphora_resolve_sync(void)
{
    void *lib;
    if (g_sync_resolved) return;
    g_sync_resolved = 1;
    lib = dlopen("libsync.so", RTLD_NOW);
    if (!lib) {
        LOGW("dlopen libsync.so failed: %s — fence wait via poll", dlerror());
        return;
    }
    g_sync_wait = (pfn_sync_wait)dlsym(lib, "sync_wait");
    LOGI("sync_wait symbol=%p", (void *)g_sync_wait);
}

/* Wait present fence then close. fence ownership transferred here. */
static void amphora_wait_present_fence(int fence)
{
    int rc;
    if (fence < 0) {
        LOGI("guest-fence none");
        return;
    }
    LOGI("guest-fence wait fd=%d", fence);
    amphora_resolve_sync();
    if (g_sync_wait) {
        rc = g_sync_wait(fence, 3000);
        if (rc != 0)
            LOGW("guest-fence sync_wait fd=%d rc=%d errno=%d", fence, rc, errno);
    } else {
        struct pollfd p;
        p.fd = fence;
        p.events = POLLIN;
        p.revents = 0;
        do {
            rc = poll(&p, 1, 3000);
        } while (rc < 0 && errno == EINTR);
        if (rc < 0)
            LOGW("guest-fence poll fd=%d rc=%d errno=%d", fence, rc, errno);
        else if (rc == 0)
            LOGW("guest-fence poll fd=%d timeout 3000ms", fence);
    }
    close(fence);
}

enum { AMPHORA_AHB_NUMFDS = -1 };

/* NDK AHardwareBuffer usage CPU read bits (hardware_buffer.h). */
#ifndef AHARDWAREBUFFER_USAGE_CPU_READ_RARELY
#define AHARDWAREBUFFER_USAGE_CPU_READ_RARELY  2ULL
#endif
#ifndef AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN
#define AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN   3ULL
#endif
#ifndef AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY (2ULL << 4) /* 32 */
#endif
#ifndef AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN
#define AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN  (3ULL << 4) /* 48 */
#endif

static void amphora_resolve_ahb(void)
{
    void *lib;
    if (g_ahb_resolved) return;
    g_ahb_resolved = 1;
    lib = dlopen("libnativewindow.so", RTLD_NOW);
    if (!lib) {
        LOGE("dlopen libnativewindow.so failed: %s", dlerror());
        return;
    }
    g_ahb_create_from_handle = (pfn_ahb_create_from_handle)dlsym(lib, "AHardwareBuffer_createFromHandle");
    g_ahb_get_native_handle = (pfn_ahb_get_native_handle)dlsym(lib, "AHardwareBuffer_getNativeHandle");
    g_ahb_release = (pfn_ahb_release)dlsym(lib, "AHardwareBuffer_release");
    g_ahb_recv = (pfn_ahb_recv)dlsym(lib, "AHardwareBuffer_recvHandleFromUnixSocket");
    g_ahb_lock = (pfn_ahb_lock)dlsym(lib, "AHardwareBuffer_lock");
    g_ahb_unlock = (pfn_ahb_unlock)dlsym(lib, "AHardwareBuffer_unlock");
    LOGI("AHB symbols create=%p getNh=%p release=%p recv=%p lock=%p unlock=%p",
         (void *)g_ahb_create_from_handle, (void *)g_ahb_get_native_handle,
         (void *)g_ahb_release, (void *)g_ahb_recv,
         (void *)g_ahb_lock, (void *)g_ahb_unlock);
}

/* knife7: log native_handle fd identity (st_dev/st_ino/st_size) for fork match. */
static void amphora_log_nh_fstat(const char *tag, const native_handle_t *nh)
{
    int i, nints;
    if (!nh) {
        LOGW("%s nh=null", tag);
        return;
    }
    LOGI("%s numFds=%d numInts=%d", tag, nh->numFds, nh->numInts);
    for (i = 0; i < nh->numFds; i++) {
        struct stat st;
        if (fstat(nh->data[i], &st) == 0)
            LOGI("%s fd[%d]=%d st_dev=%llu st_ino=%llu st_size=%lld",
                 tag, i, nh->data[i],
                 (unsigned long long)st.st_dev,
                 (unsigned long long)st.st_ino,
                 (long long)st.st_size);
        else
            LOGW("%s fd[%d]=%d fstat errno=%d", tag, i, nh->data[i], errno);
    }
    nints = nh->numInts < 4 ? nh->numInts : 4;
    if (nints > 0) {
        int a = nh->data[nh->numFds + 0];
        int b = nints > 1 ? nh->data[nh->numFds + 1] : 0;
        int c = nints > 2 ? nh->data[nh->numFds + 2] : 0;
        int d = nints > 3 ? nh->data[nh->numFds + 3] : 0;
        LOGI("%s ints[0..3]=%d,%d,%d,%d (n=%d)", tag, a, b, c, d, nh->numInts);
    }
}

/* knife8: after fence-wait — sample guest AHB; if BLACK at q50/q100, CPU-fill magenta. */
static const char *amphora_class_rgba(int r, int g, int b)
{
    if ((r > 217 ? r - 217 : 217 - r) <= 40 &&
        (g > 26 ? g - 26 : 26 - g) <= 40 &&
        (b > 179 ? b - 179 : 179 - b) <= 40)
        return "MAGENTA";
    if (r >= 240 && g >= 240 && b >= 240)
        return "WHITE";
    if (r <= 15 && g <= 15 && b <= 15)
        return "BLACK";
    return "OTHER";
}

static void amphora_guest_queue_knife(struct amphora_buf *buf)
{
    int queue_n, w, h, stride_px, cx, cy, rc;
    void *bits = NULL;
    const uint8_t *p;
    uint8_t *wp;
    int r, g, b, a, r2, g2, b2;
    const char *cls;
    const native_handle_t *nh;
    int x0, y0, x1, y1, y, x;
    uint64_t wr_usage;

    if (!buf) return;
    w = buf->width;
    h = buf->height;
    /* Client DXGI smoke is 632x446; skip desktop/taskbar. */
    if (w < 200 || h < 200) return;
    if (w > 900 || h > 700) return;

    g_guest_client_queue_n++;
    queue_n = g_guest_client_queue_n;
    if (queue_n != 50 && queue_n != 100) return;

    amphora_resolve_ahb();

    LOGI("GUEST_ID id=%d gen=%d ahb=%p %dx%d stride=%d fmt=%d usage=0x%x",
         buf->buffer_id, buf->generation, buf->ahb,
         w, h, buf->stride, buf->format, buf->usage);

    if (buf->ahb && g_ahb_get_native_handle) {
        nh = g_ahb_get_native_handle(buf->ahb);
        amphora_log_nh_fstat("GUEST_ID", nh);
    } else if (buf->buffer.handle) {
        amphora_log_nh_fstat("GUEST_ID", (const native_handle_t *)buf->buffer.handle);
    } else {
        LOGW("GUEST_ID no nh id=%d", buf->buffer_id);
    }

    if (!buf->ahb) {
        LOGW("guest-readback no ahb queue_n=%d id=%d", queue_n, buf->buffer_id);
        return;
    }
    if (!g_ahb_lock || !g_ahb_unlock) {
        LOGW("guest-readback AHB lock symbols missing queue_n=%d", queue_n);
        return;
    }

    rc = g_ahb_lock(buf->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &bits);
    if (rc != 0 || !bits) {
        LOGW("guest-readback AHB_lock OFTEN rc=%d errno=%d — retry RARELY", rc, errno);
        bits = NULL;
        rc = g_ahb_lock(buf->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &bits);
    }
    if (rc != 0 || !bits) {
        LOGW("guest-readback AHB_lock FAIL rc=%d errno=%d queue_n=%d id=%d",
             rc, errno, queue_n, buf->buffer_id);
        return;
    }

    stride_px = buf->stride > 0 ? buf->stride : w;
    cx = w / 2;
    cy = h / 2;
    p = (const uint8_t *)bits + ((size_t)cy * (size_t)stride_px + (size_t)cx) * 4u;
    r = p[0]; g = p[1]; b = p[2]; a = p[3];
    p = (const uint8_t *)bits + ((size_t)16 * (size_t)stride_px + (size_t)16) * 4u;
    r2 = p[0]; g2 = p[1]; b2 = p[2];
    cls = amphora_class_rgba(r, g, b);

    LOGI("guest-readback CLASS=%s queue_n=%d id=%d centerRGBA=%d,%d,%d,%d "
         "tlRGBA=%d,%d,%d via=AHB_lock",
         cls, queue_n, buf->buffer_id, r, g, b, a, r2, g2, b2);

    g_ahb_unlock(buf->ahb, NULL);
    bits = NULL;

    /* knife8 bind probe: if still BLACK after fence-wait, CPU-fill magenta.
     * knife10: gate fill — success must not come from CPU fill unless AMPHORA_CPU_FILL=1. */
    if (cls[0] != 'B') /* not BLACK */
        return;

    {
        const char *fill_env = getenv("AMPHORA_CPU_FILL");
        if (!fill_env || fill_env[0] != '1' || fill_env[1] != '\0') {
            LOGI("GUEST_CPU_FILL skipped queue_n=%d (set AMPHORA_CPU_FILL=1 to enable)", queue_n);
            return;
        }
    }

    wr_usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    rc = g_ahb_lock(buf->ahb, wr_usage, -1, NULL, &bits);
    if (rc != 0 || !bits) {
        LOGW("GUEST_CPU_FILL WRITE_OFTEN rc=%d errno=%d — retry WRITE_RARELY", rc, errno);
        bits = NULL;
        wr_usage = AHARDWAREBUFFER_USAGE_CPU_READ_RARELY | AHARDWAREBUFFER_USAGE_CPU_WRITE_RARELY;
        rc = g_ahb_lock(buf->ahb, wr_usage, -1, NULL, &bits);
    }
    if (rc != 0 || !bits) {
        LOGW("GUEST_CPU_FILL AHB_lock FAIL rc=%d errno=%d queue_n=%d id=%d",
             rc, errno, queue_n, buf->buffer_id);
        return;
    }

    /* Fill center 128x128 (or full buffer if smaller). */
    x0 = cx - 64; if (x0 < 0) x0 = 0;
    y0 = cy - 64; if (y0 < 0) y0 = 0;
    x1 = cx + 64; if (x1 > w) x1 = w;
    y1 = cy + 64; if (y1 > h) y1 = h;
    for (y = y0; y < y1; y++) {
        wp = (uint8_t *)bits + ((size_t)y * (size_t)stride_px + (size_t)x0) * 4u;
        for (x = x0; x < x1; x++) {
            wp[0] = 217; wp[1] = 26; wp[2] = 179; wp[3] = 255;
            wp += 4;
        }
    }
    LOGI("GUEST_CPU_FILL queue_n=%d id=%d rect=%d,%d-%d,%d RGBA=217,26,179,255",
         queue_n, buf->buffer_id, x0, y0, x1, y1);
    g_ahb_unlock(buf->ahb, NULL);
    bits = NULL;

    rc = g_ahb_lock(buf->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, NULL, &bits);
    if (rc != 0 || !bits) {
        bits = NULL;
        rc = g_ahb_lock(buf->ahb, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &bits);
    }
    if (rc != 0 || !bits) {
        LOGW("guest-readback-after-fill lock FAIL rc=%d errno=%d", rc, errno);
        return;
    }
    p = (const uint8_t *)bits + ((size_t)cy * (size_t)stride_px + (size_t)cx) * 4u;
    r = p[0]; g = p[1]; b = p[2]; a = p[3];
    cls = amphora_class_rgba(r, g, b);
    LOGI("guest-readback-after-fill CLASS=%s queue_n=%d id=%d centerRGBA=%d,%d,%d,%d",
         cls, queue_n, buf->buffer_id, r, g, b, a);
    g_ahb_unlock(buf->ahb, NULL);
}

static int amphora_import_ahb(native_handle_t *nh, int width, int height, int stride,
                              int format, int usage, void **out_ahb, const native_handle_t **out_handle)
{
    amphora_ahb_desc desc;
    void *ahb = NULL;
    int rc;
    amphora_resolve_ahb();
    if (!g_ahb_create_from_handle || !g_ahb_get_native_handle || !g_ahb_release)
        return -ENOSYS;
    memset(&desc, 0, sizeof(desc));
    desc.width = (uint32_t)width;
    desc.height = (uint32_t)height;
    desc.layers = 1;
    desc.format = (uint32_t)format;
    desc.usage = (uint64_t)(uint32_t)usage;
    desc.stride = (uint32_t)stride;
    rc = g_ahb_create_from_handle(&desc, nh, AHB_CREATE_FROM_HANDLE_CLONE, &ahb);
    if (rc != 0 || !ahb) {
        LOGE("AHardwareBuffer_createFromHandle CLONE failed rc=%d ahb=%p", rc, ahb);
        return rc ? rc : -EINVAL;
    }
    *out_ahb = ahb;
    *out_handle = g_ahb_get_native_handle(ahb);
    if (!*out_handle) {
        LOGE("AHardwareBuffer_getNativeHandle returned null");
        g_ahb_release(ahb);
        *out_ahb = NULL;
        return -EINVAL;
    }
    return 0;
}

static void amphora_buf_release(struct amphora_buf *buf)
{
    if (!buf) return;
    if (__sync_sub_and_fetch(&buf->ref, 1) > 0) return;
    if (buf->ahb && g_ahb_release)
        g_ahb_release(buf->ahb);
    else
        close_nh(buf->nh);
    buf->ahb = NULL;
    buf->nh = NULL;
    free(buf);
}

static void buf_inc(struct android_native_base_t *base)
{
    struct amphora_buf *buf = (struct amphora_buf *)base;
    __sync_add_and_fetch(&buf->ref, 1);
}
static void buf_dec(struct android_native_base_t *base)
{
    amphora_buf_release((struct amphora_buf *)base);
}

static struct amphora_buf *buf_from_anwb(struct amphora_win *win, struct ANativeWindowBuffer *anwb)
{
    int i;
    struct amphora_buf *buf;
    if (!win || !anwb) return NULL;
    buf = (struct amphora_buf *)anwb; /* embedded ANativeWindowBuffer first member */
    if (buf->buffer.common.magic == (int)ANDROID_NATIVE_BUFFER_MAGIC)
        return buf;
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
        if (win->bufs[i] && &win->bufs[i]->buffer == anwb)
            return win->bufs[i];
    return NULL;
}

static void amphora_fill_syn_anwb(struct amphora_buf *buf, int width, int height,
                                 int stride, int format, int usage, const native_handle_t *handle)
{
    memset(&buf->buffer, 0, sizeof(buf->buffer));
    buf->buffer.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    buf->buffer.common.version = (int)sizeof(struct ANativeWindowBuffer);
    buf->buffer.common.incRef = buf_inc;
    buf->buffer.common.decRef = buf_dec;
    buf->buffer.width = width;
    buf->buffer.height = height;
    buf->buffer.stride = stride;
    buf->buffer.format = format;
    buf->buffer.usage_deprecated = usage;
    buf->buffer.layerCount = 1;
    buf->buffer.usage = (uint64_t)(uint32_t)usage;
    buf->buffer.handle = handle;
}

static void win_inc(struct android_native_base_t *base)
{
    struct amphora_win *win = (struct amphora_win *)base;
    __sync_add_and_fetch(&win->ref, 1);
}

static void win_dec(struct android_native_base_t *base)
{
    struct amphora_win *win = (struct amphora_win *)base;
    int i;
    if (__sync_sub_and_fetch(&win->ref, 1) > 0) return;
    for (i = 0; i < NB_CACHED_BUFFERS; i++) {
        if (win->bufs[i]) {
            amphora_buf_release(win->bufs[i]);
            win->bufs[i] = NULL;
        }
    }
    if (win->sock >= 0) close(win->sock);
    pthread_mutex_destroy(&win->lock);
    free(win);
}

static int win_dequeue(struct ANativeWindow *window, struct ANativeWindowBuffer **out, int *fence)
{
    struct amphora_win *win = (struct amphora_win *)window;
    int32_t cmd = AMPHORA_BUF_DEQUEUE;
    struct {
        int32_t status, width, height, stride, format, usage;
        int32_t buffer_id, generation, numFds, numInts;
    } hdr;
    int fds[64], n_fds = 0, *ints = NULL;
    native_handle_t *nh;
    struct amphora_buf *buf;
    size_t nh_size;
    const native_handle_t *ahb_nh;

    if (fence) *fence = -1;
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd))) { pthread_mutex_unlock(&win->lock); return -EIO; }
    if (recv_with_fds(win->sock, &hdr, sizeof(hdr), fds, 64, &n_fds)) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    if (hdr.status) { pthread_mutex_unlock(&win->lock); return hdr.status; }
    if (hdr.numFds == AMPHORA_AHB_NUMFDS) {
        if (n_fds != 0 || hdr.numInts != 0) {
            int i; for (i = 0; i < n_fds; i++) close(fds[i]);
            pthread_mutex_unlock(&win->lock); return -EINVAL;
        }
    } else if (hdr.numFds < 0 || hdr.numInts < 0 || hdr.numFds > 64 || hdr.numInts > 256 || n_fds != hdr.numFds) {
        int i; for (i = 0; i < n_fds; i++) close(fds[i]);
        pthread_mutex_unlock(&win->lock); return -EINVAL;
    } else if (hdr.numInts) {
        ints = malloc(sizeof(int) * (size_t)hdr.numInts);
        if (!ints || read_full(win->sock, ints, sizeof(int) * (size_t)hdr.numInts)) {
            int i; free(ints); for (i = 0; i < n_fds; i++) close(fds[i]);
            pthread_mutex_unlock(&win->lock); return -EIO;
        }
    }
    /* Reuse cached amphora_buf (same AHB) so AHB_SC image import stays 1:1. */
    if (hdr.buffer_id >= 0 && hdr.buffer_id < NB_CACHED_BUFFERS &&
        win->bufs[hdr.buffer_id] &&
        win->bufs[hdr.buffer_id]->generation == hdr.generation &&
        win->bufs[hdr.buffer_id]->ahb) {
        int i;
        for (i = 0; i < n_fds; i++) close(fds[i]);
        free(ints);
        if (hdr.numFds == AMPHORA_AHB_NUMFDS) {
            void *drop = NULL;
            amphora_resolve_ahb();
            if (g_ahb_recv && g_ahb_recv(win->sock, &drop) == 0 && drop && g_ahb_release)
                g_ahb_release(drop);
            else
                LOGE("dequeue id=%d REUSE failed to drain AHB_RECV", hdr.buffer_id);
        }
        buf = win->bufs[hdr.buffer_id];
        buf->width = hdr.width;
        buf->height = hdr.height;
        buf->stride = hdr.stride;
        buf->format = hdr.format;
        buf->usage = hdr.usage;
        amphora_fill_syn_anwb(buf, hdr.width, hdr.height, hdr.stride, hdr.format, hdr.usage,
                              buf->buffer.handle);
        *out = &buf->buffer;
        pthread_mutex_unlock(&win->lock);
        LOGI("dequeue id=%d REUSE %dx%d fmt=%d usage=0x%x anwb=%p ahb=%p",
             hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage,
             (void *)&buf->buffer, buf->ahb);
        return 0;
    }
    if (hdr.numFds == AMPHORA_AHB_NUMFDS) {
        void *ahb = NULL;
        int rrc;
        amphora_resolve_ahb();
        buf = calloc(1, sizeof(*buf));
        if (!buf) { pthread_mutex_unlock(&win->lock); return -ENOMEM; }
        if (!g_ahb_recv || !g_ahb_get_native_handle || !g_ahb_release) {
            LOGE("AHB_RECV symbols missing");
            free(buf); pthread_mutex_unlock(&win->lock); return -ENOSYS;
        }
        rrc = g_ahb_recv(win->sock, &ahb);
        if (rrc != 0 || !ahb) {
            LOGE("AHB_RECV recvHandle failed rc=%d", rrc);
            free(buf); pthread_mutex_unlock(&win->lock); return -EIO;
        }
        ahb_nh = g_ahb_get_native_handle(ahb);
        if (!ahb_nh) {
            LOGE("AHB_RECV getNativeHandle null");
            g_ahb_release(ahb); free(buf);
            pthread_mutex_unlock(&win->lock); return -EINVAL;
        }
        buf->ahb = ahb;
        buf->nh = NULL;
        buf->buffer_id = hdr.buffer_id;
        buf->generation = hdr.generation;
        buf->width = hdr.width;
        buf->height = hdr.height;
        buf->stride = hdr.stride;
        buf->format = hdr.format;
        buf->usage = hdr.usage;
        buf->ref = 1;
        amphora_fill_syn_anwb(buf, hdr.width, hdr.height, hdr.stride, hdr.format, hdr.usage, ahb_nh);
        if (hdr.buffer_id >= 0 && hdr.buffer_id < NB_CACHED_BUFFERS) {
            if (win->bufs[hdr.buffer_id])
                amphora_buf_release(win->bufs[hdr.buffer_id]);
            win->bufs[hdr.buffer_id] = buf;
            __sync_add_and_fetch(&buf->ref, 1);
        }
        *out = &buf->buffer;
        pthread_mutex_unlock(&win->lock);
        LOGI("dequeue id=%d AHB_RECV %dx%d fmt=%d usage=0x%x anwb=%p ahb=%p nh=%p",
             hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage,
             (void *)&buf->buffer, ahb, (void *)ahb_nh);
        LOGI("GUEST_RECV id=%d gen=%d ahb=%p %dx%d stride=%d fmt=%d usage=0x%x",
             hdr.buffer_id, hdr.generation, ahb,
             hdr.width, hdr.height, hdr.stride, hdr.format, hdr.usage);
        amphora_log_nh_fstat("GUEST_RECV", ahb_nh);
        return 0;
    }

    /* SCM_RIGHTS fallback: CLONE into AHB (same object AHB_SC will import). */
    nh_size = sizeof(native_handle_t) + sizeof(int) * (size_t)(hdr.numFds + hdr.numInts);
    nh = malloc(nh_size);
    buf = calloc(1, sizeof(*buf));
    if (!nh || !buf) {
        int i; free(nh); free(buf); free(ints);
        for (i = 0; i < n_fds; i++) close(fds[i]);
        pthread_mutex_unlock(&win->lock); return -ENOMEM;
    }
    nh->version = (int)sizeof(native_handle_t);
    nh->numFds = hdr.numFds;
    nh->numInts = hdr.numInts;
    if (hdr.numFds) memcpy(nh->data, fds, sizeof(int) * (size_t)hdr.numFds);
    if (hdr.numInts) memcpy(nh->data + hdr.numFds, ints, sizeof(int) * (size_t)hdr.numInts);
    free(ints);
    {
        void *ahb = NULL;
        const native_handle_t *imported = NULL;
        int irc = amphora_import_ahb(nh, hdr.width, hdr.height, hdr.stride,
                                     hdr.format, hdr.usage, &ahb, &imported);
        if (irc == 0 && ahb && imported) {
            close_nh(nh);
            nh = NULL;
            buf->ahb = ahb;
            buf->nh = NULL;
            amphora_fill_syn_anwb(buf, hdr.width, hdr.height, hdr.stride, hdr.format, hdr.usage, imported);
            LOGI("dequeue id=%d NEW_AHB_CLONE %dx%d fmt=%d usage=0x%x anwb=%p ahb=%p",
                 hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage,
                 (void *)&buf->buffer, ahb);
        } else {
            buf->ahb = NULL;
            buf->nh = nh;
            amphora_fill_syn_anwb(buf, hdr.width, hdr.height, hdr.stride, hdr.format, hdr.usage, nh);
            LOGW("dequeue id=%d NEW_RAW (no AHB) %dx%d fmt=%d usage=0x%x — AHB_SC import will fail",
                 hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage);
        }
    }
    buf->buffer_id = hdr.buffer_id;
    buf->generation = hdr.generation;
    buf->width = hdr.width;
    buf->height = hdr.height;
    buf->stride = hdr.stride;
    buf->format = hdr.format;
    buf->usage = hdr.usage;
    buf->ref = 1;

    if (hdr.buffer_id >= 0 && hdr.buffer_id < NB_CACHED_BUFFERS) {
        if (win->bufs[hdr.buffer_id])
            amphora_buf_release(win->bufs[hdr.buffer_id]);
        win->bufs[hdr.buffer_id] = buf;
        __sync_add_and_fetch(&buf->ref, 1);
    }
    *out = &buf->buffer;
    pthread_mutex_unlock(&win->lock);
    return 0;
}

static int win_queue(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence)
{
    struct amphora_win *win = (struct amphora_win *)window;
    struct amphora_buf *buf;
    int32_t cmd = AMPHORA_BUF_QUEUE, ret;
    int fence_in = fence;
    int w, h, is_client;

    pthread_mutex_lock(&win->lock);
    buf = buf_from_anwb(win, buffer);
    pthread_mutex_unlock(&win->lock);
    if (!buf) {
        LOGE("queue: unknown anwb=%p", (void *)buffer);
        if (fence >= 0) close(fence);
        return -EINVAL;
    }
    w = buf->width;
    h = buf->height;
    is_client = (w >= 200 && h >= 200 && w <= 900 && h <= 700);

    /* knife8: log fence_in on first ~20 client queues (before wait/close). */
    if (is_client && g_guest_fence_log_n < 20) {
        g_guest_fence_log_n++;
        LOGI("queue id=%d fence_in=%d anwb=%p ahb=%p handle=%p",
             buf->buffer_id, fence_in, (void *)&buf->buffer, buf->ahb,
             (void *)buf->buffer.handle);
    }

    /* knife8: wait present fence BEFORE QUEUE sock and guest-readback. */
    amphora_wait_present_fence(fence);

    /* knife8: sample (and maybe CPU-fill) after GPU fence signaled. */
    amphora_guest_queue_knife(buf);
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &buf->buffer_id, sizeof(buf->buffer_id)) ||
        write_full(win->sock, &buf->generation, sizeof(buf->generation)) ||
        read_full(win->sock, &ret, sizeof(ret))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
    LOGI("queue id=%d ret=%d", buf->buffer_id, ret);
    return ret;
}

static int win_cancel(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence)
{
    struct amphora_win *win = (struct amphora_win *)window;
    struct amphora_buf *buf;
    int32_t cmd = AMPHORA_BUF_CANCEL, ret;
    pthread_mutex_lock(&win->lock);
    buf = buf_from_anwb(win, buffer);
    pthread_mutex_unlock(&win->lock);
    if (!buf) {
        LOGE("cancel: unknown anwb=%p", (void *)buffer);
        if (fence >= 0) close(fence);
        return -EINVAL;
    }
    /* knife8: wait present fence before cancel sock (same as queue). */
    amphora_wait_present_fence(fence);
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &buf->buffer_id, sizeof(buf->buffer_id)) ||
        write_full(win->sock, &buf->generation, sizeof(buf->generation)) ||
        read_full(win->sock, &ret, sizeof(ret))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
    return ret;
}

static int win_dequeue_dep(struct ANativeWindow *window, struct ANativeWindowBuffer **buffer)
{
    int fence = -1, ret = win_dequeue(window, buffer, &fence);
    if (fence >= 0) close(fence);
    return ret;
}
static int win_queue_dep(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer)
{ return win_queue(window, buffer, -1); }
static int win_cancel_dep(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer)
{ return win_cancel(window, buffer, -1); }
static int win_lock_dep(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer)
{ (void)window; (void)buffer; return 0; }

static int win_query(const struct ANativeWindow *window, int what, int *value)
{
    struct amphora_win *win = (struct amphora_win *)window;
    int32_t cmd = AMPHORA_BUF_QUERY, ret, v, w = what;
    /* Query what: 0=WIDTH 1=HEIGHT 6=DEFAULT_WIDTH 7=DEFAULT_HEIGHT 8=TRANSFORM_HINT.
     * If create_android_surface already seeded req_w/req_h, answer geometry without a
     * sock round-trip — otherwise host blocks in the adapter while guest wine thread
     * is still waiting on the WSI create-surface reply (deadlock). */
    if (win->req_w > 0 && (what == 0 /* WIDTH */ || what == 6 /* DEFAULT_WIDTH */)) {
        LOGI("query what=%d host val=%d override req_w=%d (no sock)", what, win->req_w, win->req_w);
        if (value) *value = win->req_w;
        return 0;
    }
    if (win->req_h > 0 && (what == 1 /* HEIGHT */ || what == 7 /* DEFAULT_HEIGHT */)) {
        LOGI("query what=%d host val=%d override req_h=%d (no sock)", what, win->req_h, win->req_h);
        if (value) *value = win->req_h;
        return 0;
    }
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &w, sizeof(w)) ||
        read_full(win->sock, &ret, sizeof(ret)) ||
        read_full(win->sock, &v, sizeof(v))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
    LOGI("query what=%d host ret=%d val=%d", what, ret, v);

    /* Force IDENTITY transform hint so Adreno does not SUBOPTIMAL on prerotation. */
    if (what == 8) { /* TRANSFORM_HINT */
        LOGI("TRANSFORM_HINT host val=%d -> force IDENTITY 0", v);
        if (value) *value = 0;
        return 0;
    }
    if (!ret && value) *value = v;
    return ret;
}

static int win_set_swap(struct ANativeWindow *window, int interval)
{
    struct amphora_win *win = (struct amphora_win *)window;
    int32_t cmd = AMPHORA_BUF_SET_SWAP, ret, iv = interval;
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &iv, sizeof(iv)) ||
        read_full(win->sock, &ret, sizeof(ret))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
    return ret;
}

static int win_perform(struct ANativeWindow *window, int operation, ...)
{
    struct amphora_win *win = (struct amphora_win *)window;
    int32_t cmd = AMPHORA_BUF_PERFORM, op = operation, nargs = 0, args[4], ret;
    va_list ap;
    /* AOSP NATIVE_WINDOW_GET_CONSUMER_USAGE64 / SET_USAGE64 — out/in uint64 cannot
     * ride the int32 sock protocol; satisfy locally so libvulkan can create an
     * Android surface on this sock-proxy hwnd ANW (Adreno needs 0xB00). */
    enum { NW_SET_USAGE64 = 30, NW_GET_CONSUMER_USAGE64 = 31,
           NW_SET_SHARED_BUFFER_MODE = 21, NW_SET_AUTO_REFRESH = 22 };
    va_start(ap, operation);
    if (operation == NW_GET_CONSUMER_USAGE64) {
        uint64_t *out = va_arg(ap, uint64_t *);
        va_end(ap);
        /* Adreno CreateSwapchain needs GPU_FRAMEBUFFER in the combined usage.
         * Stub consumer as SAMPLED|FRAMEBUFFER|COMPOSER_OVERLAY so SET_USAGE64
         * is 0xB00 even if GetSwapchainGrallocUsage* is weak/missing. */
        if (out) *out = 0x10000b00ull; /* GPU_SAMPLED|GPU_FRAMEBUFFER|COMPOSER_OVERLAY */
        LOGI("perform GET_CONSUMER_USAGE64 -> 0x10000b00 (local stub)");
        return 0;
    }
    if (operation == NW_SET_USAGE64) {
        uint64_t usage = va_arg(ap, uint64_t);
        va_end(ap);
        op = NATIVE_WINDOW_SET_USAGE;
        args[0] = (int32_t)usage;
        nargs = 1;
        LOGI("perform SET_USAGE64 0x%llx -> SET_USAGE 0x%x",
             (unsigned long long)usage, args[0]);
        goto send;
    }
    /* GET ops with pointer out-params cannot ride the int32 sock protocol. */
    if (operation == 23) { /* GET_REFRESH_CYCLE_DURATION */
        int64_t *out = va_arg(ap, int64_t *);
        va_end(ap);
        if (out) *out = 16666666; /* 60 Hz */
        LOGI("perform GET_REFRESH_CYCLE_DURATION -> 16666666");
        return 0;
    }
    if (operation == 24) { /* GET_NEXT_FRAME_ID */
        static uint64_t next_fid = 1;
        uint64_t *out = va_arg(ap, uint64_t *);
        va_end(ap);
        if (out) *out = next_fid++;
        return 0;
    }
    if (operation == 28 || operation == 29) { /* GET_WIDE_COLOR / GET_HDR */
        int *out = va_arg(ap, int *);
        va_end(ap);
        if (out) *out = 0;
        return 0;
    }
    if (operation == 36 || operation == 38 || operation == 39) {
        int64_t *out = va_arg(ap, int64_t *);
        va_end(ap);
        if (out) *out = 0;
        return 0;
    }
    if (operation == 35 /* SET_AUTO_PREROTATION */) {
        va_end(ap);
        LOGI("SET_AUTO_PREROTATION refuse -ENOENT (do not claim prerotation)");
        return -ENOENT;
    }
    if (operation == NW_SET_SHARED_BUFFER_MODE || operation == NW_SET_AUTO_REFRESH ||
        operation == 19 /* SET_BUFFERS_DATASPACE */ ||
        operation == 37 /* SET_DEQUEUE_TIMEOUT */) {
        va_end(ap);
        LOGI("perform op=%d (local no-op ok)", operation);
        return 0;
    }
    switch (operation) {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        args[0] = va_arg(ap, int); nargs = 1; break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        args[0] = va_arg(ap, int); args[1] = va_arg(ap, int); nargs = 2; break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        args[0] = va_arg(ap, int); args[1] = va_arg(ap, int);
        args[2] = va_arg(ap, int); nargs = 3; break;
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
        nargs = 0; break;
    default:
        va_end(ap);
        LOGI("perform op=%d (unknown, local no-op ok)", operation);
        return 0;
    }
    va_end(ap);
    if (operation == NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS ||
        operation == NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS ||
        operation == NATIVE_WINDOW_SET_BUFFERS_GEOMETRY) {
        win->req_w = args[0];
        win->req_h = args[1];
        LOGI("cache req_w=%d req_h=%d from op=%d", win->req_w, win->req_h, operation);
    }
send:
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &op, sizeof(op)) ||
        write_full(win->sock, &nargs, sizeof(nargs)) ||
        (nargs && write_full(win->sock, args, sizeof(int32_t) * (size_t)nargs)) ||
        read_full(win->sock, &ret, sizeof(ret))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
    LOGI("perform op=%d nargs=%d args[0]=%d args[1]=%d ret=%d",
         op, nargs, nargs > 0 ? args[0] : 0, nargs > 1 ? args[1] : 0, ret);
    return ret;
}

static struct ANativeWindow *make_win(int sock)
{
    struct amphora_win *win = calloc(1, sizeof(*win));
    if (!win) { close(sock); return NULL; }
    win->win.common.magic = ANDROID_NATIVE_WINDOW_MAGIC;
    win->win.common.version = (int)sizeof(struct ANativeWindow);
    win->win.common.incRef = win_inc;
    win->win.common.decRef = win_dec;
    win->win.setSwapInterval = win_set_swap;
    win->win.dequeueBuffer_DEPRECATED = win_dequeue_dep;
    win->win.lockBuffer_DEPRECATED = win_lock_dep;
    win->win.queueBuffer_DEPRECATED = win_queue_dep;
    win->win.query = win_query;
    win->win.perform = win_perform;
    win->win.cancelBuffer_DEPRECATED = win_cancel_dep;
    win->win.dequeueBuffer = win_dequeue;
    win->win.queueBuffer = win_queue;
    win->win.cancelBuffer = win_cancel;
    win->sock = sock;
    win->ref = 1;
    pthread_mutex_init(&win->lock, NULL);
    return &win->win;
}

#include "amphora_ahb_sc.inc"

/* Present PE Vulkan onto the Amphora hwnd sock-proxy ANW directly (no ImageReader
 * intermediate). Host already DEQUEUE/QUEUEs that hwnd Surface; the aarch64 helper
 * only rebuilds a native-ABI ANativeWindow over the same client sock so Box64 can
 * enter vkCreateAndroidSurfaceKHR. */
/* width/height: optional initial size from guest (ioctl on wine thread).
 * <=0 falls back to 640x480. Seeded onto proxy req_w/req_h so win_query does
 * not sock-round-trip during vkCreateAndroidSurfaceKHR. */
int32_t amphora_wsi_create_android_surface(uint64_t vk_instance, int32_t sock_fd,
                                           int32_t width, int32_t height, uint64_t *out_surface)
{
    VkInstance inst = (VkInstance)(uintptr_t)vk_instance;
    int dupfd, w, h;
    struct ANativeWindow *proxy;
    struct amphora_win *aw;
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_bridge_vkCreateAndroidSurfaceKHR create;
    VkAndroidSurfaceCreateInfoKHR_bridge info;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r;

    if (!out_surface) return -EINVAL;
    *out_surface = 0;
    if (!inst || sock_fd < 0) {
        LOGE("create_android_surface bad inst=%p sock=%d", (void *)inst, sock_fd);
        return -EINVAL;
    }
    w = (width > 0) ? width : 640;
    h = (height > 0) ? height : 480;
    dupfd = dup(sock_fd);
    if (dupfd < 0) { LOGE("dup sock: %s", strerror(errno)); return -errno; }
    proxy = make_win(dupfd);
    if (!proxy) { LOGE("make_win failed"); return -ENOMEM; }
    aw = (struct amphora_win *)proxy;
    aw->req_w = w;
    aw->req_h = h;
    LOGI("create_android_surface seed req %dx%d (no sock query)", w, h);

    lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) {
        LOGE("dlopen libvulkan: %s", dlerror());
        proxy->common.decRef(&proxy->common);
        return -ENOENT;
    }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        LOGE("no GIPA");
        proxy->common.decRef(&proxy->common);
        return -ENOENT;
    }
    create = (PFN_bridge_vkCreateAndroidSurfaceKHR)gipa(inst, "vkCreateAndroidSurfaceKHR");
    if (!create) {
        LOGE("no vkCreateAndroidSurfaceKHR on inst=%p", (void *)inst);
        proxy->common.decRef(&proxy->common);
        return -ENOSYS;
    }

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    info.window = proxy;
    r = create(inst, &info, NULL, &surface);
    LOGI("vkCreateAndroidSurfaceKHR DIRECT hwnd-ANW (no ImageReader) inst=%p proxy=%p %dx%d res=%d surface=%p",
         (void *)inst, (void *)proxy, w, h, (int)r, (void *)(uintptr_t)surface);
    if (r != VK_SUCCESS) {
        proxy->common.decRef(&proxy->common);
        return (int32_t)r;
    }
    /* Surface owns a ref via incRef inside the driver; keep our ref so the
     * sock-proxy stays alive for the lifetime of the VkSurfaceKHR. */
    *out_surface = (uint64_t)(uintptr_t)surface;
    ahb_sc_register_surface(surface, (struct amphora_win *)proxy);
    ahb_sc_ensure_instance_procs(inst);
    LOGI("AHB_SC register after create_android_surface surface=%p win=%p (no hooks)",
         (void *)(uintptr_t)surface, (void *)proxy);
    return 0;
}

/* Same-process IPC: Box64 guest cannot dlopen this aarch64 .so, so LD_PRELOAD
 * it into box64 and serve create-surface on a per-pid unix socket. */
#include <sys/un.h>
#include <stdio.h>

#define WSI_SOCK_FMT "/data/user/0/app.amphora/files/wineandroid/wsi-%d.sock"

static void *wsi_serve(void *arg)
{
    char path[128];
    struct sockaddr_un addr;
    int ls, c;
    (void)arg;
    snprintf(path, sizeof(path), WSI_SOCK_FMT, (int)getpid());
    unlink(path);
    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { LOGE("wsi socket: %s", strerror(errno)); return NULL; }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 2)) {
        LOGE("wsi bind/listen %s: %s", path, strerror(errno));
        close(ls);
        return NULL;
    }
    chmod(path, 0777);
    LOGI("wsi serve pid=%d path=%s", (int)getpid(), path);
    for (;;) {
        uint64_t inst = 0, surface = 0;
        int32_t sock = -1, ret, width = 0, height = 0;
        c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            LOGE("wsi accept: %s", strerror(errno));
            break;
        }
        /* IPC: inst(u64) + sock(i32) + width(i32) + height(i32).
         * Old clients that only sent inst+sock would hang here waiting for w/h —
         * guest wineandroid is changed in lockstep to send the size. */
        if (read_full(c, &inst, sizeof(inst)) || read_full(c, &sock, sizeof(sock)) ||
            read_full(c, &width, sizeof(width)) || read_full(c, &height, sizeof(height))) {
            LOGE("wsi read req failed");
            close(c);
            continue;
        }
        LOGI("wsi req inst=%p sock=%d size=%dx%d", (void *)(uintptr_t)inst, sock, width, height);
        ret = amphora_wsi_create_android_surface(inst, sock, width, height, &surface);
        if (write_full(c, &ret, sizeof(ret)) || write_full(c, &surface, sizeof(surface)))
            LOGE("wsi write reply failed");
        close(c);
    }
    close(ls);
    unlink(path);
    return NULL;
}


/* Swapchain IPC: wineandroid CreateSwapchain (x86_64) cannot dlsym this
 * aarch64 .so; call amphora_ahb_sc_* here on wsi-sc-%pid.sock. */
#define WSI_SC_SOCK_FMT "/data/user/0/app.amphora/files/wineandroid/wsi-sc-%d.sock"
enum {
    AHB_SC_OP_CREATE = 1,
    AHB_SC_OP_DESTROY = 2,
    AHB_SC_OP_GET_IMAGES = 3,
    AHB_SC_OP_ACQUIRE = 4,
    AHB_SC_OP_PRESENT = 5,
    AHB_SC_OP_STASH = 6,
};

static void wsi_sc_handle(int c)
{
    int32_t op = 0;
    if (read_full(c, &op, sizeof(op))) { LOGE("wsi-sc read op failed"); return; }
    if (op == AHB_SC_OP_STASH) {
        uint64_t dev = 0, phys = 0;
        int32_t ret = 0;
        if (read_full(c, &dev, sizeof(dev)) || read_full(c, &phys, sizeof(phys))) {
            LOGE("wsi-sc stash read failed"); return;
        }
        amphora_ahb_sc_stash_device(dev, phys);
        (void)write_full(c, &ret, sizeof(ret));
        return;
    }
    if (op == AHB_SC_OP_CREATE) {
        uint64_t device = 0, phys = 0, surface = 0, out_sc = 0;
        uint32_t minImageCount = 0, width = 0, height = 0, format = 0, usage = 0;
        uint32_t sharingMode = 0, preTransform = 0, compositeAlpha = 0, presentMode = 0, clipped = 0;
        uint32_t qcount = 0, qindices[8];
        VkSwapchainCreateInfoKHR info;
        VkSwapchainKHR sc = VK_NULL_HANDLE;
        VkResult r;
        int32_t ret;
        memset(qindices, 0, sizeof(qindices));
        if (read_full(c, &device, sizeof(device)) || read_full(c, &phys, sizeof(phys)) ||
            read_full(c, &surface, sizeof(surface)) ||
            read_full(c, &minImageCount, sizeof(minImageCount)) ||
            read_full(c, &width, sizeof(width)) || read_full(c, &height, sizeof(height)) ||
            read_full(c, &format, sizeof(format)) || read_full(c, &usage, sizeof(usage)) ||
            read_full(c, &sharingMode, sizeof(sharingMode)) ||
            read_full(c, &preTransform, sizeof(preTransform)) ||
            read_full(c, &compositeAlpha, sizeof(compositeAlpha)) ||
            read_full(c, &presentMode, sizeof(presentMode)) ||
            read_full(c, &clipped, sizeof(clipped)) ||
            read_full(c, &qcount, sizeof(qcount))) {
            LOGE("wsi-sc create read failed"); return;
        }
        if (qcount > 8) qcount = 8;
        if (qcount && read_full(c, qindices, sizeof(uint32_t) * qcount)) {
            LOGE("wsi-sc create qindices read failed"); return;
        }
        if (phys)
            amphora_ahb_sc_stash_device(device, phys);
        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        info.surface = (VkSurfaceKHR)(uintptr_t)surface;
        info.minImageCount = minImageCount;
        info.imageFormat = (VkFormat)format;
        info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        info.imageExtent.width = width;
        info.imageExtent.height = height;
        info.imageArrayLayers = 1;
        info.imageUsage = usage;
        info.imageSharingMode = (VkSharingMode)sharingMode;
        info.queueFamilyIndexCount = qcount;
        info.pQueueFamilyIndices = qcount ? qindices : NULL;
        info.preTransform = (VkSurfaceTransformFlagBitsKHR)preTransform;
        info.compositeAlpha = (VkCompositeAlphaFlagBitsKHR)compositeAlpha;
        info.presentMode = (VkPresentModeKHR)presentMode;
        info.clipped = clipped ? VK_TRUE : VK_FALSE;
        r = amphora_ahb_sc_create_swapchain((VkDevice)(uintptr_t)device, &info, NULL, &sc);
        ret = (int32_t)r;
        out_sc = (uint64_t)(uintptr_t)sc;
        (void)write_full(c, &ret, sizeof(ret));
        (void)write_full(c, &out_sc, sizeof(out_sc));
        LOGI("wsi-sc CREATE ret=%d sc=0x%llx", ret, (unsigned long long)out_sc);
        return;
    }
    if (op == AHB_SC_OP_DESTROY) {
        uint64_t device = 0, sc = 0;
        int32_t ret = 0;
        if (read_full(c, &device, sizeof(device)) || read_full(c, &sc, sizeof(sc))) return;
        amphora_ahb_sc_destroy_swapchain((VkDevice)(uintptr_t)device,
                                         (VkSwapchainKHR)(uintptr_t)sc, NULL);
        (void)write_full(c, &ret, sizeof(ret));
        return;
    }
    if (op == AHB_SC_OP_GET_IMAGES) {
        uint64_t device = 0, sc = 0;
        uint32_t count = 0, want = 0, i;
        VkImage images[AHB_SC_MAX_IMAGES];
        VkResult r;
        int32_t ret;
        if (read_full(c, &device, sizeof(device)) || read_full(c, &sc, sizeof(sc)) ||
            read_full(c, &count, sizeof(count)) || read_full(c, &want, sizeof(want))) return;
        if (!want) {
            r = amphora_ahb_sc_get_images((VkDevice)(uintptr_t)device,
                                          (VkSwapchainKHR)(uintptr_t)sc, &count, NULL);
            ret = (int32_t)r;
            (void)write_full(c, &ret, sizeof(ret));
            (void)write_full(c, &count, sizeof(count));
            return;
        }
        if (count > AHB_SC_MAX_IMAGES) count = AHB_SC_MAX_IMAGES;
        r = amphora_ahb_sc_get_images((VkDevice)(uintptr_t)device,
                                      (VkSwapchainKHR)(uintptr_t)sc, &count, images);
        ret = (int32_t)r;
        (void)write_full(c, &ret, sizeof(ret));
        (void)write_full(c, &count, sizeof(count));
        for (i = 0; i < count; i++) {
            uint64_t img = (uint64_t)(uintptr_t)images[i];
            (void)write_full(c, &img, sizeof(img));
        }
        return;
    }
    if (op == AHB_SC_OP_ACQUIRE) {
        uint64_t device = 0, sc = 0, timeout = 0, sem = 0, fence = 0;
        uint32_t idx = 0;
        VkResult r;
        int32_t ret;
        if (read_full(c, &device, sizeof(device)) || read_full(c, &sc, sizeof(sc)) ||
            read_full(c, &timeout, sizeof(timeout)) || read_full(c, &sem, sizeof(sem)) ||
            read_full(c, &fence, sizeof(fence))) return;
        r = amphora_ahb_sc_acquire((VkDevice)(uintptr_t)device, (VkSwapchainKHR)(uintptr_t)sc,
                                   timeout, (VkSemaphore)(uintptr_t)sem,
                                   (VkFence)(uintptr_t)fence, &idx);
        ret = (int32_t)r;
        (void)write_full(c, &ret, sizeof(ret));
        (void)write_full(c, &idx, sizeof(idx));
        return;
    }
    if (op == AHB_SC_OP_PRESENT) {
        uint64_t queue = 0;
        uint32_t wait_n = 0, sc_n = 0, i;
        uint64_t wait_sems[8], swapchains[4];
        uint32_t indices[4];
        VkPresentInfoKHR pi;
        VkResult results[4];
        VkResult r;
        int32_t ret;
        VkSemaphore wait_h[8];
        VkSwapchainKHR sc_h[4];
        memset(wait_sems, 0, sizeof(wait_sems));
        memset(swapchains, 0, sizeof(swapchains));
        memset(indices, 0, sizeof(indices));
        memset(results, 0, sizeof(results));
        if (read_full(c, &queue, sizeof(queue)) || read_full(c, &wait_n, sizeof(wait_n)) ||
            read_full(c, &sc_n, sizeof(sc_n))) return;
        if (wait_n > 8) wait_n = 8;
        if (sc_n > 4) sc_n = 4;
        for (i = 0; i < wait_n; i++)
            if (read_full(c, &wait_sems[i], sizeof(wait_sems[i]))) return;
        for (i = 0; i < sc_n; i++) {
            if (read_full(c, &swapchains[i], sizeof(swapchains[i]))) return;
            if (read_full(c, &indices[i], sizeof(indices[i]))) return;
        }
        for (i = 0; i < wait_n; i++) wait_h[i] = (VkSemaphore)(uintptr_t)wait_sems[i];
        for (i = 0; i < sc_n; i++) sc_h[i] = (VkSwapchainKHR)(uintptr_t)swapchains[i];
        memset(&pi, 0, sizeof(pi));
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = wait_n;
        pi.pWaitSemaphores = wait_n ? wait_h : NULL;
        pi.swapchainCount = sc_n;
        pi.pSwapchains = sc_h;
        pi.pImageIndices = indices;
        pi.pResults = results;
        r = amphora_ahb_sc_present((VkQueue)(uintptr_t)queue, &pi);
        ret = (int32_t)r;
        (void)write_full(c, &ret, sizeof(ret));
        for (i = 0; i < sc_n; i++) {
            int32_t rr = (int32_t)results[i];
            (void)write_full(c, &rr, sizeof(rr));
        }
        return;
    }
    LOGW("wsi-sc unknown op=%d", op);
}

static void *wsi_sc_serve(void *arg)
{
    char path[128];
    struct sockaddr_un addr;
    int ls, c;
    (void)arg;
    snprintf(path, sizeof(path), WSI_SC_SOCK_FMT, (int)getpid());
    unlink(path);
    ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) { LOGE("wsi-sc socket: %s", strerror(errno)); return NULL; }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(ls, (struct sockaddr *)&addr, sizeof(addr)) || listen(ls, 4)) {
        LOGE("wsi-sc bind/listen %s: %s", path, strerror(errno));
        close(ls);
        return NULL;
    }
    chmod(path, 0777);
    LOGI("wsi-sc serve pid=%d path=%s", (int)getpid(), path);
    for (;;) {
        c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            LOGE("wsi-sc accept: %s", strerror(errno));
            break;
        }
        wsi_sc_handle(c);
        close(c);
    }
    close(ls);
    unlink(path);
    return NULL;
}

__attribute__((constructor))
static void amphora_wsi_ctor(void)
{
    pthread_t th, th_sc;
    LOGI("wsi ctor pid=%d knife13-src-createswapchain", (int)getpid());
    /* Runtime hook installer disabled; Wine CreateSwapchain calls amphora_ahb_sc_*. */
    if (pthread_create(&th, NULL, wsi_serve, NULL) == 0)
        pthread_detach(th);
    else
        LOGE("wsi pthread_create: %s", strerror(errno));
    if (pthread_create(&th_sc, NULL, wsi_sc_serve, NULL) == 0)
        pthread_detach(th_sc);
    else
        LOGE("wsi-sc pthread_create: %s", strerror(errno));
}
