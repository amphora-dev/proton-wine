/*
 * aarch64-native Amphora WSI bridge.
 *
 * Wine's x86_64 sock-proxy ANW cannot be passed through Box64 into host
 * vkCreateAndroidSurfaceKHR (vtable is guest-ABI). This helper lives in the
 * box64 process as a native ARM64 .so: it rebuilds an aarch64 ANativeWindow
 * over the same Amphora client sock and creates the Android surface on THAT
 * hwnd ANW (no ImageReader blit hop) so PE QueuePresent DEQUEUE/QUEUEs the
 * dedicated Amphora window Surface directly.
 */
#include <android/log.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
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

struct ANativeWindowBuffer {
    struct android_native_base_t common;
    int width, height, stride, format, usage;
    void *reserved[2];
    const native_handle_t *handle;
    void *reserved_proc[8];
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

struct amphora_buf {
    struct ANativeWindowBuffer buffer;
    native_handle_t *nh;
    int buffer_id;
    int generation;
    int ref;
};

struct amphora_win {
    struct ANativeWindow win;
    int sock;
    int ref;
    pthread_mutex_t lock;
    struct amphora_buf *bufs[NB_CACHED_BUFFERS];
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

static void buf_inc(struct android_native_base_t *base)
{
    struct amphora_buf *buf = (struct amphora_buf *)base;
    __sync_add_and_fetch(&buf->ref, 1);
}

static void buf_dec(struct android_native_base_t *base)
{
    struct amphora_buf *buf = (struct amphora_buf *)base;
    if (__sync_sub_and_fetch(&buf->ref, 1) > 0) return;
    close_nh(buf->nh);
    free(buf);
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
    for (i = 0; i < NB_CACHED_BUFFERS; i++)
        if (win->bufs[i]) win->bufs[i]->buffer.common.decRef(&win->bufs[i]->buffer.common);
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

    if (fence) *fence = -1;
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd))) { pthread_mutex_unlock(&win->lock); return -EIO; }
    if (recv_with_fds(win->sock, &hdr, sizeof(hdr), fds, 64, &n_fds)) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    if (hdr.status) { pthread_mutex_unlock(&win->lock); return hdr.status; }
    if (hdr.numFds < 0 || hdr.numInts < 0 || hdr.numFds > 64 || hdr.numInts > 256 || n_fds != hdr.numFds) {
        int i; for (i = 0; i < n_fds; i++) close(fds[i]);
        pthread_mutex_unlock(&win->lock); return -EINVAL;
    }
    if (hdr.numInts) {
        ints = malloc(sizeof(int) * (size_t)hdr.numInts);
        if (!ints || read_full(win->sock, ints, sizeof(int) * (size_t)hdr.numInts)) {
            int i; free(ints); for (i = 0; i < n_fds; i++) close(fds[i]);
            pthread_mutex_unlock(&win->lock); return -EIO;
        }
    }
    /* libvulkan swapchain matches images by ANativeWindowBuffer* identity.
     * Reuse the cached slot pointer; closing the extra SCM_RIGHTS dups. */
    if (hdr.buffer_id >= 0 && hdr.buffer_id < NB_CACHED_BUFFERS &&
        win->bufs[hdr.buffer_id] &&
        win->bufs[hdr.buffer_id]->generation == hdr.generation) {
        int i;
        for (i = 0; i < n_fds; i++) close(fds[i]);
        free(ints);
        buf = win->bufs[hdr.buffer_id];
        buf->buffer.width = hdr.width;
        buf->buffer.height = hdr.height;
        buf->buffer.stride = hdr.stride;
        buf->buffer.format = hdr.format;
        buf->buffer.usage = hdr.usage;
        *out = &buf->buffer;
        pthread_mutex_unlock(&win->lock);
        LOGI("dequeue id=%d REUSE %dx%d fmt=%d usage=0x%x ptr=%p",
             hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage, (void *)buf);
        return 0;
    }
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
    buf->buffer.common.magic = ANDROID_NATIVE_BUFFER_MAGIC;
    buf->buffer.common.version = (int)sizeof(struct ANativeWindowBuffer);
    buf->buffer.common.incRef = buf_inc;
    buf->buffer.common.decRef = buf_dec;
    buf->buffer.width = hdr.width;
    buf->buffer.height = hdr.height;
    buf->buffer.stride = hdr.stride;
    buf->buffer.format = hdr.format;
    buf->buffer.usage = hdr.usage;
    buf->buffer.handle = nh;
    buf->nh = nh;
    buf->buffer_id = hdr.buffer_id;
    buf->generation = hdr.generation;
    buf->ref = 1;
    if (hdr.buffer_id >= 0 && hdr.buffer_id < NB_CACHED_BUFFERS) {
        if (win->bufs[hdr.buffer_id])
            win->bufs[hdr.buffer_id]->buffer.common.decRef(&win->bufs[hdr.buffer_id]->buffer.common);
        win->bufs[hdr.buffer_id] = buf;
        buf->buffer.common.incRef(&buf->buffer.common);
    }
    *out = &buf->buffer;
    pthread_mutex_unlock(&win->lock);
    LOGI("dequeue id=%d NEW %dx%d fmt=%d usage=0x%x ptr=%p",
         hdr.buffer_id, hdr.width, hdr.height, hdr.format, hdr.usage, (void *)buf);
    return 0;
}

