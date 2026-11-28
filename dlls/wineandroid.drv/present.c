/*
 * Swapchain presentation: the host process side, and the messages
 *
 * Copyright 2026 Amphora
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

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "android.h"
#include "wine/list.h"

/* This side runs in the Java process, where Wine's debug channels do not exist. */
#define LOG( prio, fmt, ... ) \
    p__android_log_print( ANDROID_LOG_ ## prio, "wineandroid.drv", "%s: " fmt, __func__, ##__VA_ARGS__ )

/* Both processes: one message, with at most one file descriptor. The descriptor stays the caller's. */
int present_send( int socket, const struct present_message *msg, int fd )
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { (void *)msg, sizeof(*msg) };
    struct msghdr hdr = { .msg_iov = &iov, .msg_iovlen = 1 };
    ssize_t ret;

    if (fd >= 0)
    {
        struct cmsghdr *cmsg;

        memset( control, 0, sizeof(control) );
        hdr.msg_control = control;
        hdr.msg_controllen = sizeof(control);
        cmsg = CMSG_FIRSTHDR( &hdr );
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN( sizeof(fd) );
        memcpy( CMSG_DATA( cmsg ), &fd, sizeof(fd) );
    }

    do ret = sendmsg( socket, &hdr, MSG_NOSIGNAL );
    while (ret < 0 && errno == EINTR);
    return ret == sizeof(*msg) ? 0 : -1;
}

/* Returns 1 with a message (the descriptor, if any, is the caller's to close),
 * 0 when there is none and wait is not set, -1 when the other side is gone. */
int present_recv( int socket, struct present_message *msg, int *fd, BOOL wait )
{
    char control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = { msg, sizeof(*msg) };
    struct msghdr hdr = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = control, .msg_controllen = sizeof(control) };
    struct cmsghdr *cmsg;
    ssize_t ret;

    *fd = -1;
    do ret = recvmsg( socket, &hdr, MSG_CMSG_CLOEXEC | (wait ? 0 : MSG_DONTWAIT) );
    while (ret < 0 && errno == EINTR);
    if (ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
    if (ret <= 0) return -1;

    for (cmsg = CMSG_FIRSTHDR( &hdr ); cmsg; cmsg = CMSG_NXTHDR( &hdr, cmsg ))
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS && cmsg->cmsg_len >= CMSG_LEN( sizeof(int) ))
            memcpy( fd, CMSG_DATA( cmsg ), sizeof(int) );

    if (ret == sizeof(*msg)) return 1;
    if (*fd >= 0) close( *fd );
    *fd = -1;
    return -1;
}


/* Host process side */

struct swapchain
{
    int              id;
    int              count;
    int              width;
    int              height;
    int              flags;
    AHardwareBuffer *buffers[PRESENT_MAX_IMAGES];
};

#define MAX_SWAPCHAINS 4  /* at once on a surface: the current one and the ones it replaced, until they are destroyed */

struct frame
{
    int swapchain;  /* 0 for none */
    int image;
    int fence;      /* of its rendering, -1 for none or once the compositor has it */
};

/* The Vulkan surface of one window, as the host sees it. */
struct presenter
{
    struct list      entry;
    LONG             ref;
    HWND             hwnd;
    int              socket;
    pthread_mutex_t  lock;
    ANativeWindow   *window;      /* surface of the client view, NULL while it has none */
    ASurfaceControl *layer;       /* our layer in it */
    struct swapchain swapchains[MAX_SWAPCHAINS];
    struct frame     shown;       /* given to the compositor and not released yet */
    struct frame     waiting;     /* newest frame not given to the compositor yet (when not PRESENT_FIFO) */
    BOOL             in_flight;   /* a transaction was applied and has not completed */
    struct frame     held[PRESENT_MAX_IMAGES];  /* presented while there is no layer, oldest first */
    int              held_count;
};

/* Without a layer (the app is in the background, the window is hidden) nothing paces the program:
 * frames it presents are held and given back one at a time, this far apart. */
#define HELD_FRAME_INTERVAL 100 /* ms */

