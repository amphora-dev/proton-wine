/*
 * Android driver definitions
 *
 * Copyright 2013 Alexandre Julliard
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

#ifndef __WINE_ANDROID_H
#define __WINE_ANDROID_H

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <pthread.h>
#include <jni.h>
#include <android/log.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <android/surface_control.h>

#include "windef.h"
#include "winbase.h"
#include "ntgdi.h"
#include "wine/gdi_driver.h"
#include "ntuser.h"
#include "wine/unixlib.h"
#include "android_native.h"


/**************************************************************************
 * Android interface
 */

#define DECL_FUNCPTR(f) extern typeof(f) * p##f
DECL_FUNCPTR( __android_log_print );
DECL_FUNCPTR( ANativeWindow_fromSurface );
DECL_FUNCPTR( ANativeWindow_release );

#ifdef __ANDROID_UNAVAILABLE_SYMBOLS_ARE_WEAK__
extern struct AHardwareBuffer* ANativeWindowBuffer_getHardwareBuffer(struct ANativeWindowBuffer* anwb) __INTRODUCED_IN(26);

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
DECL_FUNCPTR( ANativeWindow_acquire );
DECL_FUNCPTR( ANativeWindow_getWidth );
DECL_FUNCPTR( ANativeWindow_getHeight );
DECL_FUNCPTR( AHardwareBuffer_allocate );
DECL_FUNCPTR( ASurfaceControl_createFromWindow );
DECL_FUNCPTR( ASurfaceControl_acquire );
DECL_FUNCPTR( ASurfaceControl_release );
DECL_FUNCPTR( ASurfaceTransaction_create );
DECL_FUNCPTR( ASurfaceTransaction_delete );
DECL_FUNCPTR( ASurfaceTransaction_apply );
DECL_FUNCPTR( ASurfaceTransaction_reparent );
DECL_FUNCPTR( ASurfaceTransaction_setBuffer );
DECL_FUNCPTR( ASurfaceTransaction_setGeometry );
DECL_FUNCPTR( ASurfaceTransaction_setBufferTransparency );
DECL_FUNCPTR( ASurfaceTransaction_setVisibility );
DECL_FUNCPTR( ASurfaceTransaction_setOnComplete );
DECL_FUNCPTR( ASurfaceTransactionStats_getPreviousReleaseFenceFd );
/* API 31, NULL before that */
extern void (*pASurfaceTransaction_setEnableBackPressure)( ASurfaceTransaction *transaction, ASurfaceControl *surface_control, bool enable );
#endif

#undef DECL_FUNCPTR


/**************************************************************************
 * OpenGL driver
 */

extern UINT ANDROID_OpenGLInit( UINT version, const struct opengl_funcs *opengl_funcs, const struct opengl_driver_funcs **driver_funcs );


/**************************************************************************
 * Android pseudo-device
 */

extern void createDesktopView( int *event_source );
extern void register_native_window( HWND hwnd, struct ANativeWindow *win, BOOL client );
extern struct ANativeWindow *create_ioctl_window( HWND hwnd, BOOL opengl );
extern struct ANativeWindow *grab_ioctl_window( struct ANativeWindow *window );
extern void release_ioctl_window( struct ANativeWindow *window );
extern void destroy_ioctl_window( HWND hwnd, BOOL opengl );
extern int ioctl_window_pos_changed( HWND hwnd, const struct window_rects *rects,
                                     UINT style, UINT flags, HWND after, HWND owner );
extern int ioctl_set_window_parent( HWND hwnd, HWND parent );
extern int ioctl_set_capture( HWND hwnd );
extern int ioctl_set_cursor( int id, int width, int height,
                             int hotspotx, int hotspoty, const unsigned int *bits );
extern int ioctl_present_connect( HWND hwnd );


/**************************************************************************
 * Vulkan driver and swapchain presentation
 *
 * A client surface (a window's Vulkan surface) has a socket pair between the
 * Wine process and the host process. Swapchain images are AHardwareBuffers
 * the Wine process allocates and sends once; after that a frame is a message
 * naming the image, with the fence of its rendering. The host puts the buffer
 * on the window's layer and sends the image back, with the compositor's
 * release fence, when a later frame replaced it.
 */

struct vulkan_driver_funcs;
extern UINT ANDROID_VulkanInit( UINT version, void *vulkan_handle, const struct vulkan_driver_funcs **driver_funcs );

#define PRESENT_MAX_IMAGES 8

enum present_message_type
{
    PRESENT_CREATE,   /* to the host: a swapchain; its images follow, one AHardwareBuffer each */
    PRESENT_FRAME,    /* to the host: show an image; carries the fence of its rendering, if any */
    PRESENT_DESTROY,  /* to the host: the swapchain is gone */
    PRESENT_RELEASE,  /* from the host: the image is free; carries the fence to wait for, if any */
};

