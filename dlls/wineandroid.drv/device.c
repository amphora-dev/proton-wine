/*
 * Android pseudo-device handling
 *
 * Copyright 2014-2017 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "android.h"
#include "wine/server.h"
#include "wine/debug.h"

#include <dlfcn.h>

WINE_DEFAULT_DEBUG_CHANNEL(android);

#ifndef SYNC_IOC_WAIT
#define SYNC_IOC_WAIT _IOW('>', 0, __s32)
#endif

static HWND desktop_window;

#define ANDROIDCONTROLTYPE  ((ULONG)'A')
#define ANDROID_IOCTL(n) CTL_CODE(ANDROIDCONTROLTYPE, n, METHOD_BUFFERED, FILE_READ_ACCESS)

enum android_ioctl
{
    IOCTL_CREATE_DESKTOP_VIEW,
    IOCTL_CREATE_WINDOW,
    IOCTL_DESTROY_WINDOW,
    IOCTL_WINDOW_POS_CHANGED,
    IOCTL_SET_WINDOW_PARENT,
    IOCTL_DEQUEUE_BUFFER,
    IOCTL_QUEUE_BUFFER,
    IOCTL_CANCEL_BUFFER,
    IOCTL_QUERY,
    IOCTL_PERFORM,
    IOCTL_SET_SWAP_INT,
    IOCTL_SET_CAPTURE,
    IOCTL_SET_CURSOR,
    IOCTL_GET_BUFFER_SOCK,
    NB_IOCTLS
};

#define NB_CACHED_BUFFERS 4

/* wrapper for a native window in the context of the client (non-Java) process */
struct native_win_wrapper
{
    struct ANativeWindow          win;
    struct
    {
        struct AHardwareBuffer   *self;
        int                       buffer_id;
        int                       generation;
    } buffers[NB_CACHED_BUFFERS];
    struct AHardwareBuffer       *locked_buffer;
    HWND                          hwnd;
    BOOL                          opengl;
    LONG                          ref;
    int                           amphora_sock;
    int                           cached_width;  /* 0 = unset; last successful WIDTH */
    int                           cached_height; /* 0 = unset; last successful HEIGHT */
};

/* Amphora's Vulkan bridge consumes the per-window stream protocol,
 * while proton_11.0 uses the upstream SEQPACKET device transport.  Keep the
 * latter as the source of truth and expose a tiny stream adapter per wrapper. */
static struct native_win_wrapper *amphora_windows[65536];
static pthread_mutex_t amphora_windows_lock = PTHREAD_MUTEX_INITIALIZER;

#define IPC_SOCKET_NAME "\0\\Device\\WineAndroid"
#define IPC_SOCKET_ADDR_LEN ((socklen_t)(offsetof(struct sockaddr_un, sun_path) + sizeof(IPC_SOCKET_NAME) - 1))

static const struct sockaddr_un ipc_addr = {
    .sun_family = AF_UNIX,
    .sun_path = IPC_SOCKET_NAME,
};

struct ioctl_header
{
    int  hwnd;
    BOOL opengl;
};

struct ioctl_android_create_desktop_view
{
    struct ioctl_header hdr;
    int                 log_flags;
};

struct ioctl_android_create_window
{
    struct ioctl_header hdr;
    int                 parent;
    BOOL                is_desktop;
};

struct ioctl_android_destroy_window
{
    struct ioctl_header hdr;
};

struct ioctl_android_window_pos_changed
{
    struct ioctl_header hdr;
    RECT                window_rect;
    RECT                client_rect;
    RECT                visible_rect;
    int                 style;
    int                 flags;
    int                 after;
    int                 owner;
};

struct ioctl_android_dequeueBuffer
{
    struct ioctl_header hdr;
    int buffer_id;
    int generation;
};

struct ioctl_android_queueBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
};

struct ioctl_android_cancelBuffer
{
    struct ioctl_header hdr;
    int                 buffer_id;
    int                 generation;
};

struct ioctl_android_query
{
    struct ioctl_header hdr;
    int                 what;
    int                 value;
};

struct ioctl_android_perform
{
    struct ioctl_header hdr;
    int                 operation;
    int                 args[4];
};

struct ioctl_android_set_swap_interval
{
    struct ioctl_header hdr;
    int                 interval;
};

struct ioctl_android_set_window_parent
{
    struct ioctl_header hdr;
    int                 parent;
};

struct ioctl_android_set_capture
{
    struct ioctl_header hdr;
};

struct ioctl_android_set_cursor
{
    struct ioctl_header hdr;
    int                 id;
    int                 width;
    int                 height;
    int                 hotspotx;
    int                 hotspoty;
    int                 bits[1];
};

