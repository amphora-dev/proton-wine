/* knife13: AHB-import swapchain (Winlator vk_image route).
 * NOT a VkLayer. No GIPA/Negotiate export. No GraphicBuffer.
 * No runtime wine-table / needle / GIPA / GDPA hooks.
 * Wine CreateSwapchain (wineandroid / win32u) calls amphora_ahb_sc_* at
 * compile-time; this file only implements import + register + exports.
 * Winlator steps: dequeue AHB → GetAHBProps → CreateImage(EXTERNAL) →
 * ImportAHB+dedicated alloc → BindImageMemory.
 */
#ifndef VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID
#define VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID 0x00000400
#endif
#ifndef VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO
#define VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO 1000072001
#endif
#ifndef VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO
#define VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO 1000127001
#endif
#ifndef VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID
#define VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID 1000129001
#endif
#ifndef VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID
#define VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID 1000129002
#endif
#ifndef VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID
#define VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID 1000129003
#endif
#ifndef VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR
#define VK_STRUCTURE_TYPE_ACQUIRE_NEXT_IMAGE_INFO_KHR 1000189004
#endif

typedef struct VkExternalMemoryImageCreateInfo_ahb {
    VkStructureType sType;
    const void *pNext;
    VkExternalMemoryHandleTypeFlags handleTypes;
} VkExternalMemoryImageCreateInfo_ahb;
typedef struct VkMemoryDedicatedAllocateInfo_ahb {
    VkStructureType sType;
    const void *pNext;
    VkImage image;
    VkBuffer buffer;
} VkMemoryDedicatedAllocateInfo_ahb;
typedef struct VkAndroidHardwareBufferFormatPropertiesANDROID_ahb {
    VkStructureType sType;
    void *pNext;
    VkFormat format;
    uint64_t externalFormat;
    VkFormatFeatureFlags formatFeatures;
    VkComponentMapping samplerYcbcrConversionComponents;
    VkSamplerYcbcrModelConversion suggestedYcbcrModel;
    VkSamplerYcbcrRange suggestedYcbcrRange;
    VkChromaLocation suggestedXChromaOffset;
    VkChromaLocation suggestedYChromaOffset;
} VkAndroidHardwareBufferFormatPropertiesANDROID_ahb;
typedef struct VkAndroidHardwareBufferPropertiesANDROID_ahb {
    VkStructureType sType;
    void *pNext;
    VkDeviceSize allocationSize;
    uint32_t memoryTypeBits;
} VkAndroidHardwareBufferPropertiesANDROID_ahb;
typedef struct VkImportAndroidHardwareBufferInfoANDROID_ahb {
    VkStructureType sType;
    const void *pNext;
    void *buffer;
} VkImportAndroidHardwareBufferInfoANDROID_ahb;
typedef struct VkAcquireNextImageInfoKHR_ahb {
    VkStructureType sType;
    const void *pNext;
    VkSwapchainKHR swapchain;
    uint64_t timeout;
    VkSemaphore semaphore;
    VkFence fence;
    uint32_t deviceMask;
} VkAcquireNextImageInfoKHR_ahb;

typedef VkResult (*PFN_vkGetAndroidHardwareBufferPropertiesANDROID_ahb)(
    VkDevice, const void *, VkAndroidHardwareBufferPropertiesANDROID_ahb *);
typedef VkResult (*PFN_vkAcquireNextImage2KHR_ahb)(VkDevice, const VkAcquireNextImageInfoKHR_ahb *, uint32_t *);

#define AHB_SC_MAX_SURFACES 8
#define AHB_SC_MAX_IMAGES   NB_CACHED_BUFFERS
#define AHB_SC_MAGIC        0x41484253u
#define AHB_SC_MAX_DEVICES  8

enum { AHB_SC_FREE = 0, AHB_SC_ACQUIRED = 1, AHB_SC_PRESENTED = 2 };

