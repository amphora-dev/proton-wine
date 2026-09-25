/* Android Vulkan driver stub
 *
 * Copyright 2017 Roderick Colenbrander
 * Copyright 2026 CodeWeavers / Amphora
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

/* NOTE: If making changes here, consider whether they should be reflected in
 * the other drivers. */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"

#include "android.h"
#include "wine/debug.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* wine/vulkan.h (generated for this pin) already provides
 * VkAndroidSurfaceCreateInfoKHR / VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR.
 * Do not redeclare them locally: the structure-type token is an enum value, not a
 * #define, so #ifndef would always succeed and redefine the typedef.
 * Resolve vkCreateAndroidSurfaceKHR via the host loader (libvulkan.so). */

typedef VkResult (*PFN_android_vkCreateAndroidSurfaceKHR)( VkInstance, const VkAndroidSurfaceCreateInfoKHR *,
                                                           const VkAllocationCallbacks *, VkSurfaceKHR * );
typedef void *(*PFN_android_vkGetInstanceProcAddr)( VkInstance, const char * );

static void *android_vulkan_handle;
extern void amphora_note_acquire_signal( VkSemaphore host_sem, VkFence host_fence );
static PFN_android_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;

static const struct vulkan_driver_funcs android_vulkan_driver_funcs;

static int amphora_wsi_create_surface( uint64_t vk_instance, int sock,
                                        struct ANativeWindow *window, uint64_t *out_surface )
{
    char path[128];
    struct sockaddr_un addr;
    int fd = -1, i, q;
    int32_t sock32 = sock, reply = -EIO, width = 640, height = 480;
    uint64_t surface = 0;

    if (out_surface) *out_surface = 0;

    /* Query ANW size on the wine thread (ioctl works here) before connecting to
     * the aarch64 WSI helper — host create must not sock-query the adapter. */
    if (sock >= 0 && window && window->query)
    {
        if (!window->query( window, NATIVE_WINDOW_WIDTH, &q ) && q > 0) width = q;
        if (!window->query( window, NATIVE_WINDOW_HEIGHT, &q ) && q > 0) height = q;
    }

    snprintf( path, sizeof(path), "/data/user/0/app.amphora/files/wineandroid/wsi-%d.sock",
              (int)getpid() );
    ERR( "amphora WSI bridge connect %s inst=0x%s sock=%d size=%dx%d\n",
         path, wine_dbgstr_longlong( vk_instance ), sock, width, height );

    for (i = 0; i < 50; i++)
    {
        fd = socket( AF_UNIX, SOCK_STREAM, 0 );
        if (fd < 0) return -errno;
        memset( &addr, 0, sizeof(addr) );
        addr.sun_family = AF_UNIX;
        memcpy( addr.sun_path, path, strlen(path) + 1 );
        if (!connect( fd, (struct sockaddr *)&addr, sizeof(addr) )) break;
        close( fd );
        fd = -1;
        usleep( 40000 );
    }
    if (fd < 0)
    {
        ERR( "amphora WSI bridge connect failed %s errno=%d\n", path, errno );
        return -ENOENT;
    }
    if (write( fd, &vk_instance, sizeof(vk_instance) ) != (ssize_t)sizeof(vk_instance) ||
        write( fd, &sock32, sizeof(sock32) ) != (ssize_t)sizeof(sock32) ||
        write( fd, &width, sizeof(width) ) != (ssize_t)sizeof(width) ||
        write( fd, &height, sizeof(height) ) != (ssize_t)sizeof(height) ||
        read( fd, &reply, sizeof(reply) ) != (ssize_t)sizeof(reply) ||
        read( fd, &surface, sizeof(surface) ) != (ssize_t)sizeof(surface) )
    {
        ERR( "amphora WSI bridge ipc failed errno=%d\n", errno );
        close( fd );
        return -EIO;
    }
    close( fd );
    if (out_surface) *out_surface = surface;
    ERR( "amphora WSI bridge reply ret=%d surface=0x%s\n", reply, wine_dbgstr_longlong( surface ) );
    return reply;
}


struct android_vulkan_surface
{
    struct client_surface client;
    struct ANativeWindow *window;
    BOOL amphora_parent; /* release via ANW incRef/decRef, not ioctl wrapper */
};

static struct android_vulkan_surface *impl_from_client_surface( struct client_surface *client )
{
    return CONTAINING_RECORD( client, struct android_vulkan_surface, client );
}