static unsigned int data_map_idx( HWND hwnd, BOOL opengl )
{
    /* window handles are always even, so use low-order bit for opengl flag */
    return LOWORD(hwnd) + !!opengl;
}

static int get_ioctl_win_parent( HWND parent )
{
    if (parent != NtUserGetDesktopWindow() && !NtUserGetAncestor( parent, GA_PARENT ))
        return HandleToLong( HWND_MESSAGE );
    return HandleToLong( parent );
}

static void wait_fence_and_close( int fence )
{
    __s32 timeout = 1000;  /* FIXME: should be -1 for infinite timeout */

    if (fence == -1) return;
    ioctl( fence, SYNC_IOC_WAIT, &timeout );
    close( fence );
}

static inline struct ANativeWindowBuffer *anwb_from_ahb(AHardwareBuffer *ahb)
{
    /* AOSP: ANativeWindowBuffer_getHardwareBuffer is
     * reinterpret_cast<AHardwareBuffer*>(static_cast<GraphicBuffer*>(anwb)).
     * GraphicBuffer's primary base is RefBase (vptr), so ANativeWindowBuffer
     * sits at a non-zero offset. Recover it with pointer arithmetic only: the
     * NDK call never dereferences its argument. */
    static ptrdiff_t off = (ptrdiff_t)-1;

    if (!ahb) return NULL;

    if (off == (ptrdiff_t)-1)
    {
        struct ANativeWindowBuffer *fake =
        (struct ANativeWindowBuffer *)(uintptr_t)0x10000u;

        AHardwareBuffer *h = pANativeWindowBuffer_getHardwareBuffer(fake);
        off = (const char *)h - (const char *)fake;
    }

    return (struct ANativeWindowBuffer *)((char *)ahb - off);
}

static AHardwareBuffer *ahb_from_anwb( struct native_win_wrapper *win, struct ANativeWindowBuffer *buffer, int *buffer_id, int *generation )
{
    AHardwareBuffer *ahb;
    unsigned int i;

    if (!buffer) return NULL;

    ahb = pANativeWindowBuffer_getHardwareBuffer(buffer);

    if (win)
    {
        for (i = 0; i < NB_CACHED_BUFFERS; ++i)
        {
            if (win->buffers[i].self != ahb) continue;

            if (buffer_id) *buffer_id = win->buffers[i].buffer_id;
            if (generation) *generation = win->buffers[i].generation;
            break;
        }
    }

    return ahb;
}

/* Client-side ioctl support */


static int android_ioctl( enum android_ioctl code, void *in, DWORD in_size, void *out, DWORD *out_size, int *recv_fd )
{
    static int device_fd = -1;
    static pthread_mutex_t device_mutex = PTHREAD_MUTEX_INITIALIZER;
    int status, err = -ENOENT;
    ssize_t ret;
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov[2] = { { &status, sizeof(status) }, { out, out_size ? *out_size : 0 } };
    struct msghdr msg = { NULL, 0, iov, (out && out_size) ? 2 : 1,
                          recv_fd ? control : NULL, recv_fd ? sizeof(control) : 0, 0 };
    struct cmsghdr *cmsg;

    pthread_mutex_lock( &device_mutex );

    if (recv_fd) *recv_fd = -1;

    if (device_fd == -1)
    {
        device_fd = socket( AF_UNIX, SOCK_SEQPACKET, 0 );
        if (device_fd < 0) goto done;
        if (connect( device_fd, (const struct sockaddr *)&ipc_addr, IPC_SOCKET_ADDR_LEN ) < 0)
        {
            close( device_fd );
            device_fd = -1;
            goto done;
        }
    }

    ret = writev( device_fd, (struct iovec[]){ { &code, sizeof(code) }, { in, in_size } }, 2 );
    if (ret <= 0 || ret != sizeof(code) + in_size) goto disconnected;

    ret = recvmsg( device_fd, &msg, 0 );
    if (ret <= 0 || ret < sizeof(status)) goto disconnected;

    if (out && out_size) *out_size = ret - sizeof(status);
    err = status;

    if (recv_fd)
        for (cmsg = CMSG_FIRSTHDR( &msg ); cmsg; cmsg = CMSG_NXTHDR( &msg, cmsg ))
            if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
                cmsg->cmsg_len >= CMSG_LEN(sizeof(int)))
            {
                memcpy( recv_fd, CMSG_DATA(cmsg), sizeof(int) );
                break;
            }

    goto done;

disconnected:
    close( device_fd );
    device_fd = -1;
    WARN( "parent process is gone\n" );
    NtTerminateProcess( 0, 1 );
    err = -ENOENT;

done:
    pthread_mutex_unlock( &device_mutex );
    return err;
}