struct ahb_sc_surface {
    VkSurfaceKHR surface;
    struct amphora_win *win;
};
struct ahb_sc {
    uint32_t magic;
    VkDevice device;
    VkPhysicalDevice phys;
    VkSurfaceKHR surface;
    struct amphora_win *win;
    uint32_t count;
    VkFormat format;
    VkExtent2D extent;
    VkImage images[AHB_SC_MAX_IMAGES];
    VkDeviceMemory memories[AHB_SC_MAX_IMAGES];
    struct amphora_buf *bufs[AHB_SC_MAX_IMAGES];
    uint8_t state[AHB_SC_MAX_IMAGES];
    uint8_t queued_to_host[AHB_SC_MAX_IMAGES];
    uint32_t next_acquire;
    VkQueue signal_queue;
    int signal_queue_ok;
};

static struct ahb_sc_surface g_ahb_surfaces[AHB_SC_MAX_SURFACES];
static int g_ahb_surface_n;
static pthread_mutex_t g_ahb_sc_lock = PTHREAD_MUTEX_INITIALIZER;
static VkInstance g_ahb_instance;

static PFN_vkGetInstanceProcAddr g_real_gipa;
static PFN_vkGetDeviceProcAddr g_real_gdpa;
static PFN_vkCreateSwapchainKHR g_real_CreateSwapchainKHR;
static PFN_vkDestroySwapchainKHR g_real_DestroySwapchainKHR;
static PFN_vkGetSwapchainImagesKHR g_real_GetSwapchainImagesKHR;
static PFN_vkAcquireNextImageKHR g_real_AcquireNextImageKHR;
static PFN_vkAcquireNextImage2KHR_ahb g_real_AcquireNextImage2KHR;
static PFN_vkQueuePresentKHR g_real_QueuePresentKHR;

static struct { VkDevice dev; VkPhysicalDevice phys; } g_ahb_devs[AHB_SC_MAX_DEVICES];
static int g_ahb_dev_n;

static VkResult amphora_vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
                                             const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain);
static void amphora_vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                          const VkAllocationCallbacks *pAllocator);
static VkResult amphora_vkGetSwapchainImagesKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages);
static VkResult amphora_vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
                                              VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex);
static VkResult amphora_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo);
static VkResult amphora_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR_ahb *info,
                                               uint32_t *pImageIndex);

static void ahb_resolve_real_vulkan(void)
{
    void *lib;
    if (g_real_gipa && g_real_gdpa) return;
    lib = dlopen("libvulkan.so", RTLD_NOW);
    if (!lib) {
        LOGE("AHB_SC dlopen libvulkan failed: %s", dlerror());
        return;
    }
    if (!g_real_gipa)
        g_real_gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!g_real_gdpa)
        g_real_gdpa = (PFN_vkGetDeviceProcAddr)dlsym(lib, "vkGetDeviceProcAddr");
    LOGI("AHB_SC real gipa=%p gdpa=%p (no hooks)", (void *)g_real_gipa, (void *)g_real_gdpa);
}

static void ahb_sc_register_surface(VkSurfaceKHR surface, struct amphora_win *win)
{
    int i;
    if (!surface || !win) return;
    pthread_mutex_lock(&g_ahb_sc_lock);
    for (i = 0; i < g_ahb_surface_n; i++) {
        if (g_ahb_surfaces[i].surface == surface) {
            g_ahb_surfaces[i].win = win;
            pthread_mutex_unlock(&g_ahb_sc_lock);
            return;
        }
    }
    if (g_ahb_surface_n < AHB_SC_MAX_SURFACES) {
        g_ahb_surfaces[g_ahb_surface_n].surface = surface;
        g_ahb_surfaces[g_ahb_surface_n].win = win;
        g_ahb_surface_n++;
        LOGI("AHB_SC register surface=%p win=%p n=%d", (void *)(uintptr_t)surface, (void *)win, g_ahb_surface_n);
    } else {
        LOGW("AHB_SC surface table full");
    }
    pthread_mutex_unlock(&g_ahb_sc_lock);
}

static struct amphora_win *ahb_sc_win_for_surface(VkSurfaceKHR surface)
{
    int i;
    struct amphora_win *w = NULL;
    pthread_mutex_lock(&g_ahb_sc_lock);
    for (i = 0; i < g_ahb_surface_n; i++) {
        if (g_ahb_surfaces[i].surface == surface) { w = g_ahb_surfaces[i].win; break; }
    }
    pthread_mutex_unlock(&g_ahb_sc_lock);
    return w;
}

