/*
 * Android Vulkan driver: surfaces and swapchains
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

/* The windows of a Wine process live in another process, the Java host, so
 * the platform's own swapchain (a queue of buffers owned by a window) is out
 * of reach. This driver provides VK_KHR_surface and VK_KHR_swapchain itself,
 * on top of whatever the host Vulkan library offers without them:
 *
 *   - swapchain images are AHardwareBuffers allocated here and imported as
 *     VkImages (VK_ANDROID_external_memory_android_hardware_buffer);
 *   - presenting sends the image's index to the host process with a sync file
 *     that signals when rendering is done; the host puts the buffer on the
 *     window's layer (present.c);
 *   - acquiring takes an image the host sent back, and hands the compositor's
 *     release fence to the application's semaphore or fence.
 *
 * win32u finds every host function through vkGetInstanceProcAddr and
 * vkGetDeviceProcAddr; the driver wraps the two and answers for the entry
 * points below, so nothing above it knows the difference.
 *
 * The host is the platform's libvulkan.so, or, when WINE_ANDROID_VULKAN_HAL names
 * one, an Android Vulkan HAL module such as Mesa's Turnip, opened here the
 * way the platform's loader opens the system's driver (see "Host driver").
 */

#if 0
#pragma makedep unix
#endif

#define __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__

#include "config.h"

#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "android.h"

#include "wine/vulkan.h"
#include "wine/vulkan_driver.h"
#include "wine/list.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(vulkan);

/* Wine's vulkan.h is generated without the Android platform. */
#define VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID ((VkStructureType)1000129001)
#define VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID ((VkStructureType)1000129003)
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID 0x00000400

typedef struct VkAndroidHardwareBufferPropertiesANDROID
{
    VkStructureType sType;
    void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeBits;
} VkAndroidHardwareBufferPropertiesANDROID;

typedef struct VkImportAndroidHardwareBufferInfoANDROID
{
    VkStructureType sType;
    const void *pNext;
    AHardwareBuffer *buffer;
} VkImportAndroidHardwareBufferInfoANDROID;

typedef VkResult (VKAPI_PTR *PFN_vkGetAndroidHardwareBufferPropertiesANDROID)( VkDevice, const AHardwareBuffer *,
                                                                               VkAndroidHardwareBufferPropertiesANDROID * );

static PFN_vkGetInstanceProcAddr host_vkGetInstanceProcAddr;
static PFN_vkGetDeviceProcAddr host_vkGetDeviceProcAddr;

/* Instance level functions of the host. The same for every instance: they dispatch on their first argument. */
static PFN_vkEnumerateInstanceExtensionProperties host_vkEnumerateInstanceExtensionProperties;
static PFN_vkCreateInstance host_vkCreateInstance;
static PFN_vkEnumerateDeviceExtensionProperties host_vkEnumerateDeviceExtensionProperties;
static PFN_vkCreateDevice host_vkCreateDevice;

#define HOST_DEVICE_FUNCS \
    USE_FUNC(vkAllocateMemory) \
    USE_FUNC(vkBindImageMemory) \
    USE_FUNC(vkCreateImage) \
    USE_FUNC(vkCreateSemaphore) \
    USE_FUNC(vkDestroyDevice) \
    USE_FUNC(vkDestroyImage) \
    USE_FUNC(vkDestroySemaphore) \
    USE_FUNC(vkFreeMemory) \
    USE_FUNC(vkGetAndroidHardwareBufferPropertiesANDROID) \
    USE_FUNC(vkGetSemaphoreFdKHR) \
    USE_FUNC(vkImportFenceFdKHR) \
    USE_FUNC(vkImportSemaphoreFdKHR) \
    USE_FUNC(vkQueueSubmit)

struct device
{
    struct list     entry;
    VkDevice        handle;
    pthread_mutex_t present_lock;
    VkSemaphore     present_done;   /* signaled by the submit a present makes, exported as a sync file */
#define USE_FUNC(name) PFN_##name p_##name;
    HOST_DEVICE_FUNCS
#undef USE_FUNC
};

static struct list devices = LIST_INIT( devices );
static pthread_mutex_t devices_lock = PTHREAD_MUTEX_INITIALIZER;

enum image_state
{
    IMAGE_FREE,       /* ours; release_fence tells when the compositor is done reading it */
    IMAGE_ACQUIRED,   /* the application's */
    IMAGE_PRESENTED,  /* the host's, until it sends it back */
};

struct image
{
    AHardwareBuffer *buffer;
    VkImage          image;
    VkDeviceMemory   memory;
    enum image_state state;
    int              release_fence;
};

struct swapchain;

/* The host handle of a surface. Lives as long as win32u's client surface and the swapchains made for it. */
struct surface
{
    struct client_surface client;
    ANativeWindow        *window;      /* keeps the client view in the host alive */
    pthread_mutex_t       lock;
    int                   socket;      /* to the presenter in the host, -1 once lost */
    int                   next_id;
    struct list           swapchains;
};

struct swapchain
{
    struct list     entry;             /* in the surface's list, under its lock */
    struct device  *device;
    struct surface *surface;
    int             id;
    BOOL            retired;           /* replaced by a newer swapchain */
    VkExtent2D      extent;
    struct timespec next_present;      /* under a frame rate limit: no present before this */
    uint32_t        count;
    struct image    images[PRESENT_MAX_IMAGES];
};

static struct surface *surface_from_handle( VkSurfaceKHR handle )
{
    return (struct surface *)(UINT_PTR)handle;
}

static struct swapchain *swapchain_from_handle( VkSwapchainKHR handle )
{
    return (struct swapchain *)(UINT_PTR)handle;
}

static struct device *find_device( VkDevice handle )
{
    struct device *device;

    pthread_mutex_lock( &devices_lock );
    LIST_FOR_EACH_ENTRY( device, &devices, struct device, entry )
    {
        if (device->handle != handle) continue;
        pthread_mutex_unlock( &devices_lock );
        return device;
    }
    pthread_mutex_unlock( &devices_lock );
    return NULL;
}

static BOOL has_extension( const VkExtensionProperties *props, uint32_t count, const char *name )
{
    while (count--) if (!strcmp( props[count].extensionName, name )) return TRUE;
    return FALSE;
}

static BOOL has_name( const char *const *names, uint32_t count, const char *name )
{
    while (count--) if (!strcmp( names[count], name )) return TRUE;
    return FALSE;
}


/* Host driver */

/* An Android Vulkan HAL module is a library whose HMI symbol is a struct hwvulkan_module_t; opening its "vk0"
 * device gives the three functions a loader starts from. These are the declarations of the platform's
 * hardware/hardware.h and hardware/hwvulkan.h, which the NDK does not have. */

#define HARDWARE_MODULE_TAG (('H' << 24) | ('W' << 16) | ('M' << 8) | 'T')
#define HARDWARE_DEVICE_TAG (('H' << 24) | ('W' << 16) | ('D' << 8) | 'T')

struct hw_module;

struct hw_device
{
    uint32_t          tag;
    uint32_t          version;
    struct hw_module *module;
    uint64_t          reserved[12];
    int             (*close)( struct hw_device *device );
};

struct hw_module_methods
{
    int (*open)( const struct hw_module *module, const char *id, struct hw_device **device );
};

struct hw_module
{
    uint32_t                  tag;
    uint16_t                  module_api_version;
    uint16_t                  hal_api_version;
    const char               *id;
    const char               *name;
    const char               *author;
    struct hw_module_methods *methods;
    void                     *dso;
    uint64_t                  reserved[32 - 7];
};