static void win_incRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedIncrement( &win->ref );
}

static void win_decRef( struct android_native_base_t *base )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)base;
    InterlockedDecrement( &win->ref );
}

void createDesktopView( int *event_source )
{
    struct ioctl_android_create_desktop_view res = { 0 };
    res.log_flags = __wine_dbg_get_channel_flags(&__wine_dbch_android);
    android_ioctl( IOCTL_CREATE_DESKTOP_VIEW, &res, sizeof(res), NULL, NULL, event_source );
}

static int dequeueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer, int *fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_dequeueBuffer res = {0};
    DWORD size = sizeof(res);
    int ret, buffer_fd = -1;

    res.hdr.hwnd = HandleToLong( win->hwnd );
    res.hdr.opengl = win->opengl;
    res.buffer_id = -1;
    res.generation = 0;

    ret = android_ioctl( IOCTL_DEQUEUE_BUFFER, &res, size, &res, &size, &buffer_fd );
    if (ret) return ret;
    if (size < sizeof(res)) return -EINVAL;

    if (res.buffer_id < 0 || res.buffer_id >= NB_CACHED_BUFFERS) return -EINVAL;

    if (buffer_fd != -1)
    {
        AHardwareBuffer *ahb = NULL;
        ret = pAHardwareBuffer_recvHandleFromUnixSocket( buffer_fd, &ahb );
        close( buffer_fd );
        if (ret) return ret;

        if (win->buffers[res.buffer_id].self)
            pAHardwareBuffer_release( win->buffers[res.buffer_id].self );

        win->buffers[res.buffer_id].self = ahb;
        win->buffers[res.buffer_id].buffer_id = res.buffer_id;
        win->buffers[res.buffer_id].generation = res.generation;
    }

    if (!win->buffers[res.buffer_id].self) return -EINVAL;

    *buffer = anwb_from_ahb(win->buffers[res.buffer_id].self);
    *fence = -1;

    TRACE( "hwnd %p, buffer %p id %d gen %d fence %d\n",
           win->hwnd, *buffer, res.buffer_id, res.generation, *fence );
    return 0;
}

static int cancelBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_cancelBuffer cancel;

    TRACE( "hwnd %p buffer %p fence %d\n", win->hwnd, buffer, fence );

    if (!ahb_from_anwb( win, buffer, &cancel.buffer_id, &cancel.generation ))
    {
        wait_fence_and_close( fence );
        return -EINVAL;
    }

    cancel.hdr.hwnd = HandleToLong( win->hwnd );
    cancel.hdr.opengl = win->opengl;
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_CANCEL_BUFFER, &cancel, sizeof(cancel), NULL, NULL, NULL );
}

static int queueBuffer( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer, int fence )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_queueBuffer queue;

    TRACE( "hwnd %p buffer %p fence %d\n", win->hwnd, buffer, fence );

    if (!ahb_from_anwb( win, buffer, &queue.buffer_id, &queue.generation ))
    {
        wait_fence_and_close( fence );
        return -EINVAL;
    }

    queue.hdr.hwnd = HandleToLong( win->hwnd );
    queue.hdr.opengl = win->opengl;
    wait_fence_and_close( fence );
    return android_ioctl( IOCTL_QUEUE_BUFFER, &queue, sizeof(queue), NULL, NULL, NULL );
}

static int dequeueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer **buffer )
{
    int fence, ret = dequeueBuffer( window, buffer, &fence );

    if (!ret) wait_fence_and_close( fence );
    return ret;
}

static int cancelBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return cancelBuffer( window, buffer, -1 );
}

static int lockBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return 0;  /* nothing to do */
}

static int queueBuffer_DEPRECATED( struct ANativeWindow *window, struct ANativeWindowBuffer *buffer )
{
    return queueBuffer( window, buffer, -1 );
}

static int setSwapInterval( struct ANativeWindow *window, int interval )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_set_swap_interval swap;

    TRACE( "hwnd %p interval %d\n", win->hwnd, interval );
    swap.hdr.hwnd = HandleToLong( win->hwnd );
    swap.hdr.opengl = win->opengl;
    swap.interval = interval;
    return android_ioctl( IOCTL_SET_SWAP_INT, &swap, sizeof(swap), NULL, NULL, NULL );
}