static void ahb_sc_stash_device(VkDevice dev, VkPhysicalDevice phys)
{
    int i;
    if (!dev) return;
    pthread_mutex_lock(&g_ahb_sc_lock);
    for (i = 0; i < g_ahb_dev_n; i++) {
        if (g_ahb_devs[i].dev == dev) { g_ahb_devs[i].phys = phys; pthread_mutex_unlock(&g_ahb_sc_lock); return; }
    }
    if (g_ahb_dev_n < AHB_SC_MAX_DEVICES) {
        g_ahb_devs[g_ahb_dev_n].dev = dev;
        g_ahb_devs[g_ahb_dev_n].phys = phys;
        g_ahb_dev_n++;
        LOGI("AHB_SC vkCreateDevice stash dev=%p phys=%p", (void *)dev, (void *)phys);
    }
    pthread_mutex_unlock(&g_ahb_sc_lock);
}

static VkPhysicalDevice ahb_sc_phys_for_device(VkDevice dev)
{
    int i;
    VkPhysicalDevice p = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_ahb_sc_lock);
    for (i = 0; i < g_ahb_dev_n; i++) {
        if (g_ahb_devs[i].dev == dev) { p = g_ahb_devs[i].phys; break; }
    }
    pthread_mutex_unlock(&g_ahb_sc_lock);
    return p;
}

static struct ahb_sc *ahb_sc_from_handle(VkSwapchainKHR sc)
{
    struct ahb_sc *s = (struct ahb_sc *)(uintptr_t)sc;
    if (!s || s->magic != AHB_SC_MAGIC) return NULL;
    return s;
}

static uint32_t ahb_sc_find_mem_type(VkPhysicalDevice phys, uint32_t bits)
{
    PFN_vkGetPhysicalDeviceMemoryProperties getmp = NULL;
    VkPhysicalDeviceMemoryProperties mp;
    uint32_t i, fallback = 0;
    if (!phys) {
        for (i = 0; i < 32; i++) if (bits & (1u << i)) return i;
        return 0;
    }
    ahb_resolve_real_vulkan();
    if (g_real_gipa && g_ahb_instance)
        getmp = (PFN_vkGetPhysicalDeviceMemoryProperties)
            g_real_gipa(g_ahb_instance, "vkGetPhysicalDeviceMemoryProperties");
    if (!getmp) {
        LOGW("AHB_SC no GetPhysicalDeviceMemoryProperties — use lowest bit");
        for (i = 0; i < 32; i++) if (bits & (1u << i)) return i;
        return 0;
    }
    getmp(phys, &mp);
    for (i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) == 0) continue;
        if (!fallback) fallback = i;
        if (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
            return i;
    }
    return fallback;
}

static void ahb_sc_ensure_device_procs(VkDevice device)
{
    ahb_resolve_real_vulkan();
    if (!device || !g_real_gdpa) return;
    if (!g_real_CreateSwapchainKHR)
        g_real_CreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)g_real_gdpa(device, "vkCreateSwapchainKHR");
    if (!g_real_DestroySwapchainKHR)
        g_real_DestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)g_real_gdpa(device, "vkDestroySwapchainKHR");
    if (!g_real_GetSwapchainImagesKHR)
        g_real_GetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)g_real_gdpa(device, "vkGetSwapchainImagesKHR");
    if (!g_real_AcquireNextImageKHR)
        g_real_AcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)g_real_gdpa(device, "vkAcquireNextImageKHR");
    if (!g_real_AcquireNextImage2KHR)
        g_real_AcquireNextImage2KHR = (PFN_vkAcquireNextImage2KHR_ahb)g_real_gdpa(device, "vkAcquireNextImage2KHR");
    if (!g_real_QueuePresentKHR)
        g_real_QueuePresentKHR = (PFN_vkQueuePresentKHR)g_real_gdpa(device, "vkQueuePresentKHR");
}

static void ahb_sc_ensure_instance_procs(VkInstance inst)
{
    ahb_resolve_real_vulkan();
    if (!g_real_gipa) return;
    if (!inst) return;
    if (!g_ahb_instance) g_ahb_instance = inst;
}

