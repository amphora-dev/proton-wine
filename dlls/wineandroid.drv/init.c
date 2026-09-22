/*
 * Android driver initialisation functions
 *
 * Copyright 1996, 2013, 2017 Alexandre Julliard
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

#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <string.h>
#include <dlfcn.h>
#include <link.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winreg.h"
#include "android.h"
#include "wine/server.h"
#include "wine/debug.h"

#ifndef WINE_JAVA_CLASS
#define WINE_JAVA_CLASS "org/winehq/wine/WineActivity"
#endif

WINE_DEFAULT_DEBUG_CHANNEL(android);

unsigned int screen_width = 0;
unsigned int screen_height = 0;
RECT virtual_screen_rect = { 0, 0, 0, 0 };

static const unsigned int screen_bpp = 32;  /* we don't support other modes */

static RECT monitor_rc_work;
static int device_init_done;

typedef struct
{
    struct gdi_physdev dev;
} ANDROID_PDEVICE;

static const struct user_driver_funcs android_drv_funcs;


/******************************************************************************
 *           init_monitors
 */
void init_monitors( int width, int height )
{
    static const WCHAR trayW[] = {'S','h','e','l','l','_','T','r','a','y','W','n','d',0};
    UNICODE_STRING name;
    RECT rect;
    HWND hwnd;

    RtlInitUnicodeString( &name, trayW );
    hwnd = NtUserFindWindowEx( 0, 0, &name, NULL, 0 );

    virtual_screen_rect.right = width;
    virtual_screen_rect.bottom = height;
    monitor_rc_work = virtual_screen_rect;

    if (!hwnd || !NtUserIsWindowVisible( hwnd )) return;
    if (!NtUserGetWindowRect( hwnd, &rect, NtUserGetWinMonitorDpi( hwnd, MDT_RAW_DPI ) )) return;
    if (rect.top) monitor_rc_work.bottom = rect.top;
    else monitor_rc_work.top = rect.bottom;
    TRACE( "found tray %p %s work area %s\n", hwnd,
           wine_dbgstr_rect( &rect ), wine_dbgstr_rect( &monitor_rc_work ));

    /* if we're notified from Java thread, update registry */
    if (event_source != -1) NtUserCallNoParam( NtUserCallNoParam_DisplayModeChanged );
}


/* wrapper for NtCreateKey that creates the key recursively if necessary */
static HKEY reg_create_key( const WCHAR *name, ULONG name_len )
{
    UNICODE_STRING nameW = { name_len, name_len, (WCHAR *)name };
    OBJECT_ATTRIBUTES attr;
    NTSTATUS status;
    HANDLE ret;

    attr.Length = sizeof(attr);
    attr.RootDirectory = 0;
    attr.ObjectName = &nameW;
    attr.Attributes = 0;
    attr.SecurityDescriptor = NULL;
    attr.SecurityQualityOfService = NULL;

    status = NtCreateKey( &ret, MAXIMUM_ALLOWED, &attr, 0, NULL, 0, NULL );
    if (status == STATUS_OBJECT_NAME_NOT_FOUND)
    {
        static const WCHAR registry_rootW[] = { '\\','R','e','g','i','s','t','r','y','\\' };
        DWORD pos = 0, i = 0, len = name_len / sizeof(WCHAR);

        /* don't try to create registry root */
        if (len > ARRAY_SIZE(registry_rootW) &&
            !memcmp( name, registry_rootW, sizeof(registry_rootW) ))
            i += ARRAY_SIZE(registry_rootW);

        while (i < len && name[i] != '\\') i++;
        if (i == len) return 0;
        for (;;)
        {
            nameW.Buffer = (WCHAR *)name + pos;
            nameW.Length = (i - pos) * sizeof(WCHAR);
            status = NtCreateKey( &ret, MAXIMUM_ALLOWED, &attr, 0, NULL, 0, NULL );

            if (attr.RootDirectory) NtClose( attr.RootDirectory );
            if (status) return 0;
            if (i == len) break;
            attr.RootDirectory = ret;
            while (i < len && name[i] == '\\') i++;
            pos = i;
            while (i < len && name[i] != '\\') i++;
        }
    }
    return ret;
}


/******************************************************************************
 *           set_screen_dpi
 */