struct hwvulkan_device
{
    struct hw_device common;
    PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

static const struct hw_module *hal_module;          /* the driver in use, when it is not the platform's */
static PFN_vkGetDeviceProcAddr hal_vkGetDeviceProcAddr;

static PFN_vkVoidFunction hal_get_device_proc_addr( VkDevice device, const char *name )
{
    /* devices come from instances, and creating the first one found the function */
    return hal_vkGetDeviceProcAddr ? hal_vkGetDeviceProcAddr( device, name ) : NULL;
}

static VkResult no_driver_vkEnumerateInstanceExtensionProperties( const char *layer, uint32_t *count, VkExtensionProperties *props )
{
    return VK_ERROR_INITIALIZATION_FAILED;
}

static VkResult no_driver_vkCreateInstance( const VkInstanceCreateInfo *create_info, const VkAllocationCallbacks *allocator,
                                            VkInstance *instance )
{
    return VK_ERROR_INCOMPATIBLE_DRIVER;
}

static PFN_vkVoidFunction no_driver_get_instance_proc_addr( VkInstance instance, const char *name )
{
    if (!strcmp( name, "vkEnumerateInstanceExtensionProperties" )) return (PFN_vkVoidFunction)no_driver_vkEnumerateInstanceExtensionProperties;
    if (!strcmp( name, "vkCreateInstance" )) return (PFN_vkVoidFunction)no_driver_vkCreateInstance;
    return NULL;
}

/* Opens the HAL module at path. The driver that was asked for is the only one: when it cannot be used there is
 * no Vulkan, rather than the platform's driver passing for it. */
static void load_hal_driver( const char *path, PFN_vkGetInstanceProcAddr *instance_proc_addr, PFN_vkGetDeviceProcAddr *device_proc_addr )
{
    const struct hw_module *module;
    struct hw_device *device;
    void *handle;
    int ret;

    *instance_proc_addr = no_driver_get_instance_proc_addr;
    *device_proc_addr = hal_get_device_proc_addr;

    if (!(handle = dlopen( path, RTLD_NOW | RTLD_LOCAL )))
    {
        ERR( "failed to load %s: %s\n", debugstr_a(path), dlerror() );
        return;
    }
    if (!(module = dlsym( handle, "HMI" )) || module->tag != HARDWARE_MODULE_TAG ||
        !module->id || strcmp( module->id, "vulkan" ) || !module->methods || !module->methods->open)
    {
        ERR( "%s is not a Vulkan HAL module\n", debugstr_a(path) );
        return;
    }
    if ((ret = module->methods->open( module, "vk0", &device )) || !device || device->tag != HARDWARE_DEVICE_TAG)
    {
        ERR( "failed to open the Vulkan device of %s, error %d\n", debugstr_a(path), ret );
        return;
    }
    if (!(*instance_proc_addr = ((struct hwvulkan_device *)device)->GetInstanceProcAddr))
    {
        ERR( "%s has no vkGetInstanceProcAddr\n", debugstr_a(path) );
        *instance_proc_addr = no_driver_get_instance_proc_addr;
        return;
    }
    hal_module = module;
    /* in the system log as well: which driver drew is the first question about any picture */
    p__android_log_print( ANDROID_LOG_INFO, "wine", "vulkan: using %s from %s", module->name ? module->name : "?", path );
    TRACE( "using %s (%s) from %s\n", debugstr_a(module->name), debugstr_a(module->author), debugstr_a(path) );
}


/* Frame rate */

/* How often swapchains present is the user's choice before it is the application's. WINE_ANDROID_FRAME_RATE:
 *   screen      every frame is shown, at the display's pace, whatever present mode the application asks for
 *   unlimited   only the newest frame is shown and nothing waits for the display, whatever it asks for
 *   <number>    as screen, with presents at least 1/number seconds apart
 *   anything else, or not set: the application's present mode decides
 * The compositor only ever shows whole frames, so none of these tears. */
enum frame_rate
{
    FRAME_RATE_APPLICATION,
    FRAME_RATE_SCREEN,
    FRAME_RATE_UNLIMITED,
    FRAME_RATE_LIMITED,
};

static enum frame_rate frame_rate;
static unsigned int frame_rate_limit;   /* frames per second, for FRAME_RATE_LIMITED */

static void init_frame_rate(void)
{
    const char *env = getenv( "WINE_ANDROID_FRAME_RATE" );
    int limit;

    if (!env) return;
    if (!strcmp( env, "screen" )) frame_rate = FRAME_RATE_SCREEN;
    else if (!strcmp( env, "unlimited" )) frame_rate = FRAME_RATE_UNLIMITED;
    else if ((limit = atoi( env )) > 0)
    {
        frame_rate = FRAME_RATE_LIMITED;
        frame_rate_limit = limit;
    }
    TRACE( "frame rate %s: mode %d, limit %u\n", debugstr_a(env), frame_rate, frame_rate_limit );
}

/* Whether a swapchain created with this present mode shows every frame. */
static BOOL shows_every_frame( VkPresentModeKHR mode )
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once( &once, init_frame_rate );
    switch (frame_rate)
    {
    case FRAME_RATE_SCREEN:
    case FRAME_RATE_LIMITED:
        return TRUE;
    case FRAME_RATE_UNLIMITED:
        return FALSE;
    default:
        return mode == VK_PRESENT_MODE_FIFO_KHR || mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    }
}

/* Under a limit, waits until the swapchain may present again. A display mode of that rate would pace the
 * swapchain by itself; few panels have one for every limit, and a translation layer's own limiter (they have
 * the same number) leaves nothing to wait for here. */
static void wait_for_frame_rate( struct swapchain *swapchain )
{
    struct timespec now, *next = &swapchain->next_present;
    long interval;

    if (frame_rate != FRAME_RATE_LIMITED) return;
    interval = 1000000000 / frame_rate_limit;

    clock_gettime( CLOCK_MONOTONIC, &now );
    if (now.tv_sec < next->tv_sec || (now.tv_sec == next->tv_sec && now.tv_nsec < next->tv_nsec))
        while (clock_nanosleep( CLOCK_MONOTONIC, TIMER_ABSTIME, next, NULL ) == EINTR) /* nothing */;
    else *next = now;   /* late: the next frame is measured from this one, not from where it should have been */

    next->tv_nsec += interval;
    if (next->tv_nsec >= 1000000000)
    {
        next->tv_nsec -= 1000000000;
        next->tv_sec++;
    }
}


/* Extensions */

/* Host instance extensions about its own surfaces; ours are VK_KHR_surface and what win32u derives from it.
 * A surface the host made would be taken for one of ours. */
static const char *const hidden_instance_extensions[] =
{
    "VK_KHR_android_surface",
    "VK_KHR_display",
    "VK_EXT_headless_surface",
    "VK_KHR_surface_protected_capabilities",
    "VK_EXT_surface_maintenance1",
    "VK_KHR_surface_maintenance1",
    "VK_GOOGLE_surfaceless_query",
};

/* Ours, whether or not the host has them: the platform's loader does, a driver opened directly does not. */
static const char *const provided_instance_extensions[] =
{
    "VK_KHR_surface",
    "VK_KHR_get_surface_capabilities2",
};