static VkResult ahb_sc_import_one(VkDevice device, VkPhysicalDevice phys,
                                  struct amphora_buf *buf, VkFormat format, VkExtent2D extent,
                                  VkImageUsageFlags usage,
                                  VkSharingMode sharing, uint32_t qcount, const uint32_t *qindices,
                                  VkImage *out_image, VkDeviceMemory *out_mem)
{
    PFN_vkCreateImage create_image;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID_ahb get_ahb_props;
    PFN_vkAllocateMemory alloc_mem;
    PFN_vkBindImageMemory bind_mem;
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_mem;
    VkExternalMemoryImageCreateInfo_ahb ext;
    VkImageCreateInfo ici;
    VkAndroidHardwareBufferFormatPropertiesANDROID_ahb fmt;
    VkAndroidHardwareBufferPropertiesANDROID_ahb props;
    VkImportAndroidHardwareBufferInfoANDROID_ahb import_info;
    VkMemoryDedicatedAllocateInfo_ahb dedicated;
    VkMemoryAllocateInfo mai;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat img_fmt = format;
    VkResult r;

    if (!buf || !buf->ahb) {
        LOGW("AHB_SC import: no ahb on buf id=%d", buf ? buf->buffer_id : -1);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    ahb_resolve_real_vulkan();
    if (!g_real_gdpa) return VK_ERROR_INITIALIZATION_FAILED;

    create_image = (PFN_vkCreateImage)g_real_gdpa(device, "vkCreateImage");
    get_ahb_props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID_ahb)
        g_real_gdpa(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!get_ahb_props && g_real_gipa && g_ahb_instance)
        get_ahb_props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID_ahb)
            g_real_gipa(g_ahb_instance, "vkGetAndroidHardwareBufferPropertiesANDROID");
    if (!get_ahb_props && g_real_gipa)
        get_ahb_props = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID_ahb)
            g_real_gipa(VK_NULL_HANDLE, "vkGetAndroidHardwareBufferPropertiesANDROID");
    alloc_mem = (PFN_vkAllocateMemory)g_real_gdpa(device, "vkAllocateMemory");
    bind_mem = (PFN_vkBindImageMemory)g_real_gdpa(device, "vkBindImageMemory");
    destroy_image = (PFN_vkDestroyImage)g_real_gdpa(device, "vkDestroyImage");
    free_mem = (PFN_vkFreeMemory)g_real_gdpa(device, "vkFreeMemory");
    if (!create_image || !get_ahb_props || !alloc_mem || !bind_mem) {
        LOGW("AHB_SC import: missing device procs create=%p props=%p alloc=%p bind=%p",
             (void *)create_image, (void *)get_ahb_props, (void *)alloc_mem, (void *)bind_mem);
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.sType = (VkStructureType)VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID;
    memset(&props, 0, sizeof(props));
    props.sType = (VkStructureType)VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID;
    props.pNext = &fmt;
    r = get_ahb_props(device, buf->ahb, &props);
    if (r != VK_SUCCESS) {
        LOGW("AHB_SC GetAHBProperties failed res=%d", (int)r);
        return r;
    }
    if (fmt.format != VK_FORMAT_UNDEFINED)
        img_fmt = fmt.format;
    LOGI("AHB_SC GetAHBProperties size=%llu typeBits=0x%x vkFormat=%d (scFormat=%d) id=%d",
         (unsigned long long)props.allocationSize, props.memoryTypeBits,
         (int)fmt.format, (int)format, buf->buffer_id);

    memset(&ext, 0, sizeof(ext));
    ext.sType = (VkStructureType)VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO;
    ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;

    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.pNext = &ext;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = img_fmt;
    ici.extent.width = extent.width;
    ici.extent.height = extent.height;
    ici.extent.depth = 1;
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = sharing;
    ici.queueFamilyIndexCount = qcount;
    ici.pQueueFamilyIndices = qindices;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    r = create_image(device, &ici, NULL, &image);
    if (r != VK_SUCCESS) {
        LOGW("AHB_SC vkCreateImage failed res=%d fmt=%d", (int)r, (int)img_fmt);
        return r;
    }

    memset(&import_info, 0, sizeof(import_info));
    import_info.sType = (VkStructureType)VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID;
    import_info.buffer = buf->ahb;

    memset(&dedicated, 0, sizeof(dedicated));
    dedicated.sType = (VkStructureType)VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO;
    dedicated.pNext = &import_info;
    dedicated.image = image;

    memset(&mai, 0, sizeof(mai));
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.pNext = &dedicated;
    mai.allocationSize = props.allocationSize;
    mai.memoryTypeIndex = ahb_sc_find_mem_type(phys, props.memoryTypeBits);

    r = alloc_mem(device, &mai, NULL, &memory);
    if (r != VK_SUCCESS) {
        LOGW("AHB_SC vkAllocateMemory import failed res=%d size=%llu typeBits=0x%x typeIdx=%u",
             (int)r, (unsigned long long)props.allocationSize, props.memoryTypeBits, mai.memoryTypeIndex);
        if (destroy_image) destroy_image(device, image, NULL);
        return r;
    }
    r = bind_mem(device, image, memory, 0);
    if (r != VK_SUCCESS) {
        LOGW("AHB_SC vkBindImageMemory failed res=%d", (int)r);
        if (free_mem) free_mem(device, memory, NULL);
        if (destroy_image) destroy_image(device, image, NULL);
        return r;
    }
    *out_image = image;
    *out_mem = memory;
    LOGI("AHB_SC import ok id=%d ahb=%p image=%p mem=%p size=%llu fmt=%d",
         buf->buffer_id, buf->ahb, (void *)(uintptr_t)image, (void *)(uintptr_t)memory,
         (unsigned long long)props.allocationSize, (int)img_fmt);
    return VK_SUCCESS;
}