void set_screen_dpi( DWORD dpi )
{
    static const WCHAR dpi_value_name[] = {'L','o','g','P','i','x','e','l','s',0};
    static const WCHAR dpi_key_name[] =
    {
        '\\','R','e','g','i','s','t','r','y',
        '\\','M','a','c','h','i','n','e',
        '\\','S','y','s','t','e','m',
        '\\','C','u','r','r','e','n','t','C','o','n','t','r','o','l','S','e','t',
        '\\','H','a','r','d','w','a','r','e',' ','P','r','o','f','i','l','e','s',
        '\\','C','u','r','r','e','n','t',
        '\\','S','o','f','t','w','a','r','e',
        '\\','F','o','n','t','s'
    };
    HKEY hkey;

    if ((hkey = reg_create_key( dpi_key_name, sizeof(dpi_key_name ))))
    {
        UNICODE_STRING name;
        RtlInitUnicodeString( &name, dpi_value_name );
        NtSetValueKey( hkey, &name, 0, REG_DWORD, &dpi, sizeof(dpi) );
        NtClose( hkey );
    }
}

/**********************************************************************
 *	     fetch_display_metrics
 */
static void fetch_display_metrics(void)
{
    if (event_source != -1) return;  /* for Java threads it will be set when the top view is created */

    SERVER_START_REQ( get_window_rectangles )
    {
        req->handle = wine_server_user_handle( NtUserGetDesktopWindow() );
        req->relative = COORDS_CLIENT;
        if (!wine_server_call( req ))
        {
            screen_width  = reply->window.right;
            screen_height = reply->window.bottom;
        }
    }
    SERVER_END_REQ;

    init_monitors( screen_width, screen_height );
    TRACE( "screen %ux%u\n", screen_width, screen_height );
}


/**********************************************************************
 *           device_init
 *
 * Perform initializations needed upon creation of the first device.
 */
static void device_init(void)
{
    device_init_done = TRUE;
    fetch_display_metrics();
}


/******************************************************************************
 *           create_android_physdev
 */
static ANDROID_PDEVICE *create_android_physdev(void)
{
    ANDROID_PDEVICE *physdev;

    if (!device_init_done) device_init();

    if (!(physdev = calloc( 1, sizeof(*physdev) ))) return NULL;
    return physdev;
}

/**********************************************************************
 *           ANDROID_CreateDC
 */
static BOOL ANDROID_CreateDC( PHYSDEV *pdev, LPCWSTR device, LPCWSTR output, const DEVMODEW *initData )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs.dc_funcs );
    return TRUE;
}


/**********************************************************************
 *           ANDROID_CreateCompatibleDC
 */
static BOOL ANDROID_CreateCompatibleDC( PHYSDEV orig, PHYSDEV *pdev )
{
    ANDROID_PDEVICE *physdev = create_android_physdev();

    if (!physdev) return FALSE;

    push_dc_driver( pdev, &physdev->dev, &android_drv_funcs.dc_funcs );
    return TRUE;
}


/**********************************************************************
 *           ANDROID_DeleteDC
 */
static BOOL ANDROID_DeleteDC( PHYSDEV dev )
{
    free( dev );
    return TRUE;
}


/***********************************************************************
 *           ANDROID_ChangeDisplaySettings
 */
LONG ANDROID_ChangeDisplaySettings( LPDEVMODEW displays, LPCWSTR primary_name, HWND hwnd, DWORD flags, LPVOID lpvoid )
{
    FIXME( "(%p,%s,%p,0x%08x,%p)\n", displays, debugstr_w(primary_name), hwnd, flags, lpvoid );
    return DISP_CHANGE_SUCCESSFUL;
}


/***********************************************************************
 *           ANDROID_UpdateDisplayDevices
 */
UINT ANDROID_UpdateDisplayDevices( const struct gdi_device_manager *device_manager, void *param )
{
    static const DWORD source_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_VGA_COMPATIBLE;
    struct pci_id pci_id = {0};
    struct gdi_monitor gdi_monitor =
    {
        .rc_monitor = virtual_screen_rect,
        .rc_work = monitor_rc_work,
    };
    const DEVMODEW mode =
    {
        .dmSize = sizeof(mode),
        .dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT | DM_BITSPERPEL |
                    DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY,
        .dmBitsPerPel = screen_bpp, .dmPelsWidth = screen_width, .dmPelsHeight = screen_height, .dmDisplayFrequency = 60,
    };
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    DEVMODEW current = mode;

    device_manager->add_gpu( NULL, &pci_id, NULL, param );
    device_manager->add_source( "Default", source_flags, dpi, param );
    device_manager->add_monitor( &gdi_monitor, param );

    current.dmFields |= DM_POSITION;
    device_manager->add_modes( &current, 1, &mode, param );

    return STATUS_SUCCESS;
}