static void android_vulkan_client_surface_destroy( struct client_surface *client )
{
    struct android_vulkan_surface *surface = impl_from_client_surface( client );

    TRACE( "%s\n", debugstr_client_surface( client ) );
    android_set_vulkan_direct( client->hwnd, FALSE );
    if (!surface->window) return;
    if (surface->amphora_parent)
        surface->window->common.decRef( &surface->window->common );
    else
        release_ioctl_window( surface->window );
}

static void android_vulkan_client_surface_detach( struct client_surface *client )
{
}

static void android_vulkan_client_surface_update( struct client_surface *client )
{
}

static void android_vulkan_client_surface_present( struct client_surface *client, HDC hdc )
{
    (void)client;
    (void)hdc;
}

static const struct client_surface_funcs android_vulkan_client_surface_funcs =
{
    .destroy = android_vulkan_client_surface_destroy,
    .detach = android_vulkan_client_surface_detach,
    .update = android_vulkan_client_surface_update,
    .present = android_vulkan_client_surface_present,
};

static VkResult ANDROID_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *handle, struct client_surface **client )
{
    VkAndroidSurfaceCreateInfoKHR info =
    {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
    };
    struct android_vulkan_surface *surface;
    PFN_android_vkCreateAndroidSurfaceKHR p_vkCreateAndroidSurfaceKHR;
    VkResult res;

    const char *amphora = getenv( "AMPHORA_WINEANDROID" );

    TRACE( "%p %u %p %p %p\n", hwnd, raw, instance, handle, client );
    (void)raw;

    if (!(surface = (struct android_vulkan_surface *)client_surface_create( sizeof(*surface),
                                                                             &android_vulkan_client_surface_funcs, hwnd )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;

    /* Dedicated client ANW (CREATE_WINDOW opengl=1). Do NOT use the GDI parent
     * (opengl=0) that desktop/winefile LOCK — that ANW is NATIVE_WINDOW_IN_USE. */
    if (amphora && amphora[0] == '1' && amphora[1] == '\0')
    {
        struct ANativeWindow *tmp;
        int i;
        /* Mark before CREATE_WINDOW so GDI expose/erase on client bind is skipped. */
        android_set_vulkan_direct( hwnd, TRUE );
        tmp = create_ioctl_window( hwnd, TRUE ); /* scale arg not on proton_11.0 tip-base */
        /* Keep the ioctl wrapper alive: last-ref release sends DESTROY_WINDOW. */
        if (!tmp)
            ERR( "amphora vulkan hwnd=%p create_ioctl_window(client) failed\n", hwnd );
        for (i = 0; i < 50 && !surface->window; i++)
        {
            surface->window = get_amphora_client_window( hwnd );
            if (surface->window)
            {
                surface->window->common.incRef( &surface->window->common );
                surface->amphora_parent = TRUE;
                ERR( "amphora vulkan own ANW hwnd=%p anw=%p (client/opengl, not GDI parent)\n",
                     hwnd, surface->window );
                break;
            }
            usleep( 40000 );
        }
        if (!surface->window)
            ERR( "amphora vulkan hwnd=%p no client ANW after wait\n", hwnd );
    }

    /* Sock-proxy ANW is x86_64/Box64 and cannot enter aarch64
     * vkCreateAndroidSurfaceKHR. The aarch64 helper rebuilds an ANW over the
     * same Amphora client sock and creates the Android surface on THAT hwnd
     * ANW (no ImageReader blit) so PE QueuePresent DEQUEUE/QUEUEs the
     * dedicated Amphora window Surface directly. */
    if (surface->window && surface->amphora_parent)
    {
        int sock = amphora_native_window_sock( surface->window );
        uint64_t host_surface = 0;
        int wret;

        ERR( "amphora PE WSI bridge hwnd=%p anw=%p sock=%d inst=%p\n",
             hwnd, surface->window, sock, instance->host.instance );
        wret = amphora_wsi_create_surface( (uint64_t)(UINT_PTR)instance->host.instance,
                                           sock, surface->window, &host_surface );
        ERR( "amphora PE WSI bridge hwnd=%p ret=%d surface=0x%s\n",
             hwnd, wret, wine_dbgstr_longlong( host_surface ) );
        if (wret || !host_surface)
        {
            client_surface_release( &surface->client );
            return wret == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR ? VK_ERROR_NATIVE_WINDOW_IN_USE_KHR
                 : VK_ERROR_SURFACE_LOST_KHR;
        }
        *handle = (VkSurfaceKHR)(UINT_PTR)host_surface;
        *client = &surface->client;
        return VK_SUCCESS;
    }

    if (!surface->window)
    {
        if (!(surface->window = get_client_window( hwnd )))
        {
            ERR( "Failed to get ANativeWindow for hwnd %p\n", hwnd );
            client_surface_release( &surface->client );
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        surface->amphora_parent = FALSE;
        ERR( "vulkan surface hwnd=%p anw=%p (ioctl client)\n", hwnd, surface->window );
    }

    if (!p_vkGetInstanceProcAddr)
    {
        FIXME( "vkGetInstanceProcAddr not available, cannot create VK_KHR_android_surface\n" );
        client_surface_release( &surface->client );
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    p_vkCreateAndroidSurfaceKHR = (PFN_android_vkCreateAndroidSurfaceKHR)
        p_vkGetInstanceProcAddr( instance->host.instance, "vkCreateAndroidSurfaceKHR" );
    if (!p_vkCreateAndroidSurfaceKHR)
    {
        FIXME( "vkCreateAndroidSurfaceKHR not found on host instance\n" );
        client_surface_release( &surface->client );
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    /* Host instance gets VK_KHR_android_surface via map_instance_extensions
     * (android platform is UNEXPOSED in winevulkan make_vulkan). */
    info.window = surface->window;
    res = p_vkCreateAndroidSurfaceKHR( instance->host.instance, &info, NULL /* allocator */, handle );
    if (res != VK_SUCCESS)
    {
        ERR( "Failed to create Android surface, res=%d\n", res );
        client_surface_release( &surface->client );
        return res ? res : VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    *client = &surface->client;
    ERR( "amphora vkCreateAndroidSurfaceKHR hwnd=%p anw=%p surface=0x%s res=0\n",
         hwnd, surface->window, wine_dbgstr_longlong( *handle ) );
    TRACE( "Created surface 0x%s, client %s\n", wine_dbgstr_longlong( *handle ),
           debugstr_client_surface( *client ) );
    return VK_SUCCESS;
}

static VkBool32 ANDROID_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device,
                                                                  uint32_t index )
{
    TRACE( "%p %u\n", physical_device, index );
    (void)physical_device;
    (void)index;
    /* Android loader WSI is always presentable for graphics queues we expose. */
    return VK_TRUE;
}

static void ANDROID_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    /* Apps see Win32 WSI; host enables VK_KHR_android_surface (UNEXPOSED). */
    if (extensions->has_VK_KHR_win32_surface) extensions->has_VK_KHR_android_surface = 1;
    if (extensions->has_VK_KHR_android_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void ANDROID_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
    /* Host Android loader provides swapchain under the same name. */
    extensions->has_VK_KHR_swapchain = 1;
}

static const struct vulkan_driver_funcs android_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = ANDROID_vulkan_surface_create,
    .p_get_physical_device_presentation_support = ANDROID_get_physical_device_presentation_support,
    .p_map_instance_extensions = ANDROID_map_instance_extensions,
    .p_map_device_extensions = ANDROID_map_device_extensions,
};


/* Source-path AHB swapchain. x86_64 wine cannot dlsym aarch64
 * libamphora_wsi.so; IPC to wsi-sc-%pid.sock which calls amphora_ahb_sc_*.
 * No runtime table/GIPA/GDPA hooks. */
#define AMPHORA_SC_SOCK_FMT "/data/user/0/app.amphora/files/wineandroid/wsi-sc-%d.sock"
enum {
    AMPHORA_SC_OP_CREATE = 1,
    AMPHORA_SC_OP_DESTROY = 2,
    AMPHORA_SC_OP_GET_IMAGES = 3,
    AMPHORA_SC_OP_ACQUIRE = 4,
    AMPHORA_SC_OP_PRESENT = 5,
    AMPHORA_SC_OP_STASH = 6,
};

static int amphora_sc_connect(void)
{
    char path[128];
    struct sockaddr_un addr;
    int fd, i;
    snprintf( path, sizeof(path), AMPHORA_SC_SOCK_FMT, (int)getpid() );
    for (i = 0; i < 50; i++)
    {
        fd = socket( AF_UNIX, SOCK_STREAM, 0 );
        if (fd < 0) return -1;
        memset( &addr, 0, sizeof(addr) );
        addr.sun_family = AF_UNIX;
        memcpy( addr.sun_path, path, strlen(path) + 1 );
        if (!connect( fd, (struct sockaddr *)&addr, sizeof(addr) )) return fd;
        close( fd );
        usleep( 40000 );
    }
    ERR( "amphora sc connect failed %s errno=%d\n", path, errno );
    return -1;
}

static int amphora_sc_io_write( int fd, const void *buf, size_t n )
{
    const char *p = buf;
    while (n)
    {
        ssize_t w = write( fd, p, n );
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        p += w; n -= (size_t)w;
    }
    return 0;
}

static int amphora_sc_io_read( int fd, void *buf, size_t n )
{
    char *p = buf;
    while (n)
    {
        ssize_t r = read( fd, p, n );
        if (r <= 0) { if (r < 0 && errno == EINTR) continue; return -1; }
        p += r; n -= (size_t)r;
    }
    return 0;
}

__attribute__((visibility("default")))
void amphora_wine_vkStashDevice( VkDevice device, VkPhysicalDevice phys )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_STASH, ret = -1;
    uint64_t d = (uint64_t)(UINT_PTR)device, p = (uint64_t)(UINT_PTR)phys;
    const char *amphora = getenv( "AMPHORA_WINEANDROID" );
    if (!amphora || amphora[0] != '1' || amphora[1] != '\0') return;
    fd = amphora_sc_connect();
    if (fd < 0) return;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &d, sizeof(d) ) ||
        amphora_sc_io_write( fd, &p, sizeof(p) ) ||
        amphora_sc_io_read( fd, &ret, sizeof(ret) ))
        ERR( "amphora sc stash ipc failed\n" );
    close( fd );
}

__attribute__((visibility("default")))
VkResult amphora_wine_vkCreateSwapchainKHR( VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                            const VkAllocationCallbacks *alloc, VkSwapchainKHR *out )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_CREATE, ret;
    uint64_t d, phys = 0, surface, sc = 0;
    uint32_t minImageCount, width, height, format, usage, sharingMode;
    uint32_t preTransform, compositeAlpha, presentMode, clipped, qcount = 0;
    (void)alloc;
    if (!info || !out) return VK_ERROR_INITIALIZATION_FAILED;
    fd = amphora_sc_connect();
    if (fd < 0) return VK_ERROR_INITIALIZATION_FAILED;
    d = (uint64_t)(UINT_PTR)device;
    surface = (uint64_t)(UINT_PTR)info->surface;
    minImageCount = info->minImageCount;
    width = info->imageExtent.width;
    height = info->imageExtent.height;
    format = (uint32_t)info->imageFormat;
    usage = info->imageUsage;
    sharingMode = (uint32_t)info->imageSharingMode;
    preTransform = (uint32_t)info->preTransform;
    compositeAlpha = (uint32_t)info->compositeAlpha;
    presentMode = (uint32_t)info->presentMode;
    clipped = info->clipped ? 1u : 0u;
    qcount = info->queueFamilyIndexCount;
    if (qcount > 8) qcount = 8;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &d, sizeof(d) ) ||
        amphora_sc_io_write( fd, &phys, sizeof(phys) ) ||
        amphora_sc_io_write( fd, &surface, sizeof(surface) ) ||
        amphora_sc_io_write( fd, &minImageCount, sizeof(minImageCount) ) ||
        amphora_sc_io_write( fd, &width, sizeof(width) ) ||
        amphora_sc_io_write( fd, &height, sizeof(height) ) ||
        amphora_sc_io_write( fd, &format, sizeof(format) ) ||
        amphora_sc_io_write( fd, &usage, sizeof(usage) ) ||
        amphora_sc_io_write( fd, &sharingMode, sizeof(sharingMode) ) ||
        amphora_sc_io_write( fd, &preTransform, sizeof(preTransform) ) ||
        amphora_sc_io_write( fd, &compositeAlpha, sizeof(compositeAlpha) ) ||
        amphora_sc_io_write( fd, &presentMode, sizeof(presentMode) ) ||
        amphora_sc_io_write( fd, &clipped, sizeof(clipped) ) ||
        amphora_sc_io_write( fd, &qcount, sizeof(qcount) ) ||
        (qcount && amphora_sc_io_write( fd, info->pQueueFamilyIndices, sizeof(uint32_t) * qcount )) ||
        amphora_sc_io_read( fd, &ret, sizeof(ret) ) ||
        amphora_sc_io_read( fd, &sc, sizeof(sc) ))
    {
        ERR( "amphora sc CreateSwapchain ipc failed\n" );
        close( fd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    close( fd );
    *out = (VkSwapchainKHR)(UINT_PTR)sc;
    ERR( "amphora sc CreateSwapchain ret=%d sc=0x%s\n", ret, wine_dbgstr_longlong( sc ) );
    return (VkResult)ret;
}

__attribute__((visibility("default")))
void amphora_wine_vkDestroySwapchainKHR( VkDevice device, VkSwapchainKHR swapchain,
                                         const VkAllocationCallbacks *alloc )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_DESTROY, ret = 0;
    uint64_t d = (uint64_t)(UINT_PTR)device, sc = (uint64_t)(UINT_PTR)swapchain;
    (void)alloc;
    fd = amphora_sc_connect();
    if (fd < 0) return;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &d, sizeof(d) ) ||
        amphora_sc_io_write( fd, &sc, sizeof(sc) ) ||
        amphora_sc_io_read( fd, &ret, sizeof(ret) ))
        ERR( "amphora sc DestroySwapchain ipc failed\n" );
    close( fd );
}