static void ahb_sc_destroy_obj(struct ahb_sc *sc)
{
    PFN_vkDestroyImage destroy_image;
    PFN_vkFreeMemory free_mem;
    uint32_t i;
    if (!sc) return;
    ahb_resolve_real_vulkan();
    destroy_image = g_real_gdpa ? (PFN_vkDestroyImage)g_real_gdpa(sc->device, "vkDestroyImage") : NULL;
    free_mem = g_real_gdpa ? (PFN_vkFreeMemory)g_real_gdpa(sc->device, "vkFreeMemory") : NULL;
    for (i = 0; i < sc->count; i++) {
        if (sc->images[i] && destroy_image)
            destroy_image(sc->device, sc->images[i], NULL);
        if (sc->memories[i] && free_mem)
            free_mem(sc->device, sc->memories[i], NULL);
        if (sc->bufs[i] && sc->win) {
            if (sc->state[i] != AHB_SC_PRESENTED)
                win_cancel(&sc->win->win, &sc->bufs[i]->buffer, -1);
            sc->bufs[i] = NULL;
        }
        sc->images[i] = VK_NULL_HANDLE;
        sc->memories[i] = VK_NULL_HANDLE;
        sc->state[i] = AHB_SC_FREE;
    }
    sc->magic = 0;
    free(sc);
}