/* Host device extensions that work on its swapchains, which ours are not. */
static const char *const hidden_device_extensions[] =
{
    "VK_EXT_swapchain_maintenance1",
    "VK_KHR_swapchain_maintenance1",
    "VK_KHR_present_id",
    "VK_KHR_present_id2",
    "VK_KHR_present_wait",
    "VK_KHR_present_wait2",
    "VK_KHR_incremental_present",
    "VK_KHR_shared_presentable_image",
    "VK_KHR_display_swapchain",
    "VK_EXT_present_mode_fifo_latest_ready",
    "VK_KHR_present_mode_fifo_latest_ready",
    "VK_EXT_present_timing",
    "VK_EXT_hdr_metadata",
    "VK_EXT_display_control",
    "VK_GOOGLE_display_timing",
    "VK_NV_low_latency2",
    "VK_ANDROID_native_buffer",
};

/* Ours, whether or not the host has them. */
static const char *const provided_device_extensions[] =
{
    "VK_KHR_swapchain",
    "VK_KHR_swapchain_mutable_format",
};

/* What the swapchain needs of the host device. */
static const char *const required_device_extensions[] =
{
    "VK_ANDROID_external_memory_android_hardware_buffer",
    "VK_KHR_external_semaphore_fd",
    "VK_KHR_external_fence_fd",
};

/* What those depend on. Part of Vulkan 1.1, but an application may ask for 1.0. */
static const char *const dependency_device_extensions[] =
{
    "VK_KHR_maintenance1",
    "VK_KHR_bind_memory2",
    "VK_KHR_get_memory_requirements2",
    "VK_KHR_dedicated_allocation",
    "VK_KHR_sampler_ycbcr_conversion",
    "VK_KHR_external_memory",
    "VK_KHR_external_semaphore",
    "VK_KHR_external_fence",
    "VK_EXT_queue_family_foreign",
    "VK_KHR_image_format_list",
};

static const char *const dependency_instance_extensions[] =
{
    "VK_KHR_get_physical_device_properties2",
    "VK_KHR_external_memory_capabilities",
    "VK_KHR_external_semaphore_capabilities",
    "VK_KHR_external_fence_capabilities",
};