__attribute__((visibility("default")))
VkResult amphora_wine_vkGetSwapchainImagesKHR( VkDevice device, VkSwapchainKHR swapchain,
                                               uint32_t *count, VkImage *images )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_GET_IMAGES, ret;
    uint64_t d = (uint64_t)(UINT_PTR)device, sc = (uint64_t)(UINT_PTR)swapchain;
    uint32_t n, want, i;
    if (!count) return VK_ERROR_INITIALIZATION_FAILED;
    fd = amphora_sc_connect();
    if (fd < 0) return VK_ERROR_INITIALIZATION_FAILED;
    n = *count;
    want = images ? 1u : 0u;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &d, sizeof(d) ) ||
        amphora_sc_io_write( fd, &sc, sizeof(sc) ) ||
        amphora_sc_io_write( fd, &n, sizeof(n) ) ||
        amphora_sc_io_write( fd, &want, sizeof(want) ) ||
        amphora_sc_io_read( fd, &ret, sizeof(ret) ) ||
        amphora_sc_io_read( fd, &n, sizeof(n) ))
    {
        close( fd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (images)
    {
        for (i = 0; i < n; i++)
        {
            uint64_t img = 0;
            if (amphora_sc_io_read( fd, &img, sizeof(img) )) { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
            if (i < *count) images[i] = (VkImage)(UINT_PTR)img;
        }
    }
    close( fd );
    *count = n;
    return (VkResult)ret;
}

__attribute__((visibility("default")))
VkResult amphora_wine_vkAcquireNextImageKHR( VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                             VkSemaphore semaphore, VkFence fence, uint32_t *index )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_ACQUIRE, ret;
    uint64_t d = (uint64_t)(UINT_PTR)device, sc = (uint64_t)(UINT_PTR)swapchain;
    uint64_t to = timeout, sem = (uint64_t)(UINT_PTR)semaphore, fen = (uint64_t)(UINT_PTR)fence;
    uint32_t idx = 0;
    if (!index) return VK_ERROR_INITIALIZATION_FAILED;
    fd = amphora_sc_connect();
    if (fd < 0) return VK_ERROR_INITIALIZATION_FAILED;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &d, sizeof(d) ) ||
        amphora_sc_io_write( fd, &sc, sizeof(sc) ) ||
        amphora_sc_io_write( fd, &to, sizeof(to) ) ||
        amphora_sc_io_write( fd, &sem, sizeof(sem) ) ||
        amphora_sc_io_write( fd, &fen, sizeof(fen) ) ||
        amphora_sc_io_read( fd, &ret, sizeof(ret) ) ||
        amphora_sc_io_read( fd, &idx, sizeof(idx) ))
    {
        close( fd );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    close( fd );
    *index = idx;
    if (ret == VK_SUCCESS)
        amphora_note_acquire_signal( semaphore, fence );
    return (VkResult)ret;
}