static VkResult amphora_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSwapchainKHR *pSwapchain)
{
    struct amphora_win *win;
    struct ahb_sc *sc = NULL;
    uint32_t want, i;
    VkResult r;
    int fence = -1;

    ahb_sc_ensure_device_procs(device);
    if (!pCreateInfo || !pSwapchain)
        return VK_ERROR_INITIALIZATION_FAILED;

    LOGI("AHB_SC CreateSwapchain enter device=%p surface=%p (source-path, no hooks)",
         (void *)device, (void *)(uintptr_t)pCreateInfo->surface);

    win = ahb_sc_win_for_surface(pCreateInfo->surface);
    if (!win) {
        LOGW("AHB_SC CreateSwapchain no registered surface=%p — ICD fallback createSC=%p",
             (void *)(uintptr_t)pCreateInfo->surface, (void *)g_real_CreateSwapchainKHR);
        if (!g_real_CreateSwapchainKHR)
            return VK_ERROR_INITIALIZATION_FAILED;
        return g_real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    want = pCreateInfo->minImageCount;
    if (want < 2) want = 2;
    /* BLAST SurfaceView: setMaxDequeuedBufferCount rejects count < minUndequeued+1
     * (HA262 log: requested 3 < minimum 4 when MIN_UNDEQUEUED_BUFFERS=3). Soft-cap
     * used to be 3 for ImageReader recycle; raise floor to 4, soft-cap to 5. */
    {
        int min_undequeued = 0;
        if (win_query(&win->win, NATIVE_WINDOW_MIN_UNDEQUEUED_BUFFERS, &min_undequeued) == 0
            && min_undequeued > 0) {
            uint32_t floor = (uint32_t)min_undequeued + 1u;
            if (want < floor) want = floor;
        } else if (want < 4) {
            want = 4;
        }
    }
    if (want > 5) want = 5;
    if (want > AHB_SC_MAX_IMAGES) want = AHB_SC_MAX_IMAGES;

    {
        int set_ret = win_perform(&win->win, NATIVE_WINDOW_SET_BUFFER_COUNT, (int)want);
        LOGI("AHB_SC SET_BUFFER_COUNT want=%u ret=%d", want, set_ret);
    }

    sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->magic = AHB_SC_MAGIC;
    sc->device = device;
    sc->phys = ahb_sc_phys_for_device(device);
    sc->surface = pCreateInfo->surface;
    sc->win = win;
    sc->format = pCreateInfo->imageFormat;
    sc->extent = pCreateInfo->imageExtent;

    for (i = 0; i < want; i++) {
        struct ANativeWindowBuffer *anwb = NULL;
        struct amphora_buf *buf;
        fence = -1;
        if (win_dequeue(&win->win, &anwb, &fence) != 0 || !anwb) {
            LOGW("AHB_SC dequeue[%u] failed", i);
            r = VK_ERROR_INITIALIZATION_FAILED;
            goto fail;
        }
        if (fence >= 0) close(fence);
        buf = (struct amphora_buf *)anwb;
        if (!buf->ahb) {
            LOGW("AHB_SC dequeue[%u] id=%d has no AHB — cannot import", i, buf->buffer_id);
            r = VK_ERROR_INITIALIZATION_FAILED;
            goto fail;
        }
        r = ahb_sc_import_one(device, sc->phys, buf, sc->format, sc->extent,
                              pCreateInfo->imageUsage,
                              pCreateInfo->imageSharingMode,
                              pCreateInfo->queueFamilyIndexCount,
                              pCreateInfo->pQueueFamilyIndices,
                              &sc->images[i], &sc->memories[i]);
        if (r != VK_SUCCESS) {
            LOGW("AHB_SC import[%u] failed res=%d — fall back to ICD CreateSwapchain", i, (int)r);
            win_cancel(&win->win, anwb, -1);
            goto fallback;
        }
        sc->bufs[i] = buf;
        sc->state[i] = AHB_SC_FREE;
        sc->queued_to_host[i] = 0;
        sc->count++;
    }

    /* Queue every imported buffer to the host once at create. Present must not
     * block on ANW dequeue/recycle; host already holds the AHB surfaces. */
    for (i = 0; i < sc->count; i++) {
        int qret = win_queue(&win->win, &sc->bufs[i]->buffer, -1);
        sc->queued_to_host[i] = 1;
        LOGI("AHB_SC create win_queue idx=%u id=%d ret=%d", i, sc->bufs[i]->buffer_id, qret);
    }

    {
        PFN_vkGetDeviceQueue getq = g_real_gdpa
            ? (PFN_vkGetDeviceQueue)g_real_gdpa(device, "vkGetDeviceQueue") : NULL;
        if (getq) {
            getq(device, 0, 0, &sc->signal_queue);
            sc->signal_queue_ok = sc->signal_queue != VK_NULL_HANDLE;
        }
    }

    *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    LOGI("AHB_SC create images=%u import=ok format=%d extent=%ux%u surface=%p sc=%p",
         sc->count, (int)sc->format, sc->extent.width, sc->extent.height,
         (void *)(uintptr_t)pCreateInfo->surface, (void *)sc);
    return VK_SUCCESS;

fallback:
    ahb_sc_destroy_obj(sc);
    sc = NULL;
    if (!g_real_CreateSwapchainKHR) {
        LOGW("AHB_SC fallback: no real CreateSwapchain");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    LOGW("AHB_SC create images fallback ICD path");
    return g_real_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);

fail:
    ahb_sc_destroy_obj(sc);
    return r;
}

static void amphora_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator)
{
    struct ahb_sc *sc = ahb_sc_from_handle(swapchain);
    ahb_sc_ensure_device_procs(device);
    if (!sc) {
        if (g_real_DestroySwapchainKHR)
            g_real_DestroySwapchainKHR(device, swapchain, pAllocator);
        return;
    }
    LOGI("AHB_SC destroy sc=%p images=%u", (void *)sc, sc->count);
    ahb_sc_destroy_obj(sc);
}