static int win_queue(struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence)
{
    struct amphora_win *win = (struct amphora_win *)window;
    struct amphora_buf *buf = (struct amphora_buf *)buffer;
    int32_t cmd = AMPHORA_BUF_QUEUE, ret;
    if (fence >= 0) close(fence);
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
    struct amphora_buf *buf = (struct amphora_buf *)buffer;
    int32_t cmd = AMPHORA_BUF_CANCEL, ret;
    if (fence >= 0) close(fence);
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
    pthread_mutex_lock(&win->lock);
    if (write_full(win->sock, &cmd, sizeof(cmd)) ||
        write_full(win->sock, &w, sizeof(w)) ||
        read_full(win->sock, &ret, sizeof(ret)) ||
        read_full(win->sock, &v, sizeof(v))) {
        pthread_mutex_unlock(&win->lock); return -EIO;
    }
    pthread_mutex_unlock(&win->lock);
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
        if (out) *out = 0xB00ull; /* GPU_SAMPLED|GPU_FRAMEBUFFER|COMPOSER_OVERLAY */
        LOGI("perform GET_CONSUMER_USAGE64 -> 0xB00 (local stub)");
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
    if (operation == NW_SET_SHARED_BUFFER_MODE || operation == NW_SET_AUTO_REFRESH ||
        operation == 19 /* SET_BUFFERS_DATASPACE */ ||
        operation == 35 /* SET_AUTO_PREROTATION */ ||
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
    LOGI("perform op=%d nargs=%d ret=%d", op, nargs, ret);
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

/* Present PE Vulkan onto the Amphora hwnd sock-proxy ANW directly (no ImageReader
 * intermediate). Host already DEQUEUE/QUEUEs that hwnd Surface; the aarch64 helper
 * only rebuilds a native-ABI ANativeWindow over the same client sock so Box64 can
 * enter vkCreateAndroidSurfaceKHR. */
int32_t amphora_wsi_create_android_surface(uint64_t vk_instance, int32_t sock_fd, uint64_t *out_surface)
{
    VkInstance inst = (VkInstance)(uintptr_t)vk_instance;
    int dupfd, w = 640, h = 480, q;
    struct ANativeWindow *proxy;
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
    dupfd = dup(sock_fd);
    if (dupfd < 0) { LOGE("dup sock: %s", strerror(errno)); return -errno; }
    proxy = make_win(dupfd);
    if (!proxy) { LOGE("make_win failed"); return -ENOMEM; }
    if (!proxy->query(proxy, 0, &q) && q > 0) w = q;
    if (!proxy->query(proxy, 1, &q) && q > 0) h = q;

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
        int32_t sock = -1, ret;
        c = accept(ls, NULL, NULL);
        if (c < 0) {
            if (errno == EINTR) continue;
            LOGE("wsi accept: %s", strerror(errno));
            break;
        }
        if (read_full(c, &inst, sizeof(inst)) || read_full(c, &sock, sizeof(sock))) {
            LOGE("wsi read req failed");
            close(c);
            continue;
        }
        LOGI("wsi req inst=%p sock=%d", (void *)(uintptr_t)inst, sock);
        ret = amphora_wsi_create_android_surface(inst, sock, &surface);
        if (write_full(c, &ret, sizeof(ret)) || write_full(c, &surface, sizeof(surface)))
            LOGE("wsi write reply failed");
        close(c);
    }
    close(ls);
    unlink(path);
    return NULL;
}

__attribute__((constructor))
static void amphora_wsi_ctor(void)
{
    pthread_t th;
    LOGI("wsi ctor pid=%d", (int)getpid());
    if (pthread_create(&th, NULL, wsi_serve, NULL) == 0)
        pthread_detach(th);
    else
        LOGE("wsi pthread_create: %s", strerror(errno));
}