static const struct user_driver_funcs android_drv_funcs =
{
    .dc_funcs.pCreateCompatibleDC = ANDROID_CreateCompatibleDC,
    .dc_funcs.pCreateDC = ANDROID_CreateDC,
    .dc_funcs.pDeleteDC = ANDROID_DeleteDC,
    .dc_funcs.priority = GDI_PRIORITY_GRAPHICS_DRV,

    .pGetKeyNameText = ANDROID_GetKeyNameText,
    .pMapVirtualKeyEx = ANDROID_MapVirtualKeyEx,
    .pVkKeyScanEx = ANDROID_VkKeyScanEx,
    .pSetCursor = ANDROID_SetCursor,
    .pChangeDisplaySettings = ANDROID_ChangeDisplaySettings,
    .pUpdateDisplayDevices = ANDROID_UpdateDisplayDevices,
    .pCreateDesktop = ANDROID_CreateDesktop,
    .pCreateWindow = ANDROID_CreateWindow,
    .pSetDesktopWindow = ANDROID_SetDesktopWindow,
    .pDesktopWindowProc = ANDROID_DesktopWindowProc,
    .pDestroyWindow = ANDROID_DestroyWindow,
    .pProcessEvents = ANDROID_ProcessEvents,
    .pSetCapture = ANDROID_SetCapture,
    .pSetParent = ANDROID_SetParent,
    .pShowWindow = ANDROID_ShowWindow,
    .pWindowMessage = ANDROID_WindowMessage,
    .pWindowPosChanging = ANDROID_WindowPosChanging,
    .pCreateWindowSurface = ANDROID_CreateWindowSurface,
    .pWindowPosChanged = ANDROID_WindowPosChanged,
    .pOpenGLInit = ANDROID_OpenGLInit,
    .pVulkanInit = ANDROID_VulkanInit,
};


static const JNINativeMethod methods[] =
{
    { "wine_desktop_changed", "(II)V", desktop_changed },
    { "wine_config_changed", "(I)V", config_changed },
    { "wine_surface_changed", "(ILandroid/view/Surface;Z)V", surface_changed },
    { "wine_motion_event", "(IIIIII)Z", motion_event },
    { "wine_keyboard_event", "(IIII)Z", keyboard_event },
    { "wine_init", "()V", wine_init_jni }
};

/* box64 intercepts dlopen/dlsym for the libs it wraps (libdl.so, libc.so,
 * libandroid.so — the latter for SysV-shm emulation) and hands out fake
 * handles whose symbol tables hide ANativeWindow_*, AHardwareBuffer_* and
 * ALooper_*; plain "liblog.so" fails outright on some devices. __loader_dlopen
 * / __loader_dlsym are not in box64's wrap table, so calling them directly
 * reaches the real bionic loader. Their caller address selects the linker
 * namespace: pass the resolved &dlopen (my_dlopen under box64 = the main
 * executable, the real dlopen in libdl otherwise) so the lookup lands in the
 * default namespace, which may open /system libs. Weak refs and a dlsym
 * bootstrap (box64 passes names it does not wrap through to the real dlsym)
 * keep non-box64 builds working; plain dlopen/dlsym is the last resort. */
extern void *__loader_dlopen( const char *, int, const void * ) __attribute__((weak));
extern void *__loader_dlsym( void *, const char *, const void * ) __attribute__((weak));
extern void *android_dlopen_ext( const char *, int, const void * ) __attribute__((weak));

typedef void *(*loader_dlopen_t)( const char *, int, const void * );
typedef void *(*loader_dlsym_t)( void *, const char *, const void * );
typedef void *(*loader_dext_t)( const char *, int, const void * );

static loader_dlopen_t p_loader_dlopen;
static loader_dlsym_t p_loader_dlsym;
static loader_dext_t p_android_dlopen_ext;
static const char *loader_src;

static void resolve_loader(void)
{
    if (loader_src) return;
    if (&__loader_dlopen)
    {
        p_loader_dlopen = &__loader_dlopen;
        p_loader_dlsym = &__loader_dlsym;
        loader_src = "weak";
    }
    else if (dlsym( RTLD_DEFAULT, "__loader_dlopen" ))
    {
        p_loader_dlopen = (loader_dlopen_t)dlsym( RTLD_DEFAULT, "__loader_dlopen" );
        p_loader_dlsym = (loader_dlsym_t)dlsym( RTLD_DEFAULT, "__loader_dlsym" );
        loader_src = "dlsym";
    }
    else if (&android_dlopen_ext)
    {
        p_android_dlopen_ext = &android_dlopen_ext;
        loader_src = "dlext";
    }
    else loader_src = "plain";
}