/* Filters a host extension list: drops the hidden ones, adds the provided ones. Vulkan's two-call protocol. */
static VkResult filter_extensions( VkExtensionProperties *host, uint32_t host_count,
                                   const char *const *hidden, uint32_t hidden_count,
                                   const char *const *provided, uint32_t provided_count,
                                   uint32_t *count, VkExtensionProperties *props )
{
    uint32_t i, total = 0, capacity = props ? *count : 0;

    for (i = 0; i < host_count; i++)
    {
        if (has_name( hidden, hidden_count, host[i].extensionName )) continue;
        if (has_name( provided, provided_count, host[i].extensionName )) continue;
        if (total < capacity) props[total] = host[i];
        total++;
    }
    for (i = 0; i < provided_count; i++)
    {
        if (total < capacity)
        {
            memset( &props[total], 0, sizeof(props[total]) );
            strcpy( props[total].extensionName, provided[i] );
            props[total].specVersion = 1;
        }
        total++;
    }

    if (!props)
    {
        *count = total;
        return VK_SUCCESS;
    }
    *count = min( total, capacity );
    return total > capacity ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult get_host_instance_extensions( VkExtensionProperties **props, uint32_t *count )
{
    VkResult res;

    *props = NULL;
    do
    {
        free( *props );
        *props = NULL;
        if ((res = host_vkEnumerateInstanceExtensionProperties( NULL, count, NULL ))) return res;
        if (!(*props = calloc( *count + 1, sizeof(**props) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    } while ((res = host_vkEnumerateInstanceExtensionProperties( NULL, count, *props )) == VK_INCOMPLETE);
    if (res) free( *props );
    return res;
}

static VkResult get_host_device_extensions( VkPhysicalDevice physical_device, VkExtensionProperties **props, uint32_t *count )
{
    VkResult res;

    *props = NULL;
    do
    {
        free( *props );
        *props = NULL;
        if ((res = host_vkEnumerateDeviceExtensionProperties( physical_device, NULL, count, NULL ))) return res;
        if (!(*props = calloc( *count + 1, sizeof(**props) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    } while ((res = host_vkEnumerateDeviceExtensionProperties( physical_device, NULL, count, *props )) == VK_INCOMPLETE);
    if (res) free( *props );
    return res;
}

static VkResult android_vkEnumerateInstanceExtensionProperties( const char *layer, uint32_t *count, VkExtensionProperties *props )
{
    VkExtensionProperties *host;
    uint32_t host_count;
    VkResult res;

    if (layer) return host_vkEnumerateInstanceExtensionProperties( layer, count, props );
    if ((res = get_host_instance_extensions( &host, &host_count ))) return res;
    res = filter_extensions( host, host_count, hidden_instance_extensions, ARRAY_SIZE(hidden_instance_extensions),
                             provided_instance_extensions, ARRAY_SIZE(provided_instance_extensions), count, props );
    free( host );
    return res;
}

static VkResult android_vkEnumerateDeviceExtensionProperties( VkPhysicalDevice physical_device, const char *layer,
                                                              uint32_t *count, VkExtensionProperties *props )
{
    VkExtensionProperties *host;
    uint32_t host_count, i;
    VkResult res;

    if (layer) return host_vkEnumerateDeviceExtensionProperties( physical_device, layer, count, props );
    if ((res = get_host_device_extensions( physical_device, &host, &host_count ))) return res;

    /* no swapchain from us on a device that cannot share buffers with the compositor */
    for (i = 0; i < ARRAY_SIZE(required_device_extensions); i++)
        if (!has_extension( host, host_count, required_device_extensions[i] )) break;
    if (i < ARRAY_SIZE(required_device_extensions))
    {
        /* its own VK_KHR_swapchain, if any, is hidden with the rest: nothing here could present with it */
        WARN( "host device lacks %s, no swapchains\n", required_device_extensions[i] );
        res = filter_extensions( host, host_count, provided_device_extensions, ARRAY_SIZE(provided_device_extensions),
                                 NULL, 0, count, props );
    }
    else res = filter_extensions( host, host_count, hidden_device_extensions, ARRAY_SIZE(hidden_device_extensions),
                                  provided_device_extensions, ARRAY_SIZE(provided_device_extensions), count, props );
    free( host );
    return res;
}

static VkResult android_vkCreateInstance( const VkInstanceCreateInfo *create_info, const VkAllocationCallbacks *allocator,
                                          VkInstance *instance )
{
    VkInstanceCreateInfo info = *create_info;
    VkExtensionProperties *host;
    uint32_t host_count, i, count = 0;
    const char **names;
    VkResult res;

    if ((res = get_host_instance_extensions( &host, &host_count ))) return res;
    if (!(names = malloc( (info.enabledExtensionCount + ARRAY_SIZE(dependency_instance_extensions)) * sizeof(*names) )))
    {
        free( host );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    for (i = 0; i < info.enabledExtensionCount; i++)
    {
        const char *name = info.ppEnabledExtensionNames[i];

        /* a host that has them keeps them on: the types and results of VK_KHR_surface are in use */
        if (has_name( provided_instance_extensions, ARRAY_SIZE(provided_instance_extensions), name ) &&
            !has_extension( host, host_count, name )) continue;
        names[count++] = name;
    }
    for (i = 0; i < ARRAY_SIZE(dependency_instance_extensions); i++)
    {
        const char *name = dependency_instance_extensions[i];
        if (has_extension( host, host_count, name ) && !has_name( names, count, name )) names[count++] = name;
    }
    info.enabledExtensionCount = count;
    info.ppEnabledExtensionNames = names;

    res = host_vkCreateInstance( &info, allocator, instance );
    free( names );
    free( host );
    /* a HAL module has no vkGetDeviceProcAddr to export: it is an instance function like any other */
    if (!res && hal_module && !hal_vkGetDeviceProcAddr)
        hal_vkGetDeviceProcAddr = (void *)host_vkGetInstanceProcAddr( *instance, "vkGetDeviceProcAddr" );
    return res;
}


/* Devices */

static VkResult android_vkCreateDevice( VkPhysicalDevice physical_device, const VkDeviceCreateInfo *create_info,
                                        const VkAllocationCallbacks *allocator, VkDevice *ret )
{
    VkExportSemaphoreCreateInfo export_info = {.sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
    VkSemaphoreCreateInfo semaphore_info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &export_info};
    VkDeviceCreateInfo info = *create_info;
    VkExtensionProperties *host;
    uint32_t host_count, i, count = 0;
    BOOL swapchain = FALSE;
    struct device *device;
    const char **names;
    VkResult res;

    if ((res = get_host_device_extensions( physical_device, &host, &host_count ))) return res;
    if (!(names = malloc( (info.enabledExtensionCount + ARRAY_SIZE(required_device_extensions) +
                           ARRAY_SIZE(dependency_device_extensions)) * sizeof(*names) )))
    {
        free( host );
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }

    for (i = 0; i < info.enabledExtensionCount; i++)
    {
        const char *name = info.ppEnabledExtensionNames[i];

        if (has_name( provided_device_extensions, ARRAY_SIZE(provided_device_extensions), name ))
        {
            swapchain = TRUE;
            /* a host that has them keeps them on: the layout of a presented image belongs to VK_KHR_swapchain */
            if (!has_extension( host, host_count, name )) continue;
        }
        names[count++] = name;
    }
    if (swapchain)
    {
        for (i = 0; i < ARRAY_SIZE(required_device_extensions); i++)
        {
            const char *name = required_device_extensions[i];
            if (!has_name( names, count, name )) names[count++] = name;
        }
        for (i = 0; i < ARRAY_SIZE(dependency_device_extensions); i++)
        {
            const char *name = dependency_device_extensions[i];
            if (has_extension( host, host_count, name ) && !has_name( names, count, name )) names[count++] = name;
        }
    }
    info.enabledExtensionCount = count;
    info.ppEnabledExtensionNames = names;

    res = host_vkCreateDevice( physical_device, &info, allocator, ret );
    free( names );
    free( host );
    if (res || !swapchain) return res;

    if (!(device = calloc( 1, sizeof(*device) ))) goto failed;
    device->handle = *ret;
#define USE_FUNC(name) \
    if (!(device->p_##name = (void *)host_vkGetDeviceProcAddr( device->handle, #name ))) \
    { \
        ERR( "host device has no %s\n", #name ); \
        goto failed; \
    }
    HOST_DEVICE_FUNCS
#undef USE_FUNC

    export_info.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    if ((res = device->p_vkCreateSemaphore( device->handle, &semaphore_info, NULL, &device->present_done )))
    {
        ERR( "no exportable semaphore, res %d\n", res );
        goto failed;
    }
    pthread_mutex_init( &device->present_lock, NULL );

    pthread_mutex_lock( &devices_lock );
    list_add_tail( &devices, &device->entry );
    pthread_mutex_unlock( &devices_lock );
    TRACE( "device %p with swapchain support\n", device->handle );
    return VK_SUCCESS;

failed:
    /* the device works, only presenting will not: creating a swapchain fails */
    free( device );
    return VK_SUCCESS;
}

static void android_vkDestroyDevice( VkDevice handle, const VkAllocationCallbacks *allocator )
{
    PFN_vkDestroyDevice destroy = (void *)host_vkGetDeviceProcAddr( handle, "vkDestroyDevice" );
    struct device *device;

    if ((device = find_device( handle )))
    {
        pthread_mutex_lock( &devices_lock );
        list_remove( &device->entry );
        pthread_mutex_unlock( &devices_lock );
        device->p_vkDestroySemaphore( handle, device->present_done, NULL );
        pthread_mutex_destroy( &device->present_lock );
        free( device );
    }
    destroy( handle, allocator );
}


/* Surfaces */

static const VkSurfaceFormatKHR surface_formats[] =
{
    /* AHardwareBuffers have no BGRA format */
    {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
};

static const VkPresentModeKHR present_modes[] =
{
    VK_PRESENT_MODE_FIFO_KHR,
    VK_PRESENT_MODE_MAILBOX_KHR,
    VK_PRESENT_MODE_IMMEDIATE_KHR,
};

static uint32_t buffer_format( VkFormat format )
{
    switch (format)
    {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB: return AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return AHARDWAREBUFFER_FORMAT_R10G10B10A2_UNORM;
    default: return 0;
    }
}

static void surface_destroy( struct client_surface *client )
{
    struct surface *surface = CONTAINING_RECORD( client, struct surface, client );

    TRACE( "%s\n", debugstr_client_surface( client ) );
    if (surface->socket >= 0) close( surface->socket );
    if (surface->window) release_ioctl_window( surface->window );
    pthread_mutex_destroy( &surface->lock );
}

static void surface_detach( struct client_surface *client )
{
}

static void surface_update( struct client_surface *client )
{
    /* the host lays the client view out from the window's position */
}

static void surface_present( struct client_surface *client, HDC hdc )
{
}

static const struct client_surface_funcs surface_funcs =
{
    .destroy = surface_destroy,
    .detach = surface_detach,
    .update = surface_update,
    .present = surface_present,
};

static VkResult android_vulkan_surface_create( HWND hwnd, BOOL raw, const struct vulkan_instance *instance,
                                               VkSurfaceKHR *handle, struct client_surface **client )
{
    struct surface *surface;

    TRACE( "hwnd %p, raw %u, instance %p\n", hwnd, raw, instance );

    if (!(surface = client_surface_create( sizeof(*surface), &surface_funcs, hwnd ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    pthread_mutex_init( &surface->lock, NULL );
    list_init( &surface->swapchains );
    surface->socket = -1;

    /* the window gets a client view in the host; its surface is where the presenter puts our images */
    if ((surface->window = get_client_window( hwnd ))) surface->socket = ioctl_present_connect( hwnd );
    /* without one (a window of another process, a window that is gone) the surface exists and is lost */
    if (surface->socket < 0) WARN( "no presenter for window %p\n", hwnd );

    *handle = (UINT_PTR)surface;
    *client = &surface->client;
    TRACE( "created surface %p for window %p, socket %d\n", surface, hwnd, surface->socket );
    return VK_SUCCESS;
}

static void android_vkDestroySurfaceKHR( VkInstance instance, VkSurfaceKHR handle, const VkAllocationCallbacks *allocator )
{
    struct surface *surface = surface_from_handle( handle );

    if (!surface) return;
    /* the memory is win32u's client surface; tell the host now, the layer goes with the socket */
    pthread_mutex_lock( &surface->lock );
    if (surface->socket >= 0) shutdown( surface->socket, SHUT_RDWR );
    pthread_mutex_unlock( &surface->lock );
}

static VkResult android_vkGetPhysicalDeviceSurfaceSupportKHR( VkPhysicalDevice physical_device, uint32_t queue_family,
                                                              VkSurfaceKHR surface, VkBool32 *supported )
{
    /* presenting is a submit that signals a semaphore: any queue can */
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

static VkBool32 android_get_physical_device_presentation_support( struct vulkan_physical_device *physical_device, uint32_t queue )
{
    return VK_TRUE;
}

static VkResult android_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                                   VkSurfaceCapabilitiesKHR *caps )
{
    memset( caps, 0, sizeof(*caps) );
    /* one on screen, one waiting for the next refresh, one being drawn */
    caps->minImageCount = 3;
    caps->maxImageCount = PRESENT_MAX_IMAGES;
    /* win32u replaces the extents with the window's client area */
    caps->currentExtent.width = caps->currentExtent.height = 0xffffffff;
    caps->minImageExtent.width = caps->minImageExtent.height = 1;
    caps->maxImageExtent.width = caps->maxImageExtent.height = 16384;
    caps->maxImageArrayLayers = 1;
    /* the host rotates and scales the layer, the program never sees it */
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    /* a window's alpha means nothing on Windows; INHERIT is what programs that do not care pass */
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

static VkResult android_vkGetPhysicalDeviceSurfaceCapabilities2KHR( VkPhysicalDevice physical_device,
                                                                    const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                                    VkSurfaceCapabilities2KHR *caps )
{
    /* nothing chained to either is ours to answer: the extensions that define such structures are hidden */
    return android_vkGetPhysicalDeviceSurfaceCapabilitiesKHR( physical_device, info->surface, &caps->surfaceCapabilities );
}

static VkResult android_vkGetPhysicalDeviceSurfaceFormatsKHR( VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                              uint32_t *count, VkSurfaceFormatKHR *formats )
{
    uint32_t capacity = *count;

    if (!formats)
    {
        *count = ARRAY_SIZE(surface_formats);
        return VK_SUCCESS;
    }
    *count = min( capacity, ARRAY_SIZE(surface_formats) );
    memcpy( formats, surface_formats, *count * sizeof(*formats) );
    return *count < ARRAY_SIZE(surface_formats) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult android_vkGetPhysicalDeviceSurfaceFormats2KHR( VkPhysicalDevice physical_device,
                                                               const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                               uint32_t *count, VkSurfaceFormat2KHR *formats )
{
    uint32_t i, capacity = *count;

    if (!formats)
    {
        *count = ARRAY_SIZE(surface_formats);
        return VK_SUCCESS;
    }
    *count = min( capacity, ARRAY_SIZE(surface_formats) );
    for (i = 0; i < *count; i++) formats[i].surfaceFormat = surface_formats[i];
    return *count < ARRAY_SIZE(surface_formats) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult android_vkGetPhysicalDeviceSurfacePresentModesKHR( VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                                   uint32_t *count, VkPresentModeKHR *modes )
{
    uint32_t capacity = *count;

    if (!modes)
    {
        *count = ARRAY_SIZE(present_modes);
        return VK_SUCCESS;
    }
    *count = min( capacity, ARRAY_SIZE(present_modes) );
    memcpy( modes, present_modes, *count * sizeof(*modes) );
    return *count < ARRAY_SIZE(present_modes) ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult android_vkGetPhysicalDevicePresentRectanglesKHR( VkPhysicalDevice physical_device, VkSurfaceKHR surface,
                                                                 uint32_t *count, VkRect2D *rects )
{
    if (!rects)
    {
        *count = 1;
        return VK_SUCCESS;
    }
    if (!*count) return VK_INCOMPLETE;
    *count = 1;
    memset( rects, 0, sizeof(*rects) );
    rects->extent.width = rects->extent.height = 16384;
    return VK_SUCCESS;
}

static VkResult android_vkGetDeviceGroupPresentCapabilitiesKHR( VkDevice device, VkDeviceGroupPresentCapabilitiesKHR *caps )
{
    memset( caps->presentMask, 0, sizeof(caps->presentMask) );
    caps->presentMask[0] = 1;
    caps->modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}

static VkResult android_vkGetDeviceGroupSurfacePresentModesKHR( VkDevice device, VkSurfaceKHR surface,
                                                                VkDeviceGroupPresentModeFlagsKHR *modes )
{
    *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
    return VK_SUCCESS;
}


/* Swapchains */

/* Takes the messages the host sent. Called with the surface locked. Returns FALSE once the host is gone. */
static BOOL read_releases( struct surface *surface )
{
    struct present_message msg;
    struct swapchain *swapchain;
    int fd, ret;

    if (surface->socket < 0) return FALSE;
    while ((ret = present_recv( surface->socket, &msg, &fd, FALSE )) == 1)
    {
        if (msg.type == PRESENT_RELEASE)
        {
            LIST_FOR_EACH_ENTRY( swapchain, &surface->swapchains, struct swapchain, entry )
            {
                struct image *image;

                if (swapchain->id != msg.swapchain || msg.image < 0 || msg.image >= swapchain->count) continue;
                image = &swapchain->images[msg.image];
                if (image->state != IMAGE_PRESENTED) break;
                image->state = IMAGE_FREE;
                image->release_fence = fd;
                fd = -1;
                break;
            }
        }
        /* an image of a swapchain that is gone, or something we do not know */
        if (fd >= 0) close( fd );
    }
    if (ret < 0)
    {
        WARN( "surface %p lost its presenter\n", surface );
        close( surface->socket );
        surface->socket = -1;
        return FALSE;
    }
    return TRUE;
}

static void destroy_images( struct swapchain *swapchain )
{
    struct device *device = swapchain->device;
    uint32_t i;

    for (i = 0; i < swapchain->count; i++)
    {
        struct image *image = &swapchain->images[i];

        if (image->image) device->p_vkDestroyImage( device->handle, image->image, NULL );
        if (image->memory) device->p_vkFreeMemory( device->handle, image->memory, NULL );
        if (image->buffer) pAHardwareBuffer_release( image->buffer );
        if (image->release_fence >= 0) close( image->release_fence );
    }
}

static VkResult create_image( struct swapchain *swapchain, struct image *image, const VkSwapchainCreateInfoKHR *info )
{
    VkExternalMemoryImageCreateInfo external_info =
    {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID,
    };
    VkImageFormatListCreateInfo format_list = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    VkImageCreateInfo image_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_info,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = info->imageFormat,
        .extent = {info->imageExtent.width, info->imageExtent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = info->imageUsage,
        .sharingMode = info->imageSharingMode,
        .queueFamilyIndexCount = info->queueFamilyIndexCount,
        .pQueueFamilyIndices = info->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkAndroidHardwareBufferPropertiesANDROID props = {.sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkMemoryDedicatedAllocateInfo dedicated_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    VkImportAndroidHardwareBufferInfoANDROID import_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
        .pNext = &dedicated_info,
    };
    VkMemoryAllocateInfo alloc_info = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &import_info};
    AHardwareBuffer_Desc desc =
    {
        .width = info->imageExtent.width,
        .height = info->imageExtent.height,
        .layers = 1,
        .format = buffer_format( info->imageFormat ),
        /* drawn by the GPU, read by the compositor, or scanned out directly */
        .usage = AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER | AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                 AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY,
    };
    struct device *device = swapchain->device;
    const VkBaseInStructure *chained;
    VkResult res;
    uint32_t i;
    int ret;

    image->release_fence = -1;
    image->state = IMAGE_FREE;

    if ((ret = pAHardwareBuffer_allocate( &desc, &image->buffer )))
    {
        ERR( "AHardwareBuffer_allocate %ux%u format %#x failed: %d\n", desc.width, desc.height, desc.format, ret );
        image->buffer = NULL;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }

    if (info->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR)
    {
        /* views in the formats the application listed, the sRGB and linear forms of the same bytes */
        image_info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        for (chained = info->pNext; chained; chained = chained->pNext)
            if (chained->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) break;
        if (chained)
        {
            const VkImageFormatListCreateInfo *formats = (const VkImageFormatListCreateInfo *)chained;
            format_list.viewFormatCount = formats->viewFormatCount;
            format_list.pViewFormats = formats->pViewFormats;
            external_info.pNext = &format_list;
        }
    }

    if ((res = device->p_vkCreateImage( device->handle, &image_info, NULL, &image->image )))
    {
        ERR( "vkCreateImage failed, res %d\n", res );
        return res;
    }
    if ((res = device->p_vkGetAndroidHardwareBufferPropertiesANDROID( device->handle, image->buffer, &props )))
    {
        ERR( "vkGetAndroidHardwareBufferPropertiesANDROID failed, res %d\n", res );
        return res;
    }
    for (i = 0; i < 32; i++) if (props.memoryTypeBits & (1u << i)) break;
    dedicated_info.image = image->image;
    import_info.buffer = image->buffer;
    alloc_info.allocationSize = props.allocationSize;
    alloc_info.memoryTypeIndex = i;
    if ((res = device->p_vkAllocateMemory( device->handle, &alloc_info, NULL, &image->memory )))
    {
        ERR( "importing the buffer failed, res %d\n", res );
        return res;
    }
    if ((res = device->p_vkBindImageMemory( device->handle, image->image, image->memory, 0 )))
        ERR( "vkBindImageMemory failed, res %d\n", res );
    return res;
}

static VkResult android_vkCreateSwapchainKHR( VkDevice handle, const VkSwapchainCreateInfoKHR *info,
                                              const VkAllocationCallbacks *allocator, VkSwapchainKHR *ret )
{
    struct swapchain *swapchain, *old = swapchain_from_handle( info->oldSwapchain );
    struct surface *surface = surface_from_handle( info->surface );
    struct device *device = find_device( handle );
    struct present_message msg = {.type = PRESENT_CREATE};
    VkResult res = VK_SUCCESS;
    uint32_t i;
    BOOL fifo;

    TRACE( "device %p, surface %p, %ux%u, format %d, min images %u, mode %d, usage %#x, flags %#x, old %p\n", handle, surface,
           info->imageExtent.width, info->imageExtent.height, info->imageFormat, info->minImageCount, info->presentMode,
           info->imageUsage, info->flags, old );

    if (!device)
    {
        ERR( "device %p cannot present\n", handle );
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!buffer_format( info->imageFormat ) || info->imageArrayLayers != 1)
    {
        FIXME( "format %d with %u layers is not supported\n", info->imageFormat, info->imageArrayLayers );
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }

    if (!(swapchain = calloc( 1, sizeof(*swapchain) ))) return VK_ERROR_OUT_OF_HOST_MEMORY;
    swapchain->device = device;
    swapchain->surface = surface;
    swapchain->extent = info->imageExtent;
    /* Showing only the newest frame takes one image more than the surface's minimum: one on screen
     * until the next takes its place, one with the compositor, one waiting in the host, and one for
     * the application to draw, or it waits for the compositor after all. */
    fifo = shows_every_frame( info->presentMode );
    swapchain->count = min( max( info->minImageCount, fifo ? 3 : 4 ), PRESENT_MAX_IMAGES );
    for (i = 0; i < swapchain->count && !res; i++) res = create_image( swapchain, &swapchain->images[i], info );
    if (res) goto failed;

    pthread_mutex_lock( &surface->lock );
    if (old) old->retired = TRUE;
    if (!read_releases( surface )) res = VK_ERROR_SURFACE_LOST_KHR;
    else
    {
        msg.swapchain = swapchain->id = ++surface->next_id;
        msg.count = swapchain->count;
        msg.width = swapchain->extent.width;
        msg.height = swapchain->extent.height;
        /* PRE_MULTIPLIED is the only alpha that would reach the screen, and the surface does not offer it */
        msg.flags = PRESENT_OPAQUE;
        if (fifo) msg.flags |= PRESENT_FIFO;
        if (present_send( surface->socket, &msg, -1 )) res = VK_ERROR_SURFACE_LOST_KHR;
        for (i = 0; i < swapchain->count && !res; i++)
            if (pAHardwareBuffer_sendHandleToUnixSocket( swapchain->images[i].buffer, surface->socket ))
                res = VK_ERROR_SURFACE_LOST_KHR;
        if (!res) list_add_tail( &surface->swapchains, &swapchain->entry );
    }
    pthread_mutex_unlock( &surface->lock );
    if (res) goto failed;

    client_surface_add_ref( &surface->client );
    *ret = (UINT_PTR)swapchain;
    TRACE( "created swapchain %p, id %d, %u images\n", swapchain, swapchain->id, swapchain->count );
    return VK_SUCCESS;

failed:
    WARN( "failed, res %d\n", res );
    destroy_images( swapchain );
    free( swapchain );
    return res;
}

static void android_vkDestroySwapchainKHR( VkDevice handle, VkSwapchainKHR swapchain_handle, const VkAllocationCallbacks *allocator )
{
    struct swapchain *swapchain = swapchain_from_handle( swapchain_handle );
    struct present_message msg = {.type = PRESENT_DESTROY};
    struct surface *surface;

    if (!swapchain) return;
    surface = swapchain->surface;

    TRACE( "swapchain %p, id %d\n", swapchain, swapchain->id );

    pthread_mutex_lock( &surface->lock );
    list_remove( &swapchain->entry );
    msg.swapchain = swapchain->id;
    if (surface->socket >= 0) present_send( surface->socket, &msg, -1 );
    pthread_mutex_unlock( &surface->lock );

    /* the compositor has its own reference to a buffer it still shows */
    destroy_images( swapchain );
    free( swapchain );
    client_surface_release( &surface->client );
}

static VkResult android_vkGetSwapchainImagesKHR( VkDevice handle, VkSwapchainKHR swapchain_handle, uint32_t *count, VkImage *images )
{
    struct swapchain *swapchain = swapchain_from_handle( swapchain_handle );
    uint32_t i, capacity = *count;

    if (!images)
    {
        *count = swapchain->count;
        return VK_SUCCESS;
    }
    *count = min( capacity, swapchain->count );
    for (i = 0; i < *count; i++) images[i] = swapchain->images[i].image;
    return *count < swapchain->count ? VK_INCOMPLETE : VK_SUCCESS;
}

/* Gives a release fence to what the application waits on. Takes the fence. */
static VkResult signal_acquire( struct device *device, int fence, VkSemaphore semaphore_handle, VkFence fence_handle )
{
    VkImportSemaphoreFdInfoKHR semaphore_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
        .semaphore = semaphore_handle,
        .flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkImportFenceFdInfoKHR fence_info =
    {
        .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR,
        .fence = fence_handle,
        .flags = VK_FENCE_IMPORT_TEMPORARY_BIT,
        .handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT,
    };
    VkResult res = VK_SUCCESS;

    /* a successful import owns the descriptor; -1 imports as already signaled */
    if (semaphore_handle)
    {
        semaphore_info.fd = (fence >= 0 && fence_handle) ? dup( fence ) : fence;
        if ((res = device->p_vkImportSemaphoreFdKHR( device->handle, &semaphore_info )))
        {
            ERR( "vkImportSemaphoreFdKHR failed, res %d\n", res );
            if (semaphore_info.fd >= 0 && semaphore_info.fd != fence) close( semaphore_info.fd );
        }
        else if (!fence_handle) fence = -1;
    }
    if (fence_handle && !res)
    {
        fence_info.fd = fence;
        if ((res = device->p_vkImportFenceFdKHR( device->handle, &fence_info ))) ERR( "vkImportFenceFdKHR failed, res %d\n", res );
        else fence = -1;
    }
    if (fence >= 0) close( fence );
    return res;
}

static VkResult acquire_image( struct device *device, struct swapchain *swapchain, uint64_t timeout,
                               VkSemaphore semaphore, VkFence fence, uint32_t *index )
{
    struct surface *surface = swapchain->surface;
    struct timespec start, now;
    uint32_t i;

    clock_gettime( CLOCK_MONOTONIC, &start );
    for (;;)
    {
        struct pollfd pfd = {.events = POLLIN};
        VkResult res = VK_SUCCESS;
        int wait = -1;

        pthread_mutex_lock( &surface->lock );
        if (swapchain->retired) res = VK_ERROR_OUT_OF_DATE_KHR;
        else if (!read_releases( surface )) res = VK_ERROR_SURFACE_LOST_KHR;
        else
        {
            for (i = 0; i < swapchain->count; i++) if (swapchain->images[i].state == IMAGE_FREE) break;
            if (i < swapchain->count)
            {
                struct image *image = &swapchain->images[i];
                int release_fence = image->release_fence;

                image->release_fence = -1;
                image->state = IMAGE_ACQUIRED;
                pthread_mutex_unlock( &surface->lock );
                *index = i;
                return signal_acquire( device, release_fence, semaphore, fence );
            }
            /* our own descriptor: the surface's may be closed, and its number reused, while we wait */
            if ((pfd.fd = dup( surface->socket )) < 0) res = VK_ERROR_SURFACE_LOST_KHR;
        }
        pthread_mutex_unlock( &surface->lock );
        if (res) return res;

        /* every image is with the application or the host: wait for the host to send one back */
        if (!timeout) res = VK_NOT_READY;
        else if (timeout != UINT64_MAX)
        {
            uint64_t elapsed;

            clock_gettime( CLOCK_MONOTONIC, &now );
            elapsed = (now.tv_sec - start.tv_sec) * (uint64_t)1000000000 + now.tv_nsec - start.tv_nsec;
            if (elapsed >= timeout) res = VK_TIMEOUT;
            else wait = min( (timeout - elapsed + 999999) / 1000000, (uint64_t)INT_MAX );
        }
        if (!res && poll( &pfd, 1, wait ) < 0 && errno != EINTR) res = VK_ERROR_SURFACE_LOST_KHR;
        close( pfd.fd );
        if (res) return res;
    }
}

static VkResult android_vkAcquireNextImageKHR( VkDevice handle, VkSwapchainKHR swapchain_handle, uint64_t timeout,
                                               VkSemaphore semaphore, VkFence fence, uint32_t *index )
{
    struct swapchain *swapchain = swapchain_from_handle( swapchain_handle );
    return acquire_image( swapchain->device, swapchain, timeout, semaphore, fence, index );
}

static VkResult android_vkAcquireNextImage2KHR( VkDevice handle, const VkAcquireNextImageInfoKHR *info, uint32_t *index )
{
    struct swapchain *swapchain = swapchain_from_handle( info->swapchain );
    return acquire_image( swapchain->device, swapchain, info->timeout, info->semaphore, info->fence, index );
}

/* Hands an acquired image to the host. The fence stays the caller's. */
static VkResult present_image( struct swapchain *swapchain, uint32_t index, int fence )
{
    struct present_message msg = {.type = PRESENT_FRAME, .swapchain = swapchain->id, .image = index};
    struct surface *surface = swapchain->surface;
    VkResult res = VK_SUCCESS;

    pthread_mutex_lock( &surface->lock );
    if (index >= swapchain->count || swapchain->images[index].state != IMAGE_ACQUIRED)
    {
        ERR( "swapchain %p image %u was not acquired\n", swapchain, index );
        res = VK_ERROR_OUT_OF_DATE_KHR;
    }
    else if (swapchain->retired)
    {
        /* its successor owns the window now */
        swapchain->images[index].state = IMAGE_FREE;
        res = VK_ERROR_OUT_OF_DATE_KHR;
    }
    else if (!read_releases( surface ) || present_send( surface->socket, &msg, fence ))
    {
        swapchain->images[index].state = IMAGE_FREE;
        res = VK_ERROR_SURFACE_LOST_KHR;
    }
    else swapchain->images[index].state = IMAGE_PRESENTED;
    pthread_mutex_unlock( &surface->lock );
    return res;
}

static VkResult android_vkQueuePresentKHR( VkQueue queue, const VkPresentInfoKHR *info )
{
    VkPipelineStageFlags stack_stages[8], *stages = stack_stages;
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO};
    VkSemaphoreGetFdInfoKHR get_fd = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    VkResult res, ret = VK_SUCCESS;
    struct device *device;
    int fence = -1;
    uint32_t i;

    if (!info->swapchainCount) return VK_SUCCESS;
    device = swapchain_from_handle( info->pSwapchains[0] )->device;

    if (info->waitSemaphoreCount > ARRAY_SIZE(stack_stages) &&
        !(stages = malloc( info->waitSemaphoreCount * sizeof(*stages) )))
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    for (i = 0; i < info->waitSemaphoreCount; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    /* The compositor must not read the image before the application is done drawing it. A submit that
     * waits for what the application waits for, on its queue, and signals a semaphore gives that moment
     * as a sync file. */
    submit.waitSemaphoreCount = info->waitSemaphoreCount;
    submit.pWaitSemaphores = info->pWaitSemaphores;
    submit.pWaitDstStageMask = stages;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &device->present_done;
    get_fd.semaphore = device->present_done;
    get_fd.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;

    pthread_mutex_lock( &device->present_lock );
    if (!(res = device->p_vkQueueSubmit( queue, 1, &submit, VK_NULL_HANDLE )))
        res = device->p_vkGetSemaphoreFdKHR( device->handle, &get_fd, &fence );
    pthread_mutex_unlock( &device->present_lock );
    if (stages != stack_stages) free( stages );

    if (res)
    {
        ERR( "no fence for the frame, res %d\n", res );
        for (i = 0; info->pResults && i < info->swapchainCount; i++) info->pResults[i] = res;
        return res;
    }

    for (i = 0; i < info->swapchainCount; i++)
    {
        wait_for_frame_rate( swapchain_from_handle( info->pSwapchains[i] ) );
        res = present_image( swapchain_from_handle( info->pSwapchains[i] ), info->pImageIndices[i], fence );
        if (info->pResults) info->pResults[i] = res;
        if (res < 0 && ret >= 0) ret = res;
        else if (res > 0 && !ret) ret = res;
    }
    if (fence >= 0) close( fence );
    return ret;
}


/* Entry points */

static PFN_vkVoidFunction android_vkGetInstanceProcAddr( VkInstance instance, const char *name );
static PFN_vkVoidFunction android_vkGetDeviceProcAddr( VkDevice device, const char *name );

static const struct
{
    const char *name;
    void       *func;
    void      **host;     /* where the host's function is kept, for the ones that wrap it */
    BOOL        device;   /* dispatches on a device or one of its objects */
}
entry_points[] =
{
    {"vkGetInstanceProcAddr", android_vkGetInstanceProcAddr},
    {"vkGetDeviceProcAddr", android_vkGetDeviceProcAddr},
    {"vkEnumerateInstanceExtensionProperties", android_vkEnumerateInstanceExtensionProperties,
     (void **)&host_vkEnumerateInstanceExtensionProperties},
    {"vkCreateInstance", android_vkCreateInstance, (void **)&host_vkCreateInstance},
    {"vkEnumerateDeviceExtensionProperties", android_vkEnumerateDeviceExtensionProperties,
     (void **)&host_vkEnumerateDeviceExtensionProperties},
    {"vkCreateDevice", android_vkCreateDevice, (void **)&host_vkCreateDevice},
    {"vkDestroySurfaceKHR", android_vkDestroySurfaceKHR},
    {"vkGetPhysicalDeviceSurfaceSupportKHR", android_vkGetPhysicalDeviceSurfaceSupportKHR},
    {"vkGetPhysicalDeviceSurfaceCapabilitiesKHR", android_vkGetPhysicalDeviceSurfaceCapabilitiesKHR},
    {"vkGetPhysicalDeviceSurfaceCapabilities2KHR", android_vkGetPhysicalDeviceSurfaceCapabilities2KHR},
    {"vkGetPhysicalDeviceSurfaceFormatsKHR", android_vkGetPhysicalDeviceSurfaceFormatsKHR},
    {"vkGetPhysicalDeviceSurfaceFormats2KHR", android_vkGetPhysicalDeviceSurfaceFormats2KHR},
    {"vkGetPhysicalDeviceSurfacePresentModesKHR", android_vkGetPhysicalDeviceSurfacePresentModesKHR},
    {"vkGetPhysicalDevicePresentRectanglesKHR", android_vkGetPhysicalDevicePresentRectanglesKHR},
    {"vkDestroyDevice", android_vkDestroyDevice, NULL, TRUE},
    {"vkGetDeviceGroupPresentCapabilitiesKHR", android_vkGetDeviceGroupPresentCapabilitiesKHR, NULL, TRUE},
    {"vkGetDeviceGroupSurfacePresentModesKHR", android_vkGetDeviceGroupSurfacePresentModesKHR, NULL, TRUE},
    {"vkCreateSwapchainKHR", android_vkCreateSwapchainKHR, NULL, TRUE},
    {"vkDestroySwapchainKHR", android_vkDestroySwapchainKHR, NULL, TRUE},
    {"vkGetSwapchainImagesKHR", android_vkGetSwapchainImagesKHR, NULL, TRUE},
    {"vkAcquireNextImageKHR", android_vkAcquireNextImageKHR, NULL, TRUE},
    {"vkAcquireNextImage2KHR", android_vkAcquireNextImage2KHR, NULL, TRUE},
    {"vkQueuePresentKHR", android_vkQueuePresentKHR, NULL, TRUE},
};

/* Host entry points for its own surfaces and swapchains, and for the hidden extensions: nothing may reach them. */
static BOOL is_hidden_entry_point( const char *name )
{
    static const char *const names[] =
    {
        "vkCreateAndroidSurfaceKHR",
        "vkCreateSharedSwapchainsKHR",
        "vkGetSwapchainStatusKHR",
        "vkReleaseSwapchainImagesEXT",
        "vkReleaseSwapchainImagesKHR",
        "vkWaitForPresentKHR",
        "vkWaitForPresent2KHR",
        "vkSetHdrMetadataEXT",
        "vkGetRefreshCycleDurationGOOGLE",
        "vkGetPastPresentationTimingGOOGLE",
        "vkGetSwapchainCounterEXT",
        "vkSetLatencySleepModeNV",
        "vkLatencySleepNV",
        "vkSetLatencyMarkerNV",
        "vkGetLatencyTimingsNV",
        "vkQueueNotifyOutOfBandNV",
    };
    return has_name( names, ARRAY_SIZE(names), name );
}

static PFN_vkVoidFunction android_vkGetInstanceProcAddr( VkInstance instance, const char *name )
{
    PFN_vkVoidFunction host = host_vkGetInstanceProcAddr( instance, name );
    unsigned int i;

    if (is_hidden_entry_point( name )) return NULL;
    for (i = 0; i < ARRAY_SIZE(entry_points); i++)
    {
        if (strcmp( entry_points[i].name, name )) continue;
        if (entry_points[i].host)
        {
            /* a wrapper is only as available as what it wraps */
            if (!host) return NULL;
            *entry_points[i].host = host;
        }
        return entry_points[i].func;
    }
    return host;
}

static PFN_vkVoidFunction android_vkGetDeviceProcAddr( VkDevice device, const char *name )
{
    unsigned int i;

    if (is_hidden_entry_point( name )) return NULL;
    for (i = 0; i < ARRAY_SIZE(entry_points); i++)
        if (entry_points[i].device && !strcmp( entry_points[i].name, name )) return entry_points[i].func;
    return host_vkGetDeviceProcAddr( device, name );
}

static void android_wrap_host_proc_addr( PFN_vkGetInstanceProcAddr *instance_proc_addr, PFN_vkGetDeviceProcAddr *device_proc_addr )
{
    const char *hal = getenv( "WINE_ANDROID_VULKAN_HAL" );

    if (hal && *hal) load_hal_driver( hal, instance_proc_addr, device_proc_addr );
    host_vkGetInstanceProcAddr = *instance_proc_addr;
    host_vkGetDeviceProcAddr = *device_proc_addr;
    /* the global functions; the others are kept when win32u looks them up for an instance */
    host_vkEnumerateInstanceExtensionProperties = (void *)host_vkGetInstanceProcAddr( NULL, "vkEnumerateInstanceExtensionProperties" );
    host_vkCreateInstance = (void *)host_vkGetInstanceProcAddr( NULL, "vkCreateInstance" );
    *instance_proc_addr = android_vkGetInstanceProcAddr;
    *device_proc_addr = android_vkGetDeviceProcAddr;
}

static void android_map_instance_extensions( struct vulkan_instance_extensions *extensions )
{
    /* a Windows surface is one of ours, which needs nothing of the host instance but VK_KHR_surface's types */
    if (extensions->has_VK_KHR_surface) extensions->has_VK_KHR_win32_surface = 1;
}

static void android_map_device_extensions( struct vulkan_device_extensions *extensions )
{
    if (extensions->has_VK_KHR_external_memory_win32) extensions->has_VK_KHR_external_memory_fd = 1;
    if (extensions->has_VK_KHR_external_memory_fd) extensions->has_VK_KHR_external_memory_win32 = 1;
    if (extensions->has_VK_KHR_external_semaphore_win32) extensions->has_VK_KHR_external_semaphore_fd = 1;
    if (extensions->has_VK_KHR_external_semaphore_fd) extensions->has_VK_KHR_external_semaphore_win32 = 1;
    if (extensions->has_VK_KHR_external_fence_win32) extensions->has_VK_KHR_external_fence_fd = 1;
    if (extensions->has_VK_KHR_external_fence_fd) extensions->has_VK_KHR_external_fence_win32 = 1;
}

static const struct vulkan_driver_funcs android_vulkan_driver_funcs =
{
    .p_vulkan_surface_create = android_vulkan_surface_create,
    .p_get_physical_device_presentation_support = android_get_physical_device_presentation_support,
    .p_map_instance_extensions = android_map_instance_extensions,
    .p_map_device_extensions = android_map_device_extensions,
    .p_wrap_host_proc_addr = android_wrap_host_proc_addr,
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

    *driver_funcs = &android_vulkan_driver_funcs;
    return STATUS_SUCCESS;
}
