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
static PFN_android_vkGetInstanceProcAddr p_vkGetInstanceProcAddr;

static const struct vulkan_driver_funcs android_vulkan_driver_funcs;

static int amphora_wsi_create_surface( uint64_t vk_instance, int sock, uint64_t *out_surface )
{
    char path[128];
    struct sockaddr_un addr;
    int fd = -1, i;
    int32_t sock32 = sock, reply = -EIO;
    uint64_t surface = 0;

    if (out_surface) *out_surface = 0;
    snprintf( path, sizeof(path), "/data/user/0/app.amphora/files/wineandroid/wsi-%d.sock",
              (int)getpid() );
    ERR( "amphora WSI bridge connect %s inst=0x%s sock=%d\n",
         path, wine_dbgstr_longlong( vk_instance ), sock );

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
        struct ANativeWindow *tmp = create_ioctl_window( hwnd, TRUE, 1.0f );
        int i;
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
                                           sock, &host_surface );
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