enum present_flags
{
    PRESENT_OPAQUE = 1,  /* the alpha channel means nothing */
    PRESENT_FIFO   = 2,  /* show every frame, in order */
};

struct present_message
{
    int type;
    int swapchain;
    int image;
    int count;
    int width;
    int height;
    int flags;
};

/* host process side */
extern int present_connect( HWND hwnd, struct ANativeWindow *window );
extern void present_surface_changed( HWND hwnd, struct ANativeWindow *window );
/* both sides: one message, with at most one file descriptor */
extern int present_send( int socket, const struct present_message *msg, int fd );
extern int present_recv( int socket, struct present_message *msg, int *fd, BOOL wait );


/**************************************************************************
 * USER driver
 */

extern pthread_mutex_t win_data_mutex;
extern INT ANDROID_GetKeyNameText( LONG lparam, LPWSTR buffer, INT size );
extern UINT ANDROID_MapVirtualKeyEx( UINT code, UINT maptype, HKL hkl );
extern SHORT ANDROID_VkKeyScanEx( WCHAR ch, HKL hkl );
extern void ANDROID_SetCursor( HWND hwnd, HCURSOR handle );
extern BOOL ANDROID_CreateDesktop( const WCHAR *name, UINT width, UINT height );
extern BOOL ANDROID_CreateWindow( HWND hwnd );
extern void ANDROID_SetDesktopWindow( HWND hwnd );
extern void ANDROID_DestroyWindow( HWND hwnd );
extern BOOL ANDROID_ProcessEvents( DWORD mask );
extern LRESULT ANDROID_DesktopWindowProc( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );
extern void ANDROID_SetParent( HWND hwnd, HWND parent, HWND old_parent );
extern void ANDROID_SetCapture( HWND hwnd, UINT flags );
extern UINT ANDROID_ShowWindow( HWND hwnd, INT cmd, RECT *rect, UINT swp );
extern LRESULT ANDROID_WindowMessage( HWND hwnd, UINT msg, WPARAM wp, LPARAM lp );
extern BOOL ANDROID_WindowPosChanging( HWND hwnd, UINT swp_flags, BOOL shaped, const struct window_rects *rects );
extern BOOL ANDROID_CreateWindowSurface( HWND hwnd, BOOL layered, const RECT *surface_rect, struct window_surface **surface );
extern void ANDROID_WindowPosChanged( HWND hwnd, HWND insert_after, HWND owner_hint, UINT swp_flags,
                                      const struct window_rects *new_rects, struct window_surface *surface );
extern ANativeWindow *get_client_window( HWND hwnd );
extern BOOL has_client_surface( HWND hwnd );

extern unsigned int screen_width;
extern unsigned int screen_height;
extern RECT virtual_screen_rect;
extern MONITORINFOEXW default_monitor;

enum android_window_messages
{
    WM_ANDROID_REFRESH = WM_WINE_FIRST_DRIVER_MSG,
};

extern void init_monitors( int width, int height );
extern void set_screen_dpi( DWORD dpi );
extern void update_keyboard_lock_state( WORD vkey, UINT state );

/* JNI entry points */
extern void looper_init( JNIEnv *env, jobject obj );
extern void wine_init_jni( JNIEnv *env, jobject obj );
extern void desktop_changed( JNIEnv *env, jobject obj, jint width, jint height );
extern void config_changed( JNIEnv *env, jobject obj, jint dpi );
extern void surface_changed( JNIEnv *env, jobject obj, jint win, jobject surface,
                             jboolean client );
extern jboolean motion_event( JNIEnv *env, jobject obj, jint win, jint action,
                              jint x, jint y, jint state, jint vscroll );
extern jboolean keyboard_event( JNIEnv *env, jobject obj, jint win, jint action,
                                jint keycode, jint state );

enum event_type
{
    DESKTOP_CHANGED,
    CONFIG_CHANGED,
    SURFACE_CHANGED,
    MOTION_EVENT,
    KEYBOARD_EVENT,
};

union event_data
{
    enum event_type type;
    struct
    {
        enum event_type type;
        unsigned int    width;
        unsigned int    height;
    } desktop;
    struct
    {
        enum event_type type;
        unsigned int    dpi;
    } cfg;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        BOOL            client;
        unsigned int    width;
        unsigned int    height;
    } surface;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        INPUT           input;
    } motion;
    struct
    {
        enum event_type type;
        HWND            hwnd;
        UINT            lock_state;
        INPUT           input;
    } kbd;
};

int send_event( const union event_data *data );

extern int event_source;

/* string helpers */

static inline void ascii_to_unicode( WCHAR *dst, const char *src, size_t len )
{
    while (len--) *dst++ = (unsigned char)*src++;
}

#endif  /* __WINE_ANDROID_H */