/* What a transaction's completion needs. */
struct completion
{
    struct presenter *presenter;
    ASurfaceControl  *layer;
    struct frame      replaced;   /* the frame this transaction took off the layer */
};

static struct list presenters = LIST_INIT( presenters );
static pthread_mutex_t presenters_lock = PTHREAD_MUTEX_INITIALIZER;

static const struct frame no_frame = { 0, -1, -1 };

static void release_swapchain( struct swapchain *swapchain )
{
    int i;

    for (i = 0; i < swapchain->count; i++) if (swapchain->buffers[i]) pAHardwareBuffer_release( swapchain->buffers[i] );
    memset( swapchain, 0, sizeof(*swapchain) );
}

static struct swapchain *find_swapchain( struct presenter *presenter, int id )
{
    int i;

    for (i = 0; i < MAX_SWAPCHAINS; i++) if (presenter->swapchains[i].id == id) return &presenter->swapchains[i];
    return NULL;
}

/* Takes the layer out of the tree and drops it. */
static void remove_layer( struct presenter *presenter )
{
    ASurfaceTransaction *transaction;

    if (!presenter->layer) return;
    if ((transaction = pASurfaceTransaction_create()))
    {
        pASurfaceTransaction_reparent( transaction, presenter->layer, NULL );
        pASurfaceTransaction_apply( transaction );
        pASurfaceTransaction_delete( transaction );
    }
    pASurfaceControl_release( presenter->layer );
    presenter->layer = NULL;
}

static void presenter_release( struct presenter *presenter )
{
    int i;

    if (InterlockedDecrement( &presenter->ref )) return;
    remove_layer( presenter );
    if (presenter->window) pANativeWindow_release( presenter->window );
    for (i = 0; i < MAX_SWAPCHAINS; i++) release_swapchain( &presenter->swapchains[i] );
    if (presenter->shown.fence >= 0) close( presenter->shown.fence );
    if (presenter->waiting.fence >= 0) close( presenter->waiting.fence );
    for (i = 0; i < presenter->held_count; i++) if (presenter->held[i].fence >= 0) close( presenter->held[i].fence );
    close( presenter->socket );
    pthread_mutex_destroy( &presenter->lock );
    free( presenter );
}

/* Tells the Wine process that an image is free again. Closes the frame's fence. An image is released
 * once for each time it was presented, so what waits in the socket is bounded by the images there are,
 * and sending does not block. */
static void send_release( struct presenter *presenter, struct frame *frame )
{
    struct present_message msg = { .type = PRESENT_RELEASE, .swapchain = frame->swapchain, .image = frame->image };

    if (frame->swapchain) present_send( presenter->socket, &msg, frame->fence );
    if (frame->fence >= 0) close( frame->fence );
    *frame = no_frame;
}

static void submit_frame( struct presenter *presenter, struct frame *frame );

/* Keeps a frame presented while there is no layer. Takes the frame's fence. */
static void hold_frame( struct presenter *presenter, struct frame *frame )
{
    if (presenter->held_count == PRESENT_MAX_IMAGES)
    {
        send_release( presenter, &presenter->held[0] );
        memmove( presenter->held, presenter->held + 1, --presenter->held_count * sizeof(*presenter->held) );
    }
    presenter->held[presenter->held_count++] = *frame;
    *frame = no_frame;
}

/* Gives the oldest held frame back to the program. */
static void release_held_frame( struct presenter *presenter )
{
    if (!presenter->held_count) return;
    send_release( presenter, &presenter->held[0] );
    memmove( presenter->held, presenter->held + 1, --presenter->held_count * sizeof(*presenter->held) );
}