static void *real_dlopen( const char *name )
{
    void *handle = NULL;
    resolve_loader();
    if (p_loader_dlopen && (handle = p_loader_dlopen( name, RTLD_GLOBAL, (const void *)&dlopen )))
        return handle;
    if (p_android_dlopen_ext && (handle = p_android_dlopen_ext( name, RTLD_GLOBAL, NULL )))
        return handle;
    return dlopen( name, RTLD_GLOBAL );
}

static void *real_dlsym( void *handle, const char *name )
{
    void *sym = NULL;
    resolve_loader();
    if (p_loader_dlsym && handle && (sym = p_loader_dlsym( handle, name, (const void *)&dlsym )))
        return sym;
    return dlsym( handle, name );
}

#define DECL_FUNCPTR(f) typeof(f) * p##f = NULL

/* Under box64 the unixlib is x86_64: every dlopen/dlsym goes through box64's
 * wrappers, which hide the NDK symbols behind a fake libandroid handle, and
 * bionic's private __loader_* entry points are unreachable from x86 code. But
 * dlopen of a name box64 does not wrap returns a real handle, and dlsym on a
 * real handle yields callable bridges. libamphora_wsi.so is LD_PRELOAD'd into
 * the guest and links libandroid/liblog, so its dependency tree carries every
 * NDK symbol we need — look them up there first (same pattern as the GL probe
 * in opengl.c). */
static void *bridge_symbol( const char *name )
{
    static void *handle;
    static int tried;
    if (!tried)
    {
        const char *preload, *p;
        tried = 1;
        /* Bare-name dlopen cannot see the APK lib dir; LD_PRELOAD carries the
         * absolute path of the helper (box64's dlsym has no RTLD_DEFAULT). */
        if ((preload = getenv( "LD_PRELOAD" )))
        {
            for (p = preload; *p && !handle; )
            {
                const char *end = p + strcspn( p, ": " );
                char path[4096];
                size_t len = end - p;
                if (len && len < sizeof(path))
                {
                    memcpy( path, p, len );
                    path[len] = 0;
                    handle = dlopen( path, RTLD_NOW );
                }
                p = *end ? end + 1 : end;
            }
        }
        if (!handle) handle = dlopen( "libamphora_wsi.so", RTLD_NOW );
    }
    return handle ? dlsym( handle, name ) : NULL;
}