static int query( const ANativeWindow *window, int what, int *value )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_query query;
    DWORD size = sizeof( query );
    int ret;

    query.hdr.hwnd = HandleToLong( win->hwnd );
    query.hdr.opengl = win->opengl;
    query.what = what;
    ret = android_ioctl( IOCTL_QUERY, &query, sizeof(query), &query, &size, NULL );
    TRACE( "hwnd %p what %d got %d -> %p\n", win->hwnd, what, query.value, value );
    if (!ret)
    {
        *value = query.value;
        if (what == NATIVE_WINDOW_WIDTH || what == NATIVE_WINDOW_DEFAULT_WIDTH)
            win->cached_width = query.value;
        else if (what == NATIVE_WINDOW_HEIGHT || what == NATIVE_WINDOW_DEFAULT_HEIGHT)
            win->cached_height = query.value;
    }
    return ret;
}

static int perform( ANativeWindow *window, int operation, ... )
{
    static const char * const names[] =
    {
        "SET_USAGE", "CONNECT", "DISCONNECT", "SET_CROP", "SET_BUFFER_COUNT", "SET_BUFFERS_GEOMETRY",
        "SET_BUFFERS_TRANSFORM", "SET_BUFFERS_TIMESTAMP", "SET_BUFFERS_DIMENSIONS", "SET_BUFFERS_FORMAT",
        "SET_SCALING_MODE", "LOCK", "UNLOCK_AND_POST", "API_CONNECT", "API_DISCONNECT",
        "SET_BUFFERS_USER_DIMENSIONS", "SET_POST_TRANSFORM_CROP"
    };

    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    struct ioctl_android_perform perf;
    va_list args;

    perf.hdr.hwnd  = HandleToLong( win->hwnd );
    perf.hdr.opengl = win->opengl;
    perf.operation = operation;
    memset( perf.args, 0, sizeof(perf.args) );

    va_start( args, operation );
    switch (operation)
    {
    case NATIVE_WINDOW_SET_USAGE:
    case NATIVE_WINDOW_SET_BUFFERS_TRANSFORM:
    case NATIVE_WINDOW_SET_BUFFERS_FORMAT:
    case NATIVE_WINDOW_SET_SCALING_MODE:
    case NATIVE_WINDOW_API_CONNECT:
    case NATIVE_WINDOW_API_DISCONNECT:
        perf.args[0] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFER_COUNT:
        perf.args[0] = va_arg( args, size_t );
        TRACE( "hwnd %p %s count %d\n", win->hwnd, names[operation], perf.args[0] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_DIMENSIONS:
    case NATIVE_WINDOW_SET_BUFFERS_USER_DIMENSIONS:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d\n", win->hwnd, names[operation], perf.args[0], perf.args[1] );
        break;
    case NATIVE_WINDOW_SET_BUFFERS_GEOMETRY:
        perf.args[0] = va_arg( args, int );
        perf.args[1] = va_arg( args, int );
        perf.args[2] = va_arg( args, int );
        TRACE( "hwnd %p %s arg %dx%d %d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2] );
        break;
    case NATIVE_WINDOW_SET_CROP:
    {
        android_native_rect_t *rect = va_arg( args, android_native_rect_t * );
        perf.args[0] = rect->left;
        perf.args[1] = rect->top;
        perf.args[2] = rect->right;
        perf.args[3] = rect->bottom;
        TRACE( "hwnd %p %s rect %d,%d-%d,%d\n", win->hwnd, names[operation],
               perf.args[0], perf.args[1], perf.args[2], perf.args[3] );
        break;
    }
    case NATIVE_WINDOW_SET_BUFFERS_TIMESTAMP:
    {
        int64_t timestamp = va_arg( args, int64_t );
        perf.args[0] = timestamp;
        perf.args[1] = timestamp >> 32;
        TRACE( "hwnd %p %s arg %08x%08x\n", win->hwnd, names[operation], perf.args[1], perf.args[0] );
        break;
    }
    case NATIVE_WINDOW_LOCK:
    {
        struct ANativeWindowBuffer *buffer = NULL;
        struct ANativeWindow_Buffer *buffer_ret = va_arg( args, ANativeWindow_Buffer * );
        struct AHardwareBuffer* b = NULL;
        ARect *bounds = va_arg( args, ARect * );
        int ret = window->dequeueBuffer_DEPRECATED( window, &buffer );
        if (!ret && !buffer)
        {
            ret = -EWOULDBLOCK;
            TRACE( "got invalid buffer\n" );
        }
        if (!ret)
        {
            if (!(b = ahb_from_anwb((struct native_win_wrapper*) window, buffer, NULL, NULL))) {
                ret = -EINVAL;
            }

            if (b && (ret = pAHardwareBuffer_lock( b, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, NULL, &buffer_ret->bits )))
            {
                WARN( "AHardwareBuffer_lock %p failed %d %s\n", win->hwnd, ret, strerror(-ret) );
                window->cancelBuffer( window, buffer, -1 );
            }
        }
        if (!ret)
        {
            AHardwareBuffer_Desc d = {0};
            pAHardwareBuffer_describe(b, &d);
            buffer_ret->width  = d.width;
            buffer_ret->height = d.height;
            buffer_ret->stride = d.stride;
            buffer_ret->format = d.format;
            win->locked_buffer = b;
            if (bounds)
            {
                bounds->left   = 0;
                bounds->top    = 0;
                bounds->right  = d.width;
                bounds->bottom = d.height;
            }
        }
        va_end( args );
        TRACE( "hwnd %p %s bits %p ret %d %s\n", win->hwnd, names[operation], buffer_ret->bits, ret, strerror(-ret) );
        return ret;
    }
    case NATIVE_WINDOW_UNLOCK_AND_POST:
    {
        int ret = -EINVAL;
        if (win->locked_buffer)
        {
            pAHardwareBuffer_unlock(win->locked_buffer, NULL);
            ret = window->queueBuffer( window, anwb_from_ahb(win->locked_buffer), -1 );
            win->locked_buffer = NULL;
        }
        va_end( args );
        TRACE( "hwnd %p %s ret %d\n", win->hwnd, names[operation], ret );
        return ret;
    }
    case NATIVE_WINDOW_CONNECT:
    case NATIVE_WINDOW_DISCONNECT:
        TRACE( "hwnd %p %s\n", win->hwnd, names[operation] );
        break;
    case NATIVE_WINDOW_SET_POST_TRANSFORM_CROP:
    default:
        FIXME( "unsupported perform hwnd %p op %d %s\n", win->hwnd, operation,
               operation < ARRAY_SIZE( names ) ? names[operation] : "???" );
        break;
    }
    va_end( args );
    return android_ioctl( IOCTL_PERFORM, &perf, sizeof(perf), NULL, NULL, NULL );
}


#define AMPHORA_BUF_DEQUEUE  1
#define AMPHORA_BUF_QUEUE    2
#define AMPHORA_BUF_CANCEL   3
#define AMPHORA_BUF_QUERY    4
#define AMPHORA_BUF_PERFORM  5
#define AMPHORA_BUF_SET_SWAP 6
#define AMPHORA_AHB_NUMFDS  -1

struct amphora_adapter
{
    struct native_win_wrapper *win;
    int sock;
};

static int amphora_write_full( int fd, const void *buf, size_t len )
{
    const char *ptr = buf;

    while (len)
    {
        ssize_t ret = write( fd, ptr, len );
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!ret) return -1;
        ptr += ret;
        len -= ret;
    }
    return 0;
}

static int amphora_read_full( int fd, void *buf, size_t len )
{
    char *ptr = buf;

    while (len)
    {
        ssize_t ret = read( fd, ptr, len );
        if (ret < 0)
        {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!ret) return -1;
        ptr += ret;
        len -= ret;
    }
    return 0;
}

static int amphora_adapter_dequeue( struct amphora_adapter *adapter )
{
    struct native_win_wrapper *win = adapter->win;
    struct ANativeWindowBuffer *buffer = NULL;
    AHardwareBuffer_Desc desc = {0};
    AHardwareBuffer *ahb;
    int fence = -1, id = -1, generation = 0;
    struct
    {
        int32_t status;
        int32_t width, height, stride, format, usage;
        int32_t buffer_id, generation;
        int32_t numFds, numInts;
    } reply = {0};

    reply.status = dequeueBuffer( &win->win, &buffer, &fence );
    wait_fence_and_close( fence );
    if (!reply.status)
    {
        ahb = ahb_from_anwb( win, buffer, &id, &generation );
        if (!ahb)
            reply.status = -EINVAL;
        else
        {
            pAHardwareBuffer_describe( ahb, &desc );
            reply.width = desc.width;
            reply.height = desc.height;
            reply.stride = desc.stride;
            reply.format = desc.format;
            reply.usage = desc.usage;
            reply.buffer_id = id;
            reply.generation = generation;
            /* The tip WSI bridge recognizes -1 as an AHardwareBuffer handle
             * serialized immediately after this fixed-size reply. */
            reply.numFds = AMPHORA_AHB_NUMFDS;
        }
    }

    if (amphora_write_full( adapter->sock, &reply, sizeof(reply) )) return -1;
    if (!reply.status && pAHardwareBuffer_sendHandleToUnixSocket( ahb, adapter->sock )) return -1;
    return 0;
}

static int amphora_adapter_queue( struct amphora_adapter *adapter, BOOL cancel )
{
    struct native_win_wrapper *win = adapter->win;
    int32_t id, generation, ret;
    struct ANativeWindowBuffer *buffer;

    if (amphora_read_full( adapter->sock, &id, sizeof(id) ) ||
        amphora_read_full( adapter->sock, &generation, sizeof(generation) )) return -1;
    if (id < 0 || id >= NB_CACHED_BUFFERS || !win->buffers[id].self ||
        win->buffers[id].generation != generation)
        ret = -EINVAL;
    else
    {
        buffer = anwb_from_ahb( win->buffers[id].self );
        ret = cancel ? cancelBuffer( &win->win, buffer, -1 )
                     : queueBuffer( &win->win, buffer, -1 );
    }
    return amphora_write_full( adapter->sock, &ret, sizeof(ret) );
}

static int amphora_adapter_query( struct amphora_adapter *adapter )
{
    int32_t what, ret, value = 0;

    if (amphora_read_full( adapter->sock, &what, sizeof(what) )) return -1;
    /* Prefer cached geometry so host win_query (during create) need not wait on
     * ioctl from this adapter thread; still ioctl if cache missing. */
    if ((what == NATIVE_WINDOW_WIDTH || what == NATIVE_WINDOW_DEFAULT_WIDTH) &&
        adapter->win->cached_width > 0)
    {
        value = adapter->win->cached_width;
        ret = 0;
    }
    else if ((what == NATIVE_WINDOW_HEIGHT || what == NATIVE_WINDOW_DEFAULT_HEIGHT) &&
             adapter->win->cached_height > 0)
    {
        value = adapter->win->cached_height;
        ret = 0;
    }
    else
        ret = query( &adapter->win->win, what, &value );
    if (amphora_write_full( adapter->sock, &ret, sizeof(ret) ) ||
        amphora_write_full( adapter->sock, &value, sizeof(value) )) return -1;
    return 0;
}

static int amphora_adapter_perform( struct amphora_adapter *adapter )
{
    struct ioctl_android_perform perf = {0};
    int32_t nargs, ret;

    perf.hdr.hwnd = HandleToLong( adapter->win->hwnd );
    perf.hdr.opengl = adapter->win->opengl;
    if (amphora_read_full( adapter->sock, &perf.operation, sizeof(perf.operation) ) ||
        amphora_read_full( adapter->sock, &nargs, sizeof(nargs) )) return -1;
    if (nargs < 0 || nargs > ARRAY_SIZE(perf.args)) return -1;
    if (nargs && amphora_read_full( adapter->sock, perf.args, nargs * sizeof(perf.args[0]) )) return -1;
    ret = android_ioctl( IOCTL_PERFORM, &perf, sizeof(perf), NULL, NULL, NULL );
    return amphora_write_full( adapter->sock, &ret, sizeof(ret) );
}

static void *amphora_adapter_thread( void *arg )
{
    struct amphora_adapter *adapter = arg;
    int32_t command, value, ret;

    while (!amphora_read_full( adapter->sock, &command, sizeof(command) ))
    {
        switch (command)
        {
        case AMPHORA_BUF_DEQUEUE:
            if (amphora_adapter_dequeue( adapter )) goto done;
            break;
        case AMPHORA_BUF_QUEUE:
            if (amphora_adapter_queue( adapter, FALSE )) goto done;
            break;
        case AMPHORA_BUF_CANCEL:
            if (amphora_adapter_queue( adapter, TRUE )) goto done;
            break;
        case AMPHORA_BUF_QUERY:
            if (amphora_adapter_query( adapter )) goto done;
            break;
        case AMPHORA_BUF_PERFORM:
            if (amphora_adapter_perform( adapter )) goto done;
            break;
        case AMPHORA_BUF_SET_SWAP:
            if (amphora_read_full( adapter->sock, &value, sizeof(value) )) goto done;
            ret = setSwapInterval( &adapter->win->win, value );
            if (amphora_write_full( adapter->sock, &ret, sizeof(ret) )) goto done;
            break;
        default:
            WARN( "unknown Amphora buffer command %d for hwnd %p\n", command, adapter->win->hwnd );
            goto done;
        }
    }
done:
    close( adapter->sock );
    free( adapter );
    return NULL;
}

static int create_amphora_adapter( struct native_win_wrapper *win )
{
    struct amphora_adapter *adapter;
    pthread_t thread;
    int socks[2];

    if (socketpair( AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, socks )) return -1;
    if (!(adapter = malloc( sizeof(*adapter) )))
    {
        close( socks[0] );
        close( socks[1] );
        return -1;
    }
    adapter->win = win;
    adapter->sock = socks[1];
    if (pthread_create( &thread, NULL, amphora_adapter_thread, adapter ))
    {
        close( socks[0] );
        close( socks[1] );
        free( adapter );
        return -1;
    }
    pthread_detach( thread );
    return socks[0];
}

struct ANativeWindow *create_ioctl_window( HWND hwnd, BOOL opengl )
{
    struct ioctl_android_create_window req;
    struct native_win_wrapper *win = calloc( 1, sizeof(*win) );

    if (!win) return NULL;

    win->win.common.magic             = ANDROID_NATIVE_WINDOW_MAGIC;
    win->win.common.version           = sizeof(ANativeWindow);
    win->win.common.incRef            = win_incRef;
    win->win.common.decRef            = win_decRef;
    win->win.setSwapInterval          = setSwapInterval;
    win->win.dequeueBuffer_DEPRECATED = dequeueBuffer_DEPRECATED;
    win->win.lockBuffer_DEPRECATED    = lockBuffer_DEPRECATED;
    win->win.queueBuffer_DEPRECATED   = queueBuffer_DEPRECATED;
    win->win.query                    = query;
    win->win.perform                  = perform;
    win->win.cancelBuffer_DEPRECATED  = cancelBuffer_DEPRECATED;
    win->win.dequeueBuffer            = dequeueBuffer;
    win->win.queueBuffer              = queueBuffer;
    win->win.cancelBuffer             = cancelBuffer;
    win->ref  = 1;
    win->hwnd = hwnd;
    win->opengl = opengl;
    win->amphora_sock = -1;
    TRACE( "-> %p %p opengl=%u\n", win, win->hwnd, opengl );

    req.hdr.hwnd = HandleToLong( win->hwnd );
    req.hdr.opengl = win->opengl;
    req.parent = get_ioctl_win_parent( NtUserGetAncestor( hwnd, GA_PARENT ));
    req.is_desktop = hwnd == desktop_window;
    android_ioctl( IOCTL_CREATE_WINDOW, &req, sizeof(req), NULL, NULL, NULL );

    pthread_mutex_lock( &amphora_windows_lock );
    amphora_windows[data_map_idx( hwnd, opengl )] = win;
    pthread_mutex_unlock( &amphora_windows_lock );
    return &win->win;
}

struct ANativeWindow *grab_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    InterlockedIncrement( &win->ref );
    return window;
}