/* The transaction was presented, or dropped for a later one: what it replaced can be reused. */
static void transaction_complete( void *context, ASurfaceTransactionStats *stats )
{
    struct completion *completion = context;
    struct presenter *presenter = completion->presenter;

    completion->replaced.fence = pASurfaceTransactionStats_getPreviousReleaseFenceFd( stats, completion->layer );
    send_release( presenter, &completion->replaced );

    pthread_mutex_lock( &presenter->lock );
    /* a transaction on a layer that was replaced since says nothing about the one on the current layer */
    if (completion->layer == presenter->layer)
    {
        presenter->in_flight = FALSE;
        if (presenter->waiting.swapchain)
        {
            struct frame frame = presenter->waiting;
            presenter->waiting = no_frame;
            submit_frame( presenter, &frame );
        }
    }
    pthread_mutex_unlock( &presenter->lock );

    pASurfaceControl_release( completion->layer );
    presenter_release( presenter );
    free( completion );
}

/* Gives a frame to the compositor, or holds it while there is no layer. Takes the frame's fence. */
static void submit_frame( struct presenter *presenter, struct frame *frame )
{
    struct swapchain *swapchain = find_swapchain( presenter, frame->swapchain );
    ASurfaceTransaction *transaction;
    struct completion *completion;
    ARect source, destination;
    int width, height;

    if (!swapchain || frame->image < 0 || frame->image >= swapchain->count)
    {
        /* nothing to show; whoever sent it must not wait for it forever */
        send_release( presenter, frame );
        return;
    }
    if (!presenter->layer)
    {
        hold_frame( presenter, frame );
        return;
    }
    if (!(completion = malloc( sizeof(*completion) )))
    {
        send_release( presenter, frame );
        return;
    }
    if (!(transaction = pASurfaceTransaction_create()))
    {
        free( completion );
        send_release( presenter, frame );
        return;
    }

    /* the view is the size of the window's client area; a swapchain of another size (a resize under way) is scaled to it */
    width = pANativeWindow_getWidth( presenter->window );
    height = pANativeWindow_getHeight( presenter->window );
    source.left = source.top = destination.left = destination.top = 0;
    source.right = swapchain->width;
    source.bottom = swapchain->height;
    destination.right = width > 0 ? width : swapchain->width;
    destination.bottom = height > 0 ? height : swapchain->height;

    /* the transaction owns the fence from here on */
    pASurfaceTransaction_setBuffer( transaction, presenter->layer, swapchain->buffers[frame->image], frame->fence );
    pASurfaceTransaction_setGeometry( transaction, presenter->layer, &source, &destination, ANATIVEWINDOW_TRANSFORM_IDENTITY );
    pASurfaceTransaction_setBufferTransparency( transaction, presenter->layer,
                                                (swapchain->flags & PRESENT_OPAQUE) ? ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE
                                                                                    : ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT );
    pASurfaceTransaction_setVisibility( transaction, presenter->layer, ASURFACE_TRANSACTION_VISIBILITY_SHOW );
    /* in order, one per refresh; without it the compositor drops a frame that a newer one overtakes */
    if (pASurfaceTransaction_setEnableBackPressure)
        pASurfaceTransaction_setEnableBackPressure( transaction, presenter->layer, (swapchain->flags & PRESENT_FIFO) != 0 );

    completion->presenter = presenter;
    completion->layer = presenter->layer;
    completion->replaced = presenter->shown;
    InterlockedIncrement( &presenter->ref );
    pASurfaceControl_acquire( presenter->layer );
    pASurfaceTransaction_setOnComplete( transaction, completion, transaction_complete );

    presenter->shown = *frame;
    presenter->shown.fence = -1;
    presenter->in_flight = TRUE;
    *frame = no_frame;

    pASurfaceTransaction_apply( transaction );
    pASurfaceTransaction_delete( transaction );
}

static void handle_frame( struct presenter *presenter, const struct present_message *msg, int fence )
{
    struct swapchain *swapchain = find_swapchain( presenter, msg->swapchain );
    struct frame frame = { msg->swapchain, msg->image, fence };

    if (!swapchain)
    {
        if (fence >= 0) close( fence );
        return;
    }

    /* Every frame, in order: the compositor queues them and the images run out, which paces the program. */
    if ((swapchain->flags & PRESENT_FIFO) || !presenter->in_flight)
    {
        submit_frame( presenter, &frame );
        return;
    }

    /* Newest frame only: one transaction per refresh is all the compositor can show. A frame that a
     * newer one overtakes while it waits here never leaves this process, so the program gets its
     * image back at once and is not held to the refresh rate. */
    if (presenter->waiting.swapchain) send_release( presenter, &presenter->waiting );
    presenter->waiting = frame;
}