__attribute__((visibility("default")))
VkResult amphora_wine_vkQueuePresentKHR( VkQueue queue, const VkPresentInfoKHR *info )
{
    int fd;
    int32_t op = AMPHORA_SC_OP_PRESENT, ret;
    uint64_t q;
    uint32_t wait_n, sc_n, i;
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    fd = amphora_sc_connect();
    if (fd < 0) return VK_ERROR_INITIALIZATION_FAILED;
    q = (uint64_t)(UINT_PTR)queue;
    wait_n = info->waitSemaphoreCount;
    sc_n = info->swapchainCount;
    if (wait_n > 8) wait_n = 8;
    if (sc_n > 4) sc_n = 4;
    if (amphora_sc_io_write( fd, &op, sizeof(op) ) ||
        amphora_sc_io_write( fd, &q, sizeof(q) ) ||
        amphora_sc_io_write( fd, &wait_n, sizeof(wait_n) ) ||
        amphora_sc_io_write( fd, &sc_n, sizeof(sc_n) ))
    { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
    for (i = 0; i < wait_n; i++)
    {
        uint64_t s = (uint64_t)(UINT_PTR)info->pWaitSemaphores[i];
        if (amphora_sc_io_write( fd, &s, sizeof(s) )) { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
    }
    for (i = 0; i < sc_n; i++)
    {
        uint64_t s = (uint64_t)(UINT_PTR)info->pSwapchains[i];
        uint32_t idx = info->pImageIndices[i];
        if (amphora_sc_io_write( fd, &s, sizeof(s) ) || amphora_sc_io_write( fd, &idx, sizeof(idx) ))
        { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
    }
    if (amphora_sc_io_read( fd, &ret, sizeof(ret) )) { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
    for (i = 0; i < sc_n; i++)
    {
        int32_t rr = 0;
        if (amphora_sc_io_read( fd, &rr, sizeof(rr) )) { close( fd ); return VK_ERROR_INITIALIZATION_FAILED; }
        if (info->pResults) info->pResults[i] = (VkResult)rr;
    }
    close( fd );
    return (VkResult)ret;
}

/**********************************************************************
 *           ANDROID_VulkanInit
 */
UINT ANDROID_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs )
{
    if (version != WINE_VULKAN_DRIVER_VERSION)
    {
        ERR( "version mismatch, win32u wants %u but driver has %u\n", version, WINE_VULKAN_DRIVER_VERSION );
        return STATUS_INVALID_PARAMETER;
    }

    /* win32u normally passes an already-opened handle; fall back for builds
     * without SONAME_LIBVULKAN (common on Android — system libvulkan.so). */
    if (!vulkan_handle)
    {
#ifdef SONAME_LIBVULKAN
        vulkan_handle = dlopen( SONAME_LIBVULKAN, RTLD_NOW );
        if (!vulkan_handle) ERR( "Failed to load %s: %s\n", SONAME_LIBVULKAN, dlerror() );
#else
        vulkan_handle = dlopen( "libvulkan.so", RTLD_NOW );
        if (!vulkan_handle) ERR( "Failed to load libvulkan.so: %s\n", dlerror() );
#endif
    }
    if (!vulkan_handle) return STATUS_NOT_SUPPORTED;

    android_vulkan_handle = vulkan_handle;
    p_vkGetInstanceProcAddr = dlsym( android_vulkan_handle, "vkGetInstanceProcAddr" );
    if (!p_vkGetInstanceProcAddr)
        WARN( "vkGetInstanceProcAddr not found in vulkan library\n" );

    {
        Dl_info info;
        const char *lib = "(unknown)";
        const char *icd = getenv( "VK_ICD_FILENAMES" );
        const char *adreno = getenv( "ADRENOTOOLS_DRIVER_NAME" );
        if (p_vkGetInstanceProcAddr && dladdr( (void *)p_vkGetInstanceProcAddr, &info ) && info.dli_fname)
            lib = info.dli_fname;
        ERR( "ANDROID_VulkanInit ok handle=%p gipa=%p lib=%s icd=%s adrenotools=%s\n",
             android_vulkan_handle, p_vkGetInstanceProcAddr, lib,
             icd ? icd : "(unset)", adreno ? adreno : "(unset)" );
    }
    TRACE( "using vulkan handle %p\n", android_vulkan_handle );
    *driver_funcs = &android_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