static VkResult amphora_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pSwapchainImageCount, VkImage *pSwapchainImages)
{
    struct ahb_sc *sc = ahb_sc_from_handle(swapchain);
    uint32_t i;
    if (!sc) {
        ahb_sc_ensure_device_procs(device);
        if (!g_real_GetSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
        return g_real_GetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    }
    if (!pSwapchainImageCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pSwapchainImages) {
        *pSwapchainImageCount = sc->count;
        return VK_SUCCESS;
    }
    if (*pSwapchainImageCount < sc->count) {
        uint32_t n = *pSwapchainImageCount;
        for (i = 0; i < n; i++) pSwapchainImages[i] = sc->images[i];
        *pSwapchainImageCount = sc->count;
        return VK_INCOMPLETE;
    }
    for (i = 0; i < sc->count; i++) pSwapchainImages[i] = sc->images[i];
    *pSwapchainImageCount = sc->count;
    return VK_SUCCESS;
}

/* GPU signal is done on the DXVK queue thread (win32u QueueSubmit flush).
 * Never QueueSubmit from this IPC thread — it races VkQueue and drops fences. */
static VkResult ahb_sc_signal_acquire(struct ahb_sc *sc, VkSemaphore semaphore, VkFence fence)
{
    (void)sc;
    if (semaphore || fence)
        LOGI("AHB_SC acquire signal deferred-to-win32u sem=%p fence=%p",
             (void *)(uintptr_t)semaphore, (void *)(uintptr_t)fence);
    return VK_SUCCESS;
}

static VkResult amphora_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout,
    VkSemaphore semaphore, VkFence fence, uint32_t *pImageIndex)
{
    struct ahb_sc *sc = ahb_sc_from_handle(swapchain);
    uint32_t idx;
    (void)timeout;
    if (!sc) {
        ahb_sc_ensure_device_procs(device);
        if (!g_real_AcquireNextImageKHR) return VK_ERROR_INITIALIZATION_FAILED;
        return g_real_AcquireNextImageKHR(device, swapchain, timeout, semaphore, fence, pImageIndex);
    }
    if (!pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;

    {
        uint32_t i, match = sc->count;
        VkResult sr;
        for (i = 0; i < sc->count; i++) {
            idx = (sc->next_acquire + i) % sc->count;
            if (sc->state[idx] == AHB_SC_FREE) { match = idx; break; }
        }
        if (match == sc->count) {
            LOGW("AHB_SC acquire no FREE slot count=%u timeout=%llu",
                 sc->count, (unsigned long long)timeout);
            if (timeout == 0) return VK_NOT_READY;
            /* UINT64_MAX would hang the IPC server forever — return TIMEOUT. */
            return VK_TIMEOUT;
        }
        sc->state[match] = AHB_SC_ACQUIRED;
        sc->next_acquire = (match + 1) % sc->count;
        *pImageIndex = match;
        sr = ahb_sc_signal_acquire(sc, semaphore, fence);
        if (sr != VK_SUCCESS)
            LOGW("AHB_SC acquire signal idx=%u res=%d", match, (int)sr);
        return VK_SUCCESS;
    }
}

static VkResult amphora_vkAcquireNextImage2KHR(VkDevice device, const VkAcquireNextImageInfoKHR_ahb *info,
                                               uint32_t *pImageIndex)
{
    if (!info) return VK_ERROR_INITIALIZATION_FAILED;
    if (ahb_sc_from_handle(info->swapchain))
        return amphora_vkAcquireNextImageKHR(device, info->swapchain, info->timeout,
                                             info->semaphore, info->fence, pImageIndex);
    ahb_sc_ensure_device_procs(device);
    if (!g_real_AcquireNextImage2KHR) {
        return amphora_vkAcquireNextImageKHR(device, info->swapchain, info->timeout,
                                             info->semaphore, info->fence, pImageIndex);
    }
    return g_real_AcquireNextImage2KHR(device, info, pImageIndex);
}

/* Present wait-sems are consumed in win32u on the DXVK queue thread. */
static VkResult ahb_sc_wait_present_semaphores(VkQueue queue, const VkPresentInfoKHR *info)
{
    (void)queue;
    (void)info;
    return VK_SUCCESS;
}

static VkResult amphora_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pPresentInfo)
{
    uint32_t i;
    int any_ours = 0, any_theirs = 0;
    VkResult last = VK_SUCCESS;

    if (!pPresentInfo) return VK_ERROR_INITIALIZATION_FAILED;

    for (i = 0; i < pPresentInfo->swapchainCount; i++) {
        if (ahb_sc_from_handle(pPresentInfo->pSwapchains[i])) any_ours = 1;
        else any_theirs = 1;
    }

    if (any_ours)
        ahb_sc_wait_present_semaphores(queue, pPresentInfo);

    if (any_theirs && !any_ours) {
        ahb_resolve_real_vulkan();
        if (!g_real_QueuePresentKHR && g_real_gdpa && g_ahb_dev_n > 0)
            g_real_QueuePresentKHR = (PFN_vkQueuePresentKHR)
                g_real_gdpa(g_ahb_devs[0].dev, "vkQueuePresentKHR");
        if (g_real_QueuePresentKHR)
            return g_real_QueuePresentKHR(queue, pPresentInfo);
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    for (i = 0; i < pPresentInfo->swapchainCount; i++) {
        struct ahb_sc *sc = ahb_sc_from_handle(pPresentInfo->pSwapchains[i]);
        uint32_t idx = pPresentInfo->pImageIndices[i];
        int qret;
        if (!sc) {
            LOGW("AHB_SC QueuePresent mixed non-ours sc=%p — skipped",
                 (void *)(uintptr_t)pPresentInfo->pSwapchains[i]);
            if (pPresentInfo->pResults) pPresentInfo->pResults[i] = VK_ERROR_SURFACE_LOST_KHR;
            last = VK_ERROR_SURFACE_LOST_KHR;
            continue;
        }
        if (idx >= sc->count || !sc->bufs[idx]) {
            LOGW("AHB_SC QueuePresent bad idx=%u count=%u", idx, sc->count);
            if (pPresentInfo->pResults) pPresentInfo->pResults[i] = VK_ERROR_OUT_OF_DATE_KHR;
            last = VK_ERROR_OUT_OF_DATE_KHR;
            continue;
        }
        /* Already on host from create — only notify + recycle slot. */
        amphora_guest_queue_knife(sc->bufs[idx]);
        qret = 0;
        sc->state[idx] = AHB_SC_FREE;
        if (qret != 0) {
            if (pPresentInfo->pResults) pPresentInfo->pResults[i] = VK_ERROR_SURFACE_LOST_KHR;
            last = VK_ERROR_SURFACE_LOST_KHR;
        } else {
            if (pPresentInfo->pResults) pPresentInfo->pResults[i] = VK_SUCCESS;
        }
    }
    return last;
}

/* Exported so wineandroid / same-SO callers can invoke without a layer. */
void amphora_ahb_sc_stash_device(uint64_t device, uint64_t phys)
{
    ahb_sc_stash_device((VkDevice)(uintptr_t)device, (VkPhysicalDevice)(uintptr_t)phys);
}

VkResult amphora_ahb_sc_create_swapchain(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                         const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    return amphora_vkCreateSwapchainKHR(device, info, alloc, out);
}
void amphora_ahb_sc_destroy_swapchain(VkDevice device, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc)
{
    amphora_vkDestroySwapchainKHR(device, sc, alloc);
}
VkResult amphora_ahb_sc_get_images(VkDevice device, VkSwapchainKHR sc, uint32_t *count, VkImage *images)
{
    return amphora_vkGetSwapchainImagesKHR(device, sc, count, images);
}
VkResult amphora_ahb_sc_acquire(VkDevice device, VkSwapchainKHR sc, uint64_t timeout,
                                VkSemaphore sem, VkFence fence, uint32_t *idx)
{
    return amphora_vkAcquireNextImageKHR(device, sc, timeout, sem, fence, idx);
}
VkResult amphora_ahb_sc_present(VkQueue queue, const VkPresentInfoKHR *info)
{
    return amphora_vkQueuePresentKHR(queue, info);
}
void amphora_ahb_sc_register(uint64_t surface, void *win)
{
    ahb_sc_register_surface((VkSurfaceKHR)(uintptr_t)surface, (struct amphora_win *)win);
}