static int handle_create( struct presenter *presenter, const struct present_message *msg )
{
    struct swapchain *swapchain;
    int i, ret;

    if (msg->count < 1 || msg->count > PRESENT_MAX_IMAGES || msg->swapchain <= 0 || find_swapchain( presenter, msg->swapchain ))
        return -1;
    if (!(swapchain = find_swapchain( presenter, 0 )))
    {
        LOG( ERROR, "%p: more than %d swapchains", presenter->hwnd, MAX_SWAPCHAINS );
        return -1;
    }

    /* each buffer is one packet, sent right behind the message that announced them */
    for (i = 0; i < msg->count; i++)
    {
        if ((ret = pAHardwareBuffer_recvHandleFromUnixSocket( presenter->socket, &swapchain->buffers[i] )))
        {
            LOG( ERROR, "%p: image %d of %d not received: %d", presenter->hwnd, i, msg->count, ret );
            swapchain->count = i;
            release_swapchain( swapchain );
            return -1;
        }
    }
    swapchain->id = msg->swapchain;
    swapchain->count = msg->count;
    swapchain->width = msg->width;
    swapchain->height = msg->height;
    swapchain->flags = msg->flags;
    LOG( INFO, "%p: swapchain %d, %d images %dx%d, flags %#x", presenter->hwnd, msg->swapchain, msg->count,
         msg->width, msg->height, msg->flags );
    return 0;
}

static void handle_destroy( struct presenter *presenter, const struct present_message *msg )
{
    struct swapchain *swapchain = find_swapchain( presenter, msg->swapchain );
    int i, j;

    if (!swapchain || !msg->swapchain) return;
    /* The compositor keeps the buffer on the layer alive, so the last frame stays up until the next
     * swapchain's first one. Nobody waits for these images any more. */
    if (presenter->shown.swapchain == msg->swapchain) presenter->shown = no_frame;
    if (presenter->waiting.swapchain == msg->swapchain)
    {
        if (presenter->waiting.fence >= 0) close( presenter->waiting.fence );
        presenter->waiting = no_frame;
    }
    for (i = j = 0; i < presenter->held_count; i++)
    {
        if (presenter->held[i].swapchain != msg->swapchain) presenter->held[j++] = presenter->held[i];
        else if (presenter->held[i].fence >= 0) close( presenter->held[i].fence );
    }
    presenter->held_count = j;
    release_swapchain( swapchain );
}

static void *presenter_thread( void *arg )
{
    struct presenter *presenter = arg;
    struct pollfd pfd = { .fd = presenter->socket, .events = POLLIN };
    struct present_message msg;
    int fd, ret = 0;

    for (;;)
    {
        pthread_mutex_lock( &presenter->lock );
        ret = presenter->held_count ? HELD_FRAME_INTERVAL : -1;
        pthread_mutex_unlock( &presenter->lock );

        if (!(ret = poll( &pfd, 1, ret )))
        {
            pthread_mutex_lock( &presenter->lock );
            if (!presenter->layer) release_held_frame( presenter );
            pthread_mutex_unlock( &presenter->lock );
            continue;
        }
        if (ret < 0 && errno == EINTR) continue;
        if (ret < 0 || present_recv( presenter->socket, &msg, &fd, FALSE ) != 1) break;
        ret = 0;

        pthread_mutex_lock( &presenter->lock );
        switch (msg.type)
        {
        case PRESENT_CREATE:
            if (fd >= 0) close( fd );
            ret = handle_create( presenter, &msg );
            break;
        case PRESENT_FRAME:
            handle_frame( presenter, &msg, fd );
            break;
        case PRESENT_DESTROY:
            if (fd >= 0) close( fd );
            handle_destroy( presenter, &msg );
            break;
        default:
            if (fd >= 0) close( fd );
            ret = -1;
            break;
        }
        pthread_mutex_unlock( &presenter->lock );
        if (ret) break;
    }

    /* the Wine process closed the surface, or died, or sent something we do not understand */
    pthread_mutex_lock( &presenters_lock );
    list_remove( &presenter->entry );
    pthread_mutex_unlock( &presenters_lock );

    pthread_mutex_lock( &presenter->lock );
    shutdown( presenter->socket, SHUT_RDWR );
    remove_layer( presenter );
    pthread_mutex_unlock( &presenter->lock );
    presenter_release( presenter );
    return NULL;
}