#define LOAD_FUNCPTR(lib, func) do { \
    if ((p##func = (typeof(p##func))bridge_symbol( #func )) == NULL && \
        (p##func = (typeof(p##func))real_dlsym( lib, #func )) == NULL) \
        { ERR( "can't find symbol %s\n", #func); abort(); return; } \
    } while(0)

DECL_FUNCPTR( __android_log_print );
DECL_FUNCPTR( ANativeWindow_fromSurface );
DECL_FUNCPTR( ANativeWindow_release );
DECL_FUNCPTR( AHardwareBuffer_describe );
DECL_FUNCPTR( AHardwareBuffer_acquire );
DECL_FUNCPTR( AHardwareBuffer_release );
DECL_FUNCPTR( AHardwareBuffer_lock );
DECL_FUNCPTR( AHardwareBuffer_unlock );
DECL_FUNCPTR( AHardwareBuffer_recvHandleFromUnixSocket );
DECL_FUNCPTR( AHardwareBuffer_sendHandleToUnixSocket );
DECL_FUNCPTR( ANativeWindowBuffer_getHardwareBuffer );
DECL_FUNCPTR( ALooper_acquire );
DECL_FUNCPTR( ALooper_forThread );
DECL_FUNCPTR( ALooper_addFd );
DECL_FUNCPTR( ALooper_removeFd );
DECL_FUNCPTR( ALooper_release );

/* Logging is optional: some box64 builds cannot load liblog at all. */
static int stub_android_log_print( int prio, const char *tag, const char *fmt, ... )
{
    (void)prio;
    (void)tag;
    (void)fmt;
    return 0;
}

static void load_android_libs(void)
{
    /* Absolute paths first: the bare soname resolves against the guest's
     * LD_LIBRARY_PATH (imagefs usr/lib) before /system on some setups. */
    /* lib*-real.so are symlinks published by libamphora_wsi's ctor under names
     * box64 does not wrap; bare/absolute libandroid.so hits box64's fake
     * handle. Absolute paths first for plain devices. */
    static const char *const android_names[] =
        { "libandroid-real.so", "/system/lib64/libandroid.so", "/system/lib/libandroid.so", "libandroid.so", NULL };
    static const char *const log_names[] =
        { "liblog-real.so", "/system/lib64/liblog.so", "/system/lib/liblog.so", "liblog.so", NULL };
    void *libandroid = NULL, *liblog = NULL;
    const char *const *name;
    const char *android_src = NULL, *log_src = NULL;

    for (name = android_names; *name && !libandroid; name++)
        if ((libandroid = real_dlopen( *name ))) android_src = *name;
    for (name = log_names; *name && !liblog; name++)
        if ((liblog = real_dlopen( *name ))) log_src = *name;

    {
        void *probe = bridge_symbol( "ALooper_forThread" );
        ERR( "load_android_libs: loader=%s bridge=%s android=%s/%s log=%s/%s\n", loader_src,
             probe ? "ok" : "none", android_src ? android_src : "FAIL", probe ? "via-bridge" : "direct",
             log_src ? log_src : "FAIL", probe ? "via-bridge" : "direct" );
        if (!libandroid && !probe)
        {
            ERR( "failed to load libandroid.so: %s\n", dlerror() );
            abort();
            return;
        }
    }
    if (!(p__android_log_print = (typeof(p__android_log_print))bridge_symbol( "__android_log_print" )) &&
        (!liblog || !(p__android_log_print = (typeof(p__android_log_print))real_dlsym( liblog, "__android_log_print" ))))
    {
        ERR( "failed to load liblog.so: %s - using stub\n", dlerror() );
        p__android_log_print = stub_android_log_print;
    }
    LOAD_FUNCPTR( libandroid, ANativeWindow_fromSurface );
    LOAD_FUNCPTR( libandroid, ANativeWindow_release );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_describe );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_acquire );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_release );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_lock );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_unlock );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_recvHandleFromUnixSocket );
    LOAD_FUNCPTR( libandroid, AHardwareBuffer_sendHandleToUnixSocket );
    LOAD_FUNCPTR( libandroid, ANativeWindowBuffer_getHardwareBuffer );
    LOAD_FUNCPTR( libandroid, ALooper_acquire );
    LOAD_FUNCPTR( libandroid, ALooper_forThread );
    LOAD_FUNCPTR( libandroid, ALooper_addFd );
    LOAD_FUNCPTR( libandroid, ALooper_removeFd );
    LOAD_FUNCPTR( libandroid, ALooper_release );
}

#undef DECL_FUNCPTR
#undef LOAD_FUNCPTR

NTSTATUS __wine_unix_lib_init(void)
{
    pthread_mutexattr_t attr;

    load_android_libs();

    pthread_mutexattr_init( &attr );
    pthread_mutexattr_settype( &attr, PTHREAD_MUTEX_RECURSIVE );
    pthread_mutex_init( &win_data_mutex, &attr );
    pthread_mutexattr_destroy( &attr );

    __wine_set_user_driver( &android_drv_funcs, WINE_GDI_DRIVER_VERSION );
    return STATUS_SUCCESS;
}

jint JNI_OnLoad( JavaVM *vm, void *reserved )
{
    JNIEnv *env;
    jclass class;

    load_android_libs();

    if ((*vm)->AttachCurrentThread( vm, &env, NULL ) != JNI_OK) return JNI_ERR;
    if (!(class = (*env)->FindClass( env, WINE_JAVA_CLASS ))) return JNI_ERR;
    (*env)->RegisterNatives( env, class, methods, ARRAY_SIZE( methods ));
    (*env)->DeleteLocalRef( env, class );
    return JNI_VERSION_1_6;
}

/* Proton ntdll only dlsyms __wine_unix_call_funcs (no upstream __wine_unix_lib_init
 * auto-call). Index 0 is invoked from DllMain via WINE_UNIX_CALL(0). */
static NTSTATUS androiddrv_unix_init( void *args )
{
    return __wine_unix_lib_init();
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    androiddrv_unix_init,
};

#ifdef _WIN64
const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    androiddrv_unix_init,
};
#endif /* _WIN64 */