void release_ioctl_window( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    unsigned int i;

    if (InterlockedDecrement( &win->ref ) > 0) return;

    TRACE( "%p %p\n", win, win->hwnd );
    pthread_mutex_lock( &amphora_windows_lock );
    if (amphora_windows[data_map_idx( win->hwnd, win->opengl )] == win)
        amphora_windows[data_map_idx( win->hwnd, win->opengl )] = NULL;
    pthread_mutex_unlock( &amphora_windows_lock );
    if (win->amphora_sock >= 0) close( win->amphora_sock );
    for (i = 0; i < ARRAY_SIZE( win->buffers ); i++)
        if (win->buffers[i].self) pAHardwareBuffer_release(win->buffers[i].self);

    destroy_ioctl_window( win->hwnd, win->opengl );
    free( win );
}

void destroy_ioctl_window( HWND hwnd, BOOL opengl )
{
    struct ioctl_android_destroy_window req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = opengl;
    android_ioctl( IOCTL_DESTROY_WINDOW, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_window_pos_changed( HWND hwnd, const struct window_rects *rects,
                              UINT style, UINT flags, HWND after, HWND owner )
{
    struct ioctl_android_window_pos_changed req;

    req.hdr.hwnd     = HandleToLong( hwnd );
    req.hdr.opengl   = FALSE;
    req.window_rect  = rects->window;
    req.client_rect  = rects->client;
    req.visible_rect = rects->visible;
    req.style        = style;
    req.flags        = flags;
    req.after        = HandleToLong( after );
    req.owner        = HandleToLong( owner );
    return android_ioctl( IOCTL_WINDOW_POS_CHANGED, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_window_parent( HWND hwnd, HWND parent )
{
    struct ioctl_android_set_window_parent req;

    req.hdr.hwnd = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    req.parent = get_ioctl_win_parent( parent );
    return android_ioctl( IOCTL_SET_WINDOW_PARENT, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_capture( HWND hwnd )
{
    struct ioctl_android_set_capture req;

    req.hdr.hwnd  = HandleToLong( hwnd );
    req.hdr.opengl = FALSE;
    return android_ioctl( IOCTL_SET_CAPTURE, &req, sizeof(req), NULL, NULL, NULL );
}

int ioctl_set_cursor( int id, int width, int height,
                      int hotspotx, int hotspoty, const unsigned int *bits )
{
    struct ioctl_android_set_cursor *req;
    unsigned int size = offsetof( struct ioctl_android_set_cursor, bits[width * height] );
    int ret;

    if (!(req = malloc( size ))) return -ENOMEM;
    req->hdr.hwnd   = 0;  /* unused */
    req->hdr.opengl = FALSE;
    req->id       = id;
    req->width    = width;
    req->height   = height;
    req->hotspotx = hotspotx;
    req->hotspoty = hotspoty;
    memcpy( req->bits, bits, width * height * sizeof(req->bits[0]) );
    ret = android_ioctl( IOCTL_SET_CURSOR, req, size, NULL, NULL, NULL );
    free( req );
    return ret;
}


/* Return only wrappers whose host ANativeWindow is ready.  The Vulkan path
 * polls this after CREATE_WINDOW, matching the host register/import wait. */
static struct ANativeWindow *get_amphora_window( HWND hwnd, BOOL opengl )
{
    struct native_win_wrapper *win;
    int width, height;

    pthread_mutex_lock( &amphora_windows_lock );
    win = amphora_windows[data_map_idx( hwnd, opengl )];
    pthread_mutex_unlock( &amphora_windows_lock );
    if (!win || query( &win->win, NATIVE_WINDOW_WIDTH, &width )) return NULL;
    if (query( &win->win, NATIVE_WINDOW_HEIGHT, &height )) return NULL;
    TRACE( "Amphora hwnd %p opengl %u wrapper %p size %dx%d\n", hwnd, opengl, win, width, height );
    return &win->win;
}

struct ANativeWindow *get_amphora_parent_window( HWND hwnd )
{
    return get_amphora_window( hwnd, FALSE );
}

struct ANativeWindow *get_amphora_client_window( HWND hwnd )
{
    return get_amphora_window( hwnd, TRUE );
}

/* Fetch tip-model host AMPHORA_BUF wine FD (SCM_RIGHTS) for hwnd+opengl. */
static int ioctl_get_buffer_sock( HWND hwnd, BOOL opengl, int *out_fd )
{
    struct ioctl_header req;
    int ret, fd = -1;

    if (!out_fd) return -EINVAL;
    *out_fd = -1;
    req.hwnd = HandleToLong( hwnd );
    req.opengl = opengl;
    ret = android_ioctl( IOCTL_GET_BUFFER_SOCK, &req, sizeof(req), NULL, NULL, &fd );
    if (ret) return ret;
    if (fd < 0) return -ENOENT;
    *out_fd = fd;
    return 0;
}

int amphora_native_window_sock( struct ANativeWindow *window )
{
    struct native_win_wrapper *win = (struct native_win_wrapper *)window;
    int sock, i, ret, host_fd = -1;

    if (!window || window->dequeueBuffer != dequeueBuffer || window->perform != perform) return -1;
    pthread_mutex_lock( &amphora_windows_lock );
    sock = win->amphora_sock;
    pthread_mutex_unlock( &amphora_windows_lock );
    if (sock >= 0)
    {
        TRACE( "Amphora hwnd %p opengl %u cached sock %d\n", win->hwnd, win->opengl, sock );
        return sock;
    }

    /* Prefer host AMPHORA_BUF sock (tip model). Retry briefly while registerSurface races. */
    for (i = 0; i < 50; i++)
    {
        ret = ioctl_get_buffer_sock( win->hwnd, win->opengl, &host_fd );
        if (!ret && host_fd >= 0)
        {
            pthread_mutex_lock( &amphora_windows_lock );
            if (win->amphora_sock < 0)
                win->amphora_sock = host_fd;
            else
            {
                close( host_fd );
                host_fd = win->amphora_sock;
            }
            sock = win->amphora_sock;
            pthread_mutex_unlock( &amphora_windows_lock );
            ERR( "amphora hwnd %p opengl %u host AMPHORA_BUF sock %d (ioctl)\n",
                 win->hwnd, win->opengl, sock );
            return sock;
        }
        if (ret == -ENOTSUP || ret == -EINVAL)
            break; /* old host without IOCTL_GET_BUFFER_SOCK */
        if (host_fd >= 0) { close( host_fd ); host_fd = -1; }
        usleep( 40000 );
    }

    /* Fallback: local stream adapter that forwards AMPHORA_BUF → ioctl ops. */
    sock = create_amphora_adapter( win );
    pthread_mutex_lock( &amphora_windows_lock );
    if (win->amphora_sock < 0)
        win->amphora_sock = sock;
    else if (sock >= 0)
    {
        close( sock );
        sock = win->amphora_sock;
    }
    else
        sock = win->amphora_sock;
    pthread_mutex_unlock( &amphora_windows_lock );
    ERR( "amphora hwnd %p opengl %u adapter sock %d (fallback)\n", win->hwnd, win->opengl, sock );
    return sock;
}

/**********************************************************************
 *           ANDROID_SetDesktopWindow
 */
void ANDROID_SetDesktopWindow( HWND hwnd )
{
    desktop_window = hwnd;
}