/* Puts our layer into the client view's surface, with the last frame if there is one. */
static void create_layer( struct presenter *presenter )
{
    if (!presenter->window) return;
    if (!(presenter->layer = pASurfaceControl_createFromWindow( presenter->window, "wine-vulkan" )))
    {
        LOG( ERROR, "%p: no layer in window %p (not a SurfaceView's surface?)", presenter->hwnd, presenter->window );
        return;
    }
    if (presenter->held_count)
    {
        /* the newest of what was presented meanwhile; it takes the old layer's frame off our hands */
        struct frame frame = presenter->held[presenter->held_count - 1];
        presenter->held_count--;
        while (presenter->held_count) release_held_frame( presenter );
        submit_frame( presenter, &frame );
    }
    else if (presenter->shown.swapchain)
    {
        /* still ours: it was on the old layer, which will not report it released */
        struct frame frame = presenter->shown;
        presenter->shown = no_frame;
        submit_frame( presenter, &frame );
    }
}

/* The client view of a window got a surface, another one, or lost it (NULL). Main thread. */
void present_surface_changed( HWND hwnd, struct ANativeWindow *window )
{
    struct presenter *presenter;

    pthread_mutex_lock( &presenters_lock );
    LIST_FOR_EACH_ENTRY( presenter, &presenters, struct presenter, entry )
    {
        if (presenter->hwnd != hwnd) continue;
        pthread_mutex_lock( &presenter->lock );
        if (presenter->window != window)
        {
            remove_layer( presenter );
            /* no completion is owed for a layer that is gone */
            presenter->in_flight = FALSE;
            if (presenter->waiting.swapchain) hold_frame( presenter, &presenter->waiting );
            if (presenter->window) pANativeWindow_release( presenter->window );
            if ((presenter->window = window)) pANativeWindow_acquire( window );
            create_layer( presenter );
        }
        pthread_mutex_unlock( &presenter->lock );
    }
    pthread_mutex_unlock( &presenters_lock );
}

/* A Vulkan surface was created for the window. Returns the Wine process's end of its socket, or -errno. Main thread. */
int present_connect( HWND hwnd, struct ANativeWindow *window )
{
    struct presenter *presenter;
    pthread_attr_t attr;
    pthread_t thread;
    int fds[2];

    if (socketpair( AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds ) < 0) return -errno;
    if (!(presenter = calloc( 1, sizeof(*presenter) )))
    {
        close( fds[0] );
        close( fds[1] );
        return -ENOMEM;
    }
    presenter->ref = 1;
    presenter->hwnd = hwnd;
    presenter->socket = fds[0];
    presenter->shown = no_frame;
    presenter->waiting = no_frame;
    pthread_mutex_init( &presenter->lock, NULL );
    if ((presenter->window = window)) pANativeWindow_acquire( window );
    create_layer( presenter );

    pthread_mutex_lock( &presenters_lock );
    list_add_tail( &presenters, &presenter->entry );
    pthread_mutex_unlock( &presenters_lock );

    pthread_attr_init( &attr );
    pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
    if (pthread_create( &thread, &attr, presenter_thread, presenter ))
    {
        pthread_attr_destroy( &attr );
        pthread_mutex_lock( &presenters_lock );
        list_remove( &presenter->entry );
        pthread_mutex_unlock( &presenters_lock );
        presenter_release( presenter );
        close( fds[1] );
        return -EAGAIN;
    }
    pthread_attr_destroy( &attr );
    LOG( INFO, "%p: surface connected, window %p", hwnd, window );
    return fds[1];
}
