/*
 * wineandroid.drv entry points
 *
 * Copyright 2022 Jacek Caban for CodeWeavers
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

#include <stdarg.h>
#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "winioctl.h"
#include "ddk/wdm.h"
#include "unixlib.h"
#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(android);


extern NTSTATUS CDECL wine_ntoskrnl_main_loop( HANDLE stop_event );
static HANDLE stop_event;


static NTSTATUS WINAPI ioctl_callback( DEVICE_OBJECT *device, IRP *irp )
{
    struct ioctl_params params = { .irp = irp, .client_id = HandleToUlong(PsGetCurrentProcessId()) };
    NTSTATUS status = ANDROID_CALL( dispatch_ioctl, &params );
    IoCompleteRequest( irp, IO_NO_INCREMENT );
    return status;
}

static NTSTATUS CALLBACK init_android_driver( DRIVER_OBJECT *driver, UNICODE_STRING *name )
{
    static const WCHAR device_nameW[] = {'\\','D','e','v','i','c','e','\\','W','i','n','e','A','n','d','r','o','i','d',0 };
    static const WCHAR device_linkW[] = {'\\','?','?','\\','W','i','n','e','A','n','d','r','o','i','d',0 };

    UNICODE_STRING nameW, linkW;
    DEVICE_OBJECT *device;
    NTSTATUS status;

    driver->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ioctl_callback;

    RtlInitUnicodeString( &nameW, device_nameW );
    RtlInitUnicodeString( &linkW, device_linkW );

    if ((status = IoCreateDevice( driver, 0, &nameW, 0, 0, FALSE, &device ))) return status;
    return IoCreateSymbolicLink( &linkW, &nameW );
}

static DWORD CALLBACK device_thread( void *arg )
{
    static const WCHAR driver_nameW[] = {'\\','D','r','i','v','e','r','\\','W','i','n','e','A','n','d','r','o','i','d',0 };

    HANDLE start_event = arg;
    UNICODE_STRING nameW;
    NTSTATUS status;
    DWORD ret;

    TRACE( "starting process %lx\n", GetCurrentProcessId() );

    status = ANDROID_CALL( java_init, NULL );
    ERR( "amphora device_thread java_init status=%lx\n", status );
    if (status) return 0;  /* not running under Java */

    RtlInitUnicodeString( &nameW, driver_nameW );
    if ((status = IoCreateDriver( &nameW, init_android_driver )))
    {
        ERR( "amphora device_thread IoCreateDriver status=%lx\n", status );
        FIXME( "failed to create driver error %lx\n", status );
        return status;
    }
    ERR( "amphora device_thread IoCreateDriver status=%lx\n", status );

    stop_event = CreateEventW( NULL, TRUE, FALSE, NULL );
    ERR( "amphora device_thread before SetEvent start_event=%p\n", start_event );
    SetEvent( start_event );
    ERR( "amphora device_thread after SetEvent start_event=%p\n", start_event );

    ERR( "amphora device_thread entered wine_ntoskrnl_main_loop stop_event=%p\n", stop_event );
    ret = wine_ntoskrnl_main_loop( stop_event );
    ERR( "amphora device_thread returned wine_ntoskrnl_main_loop ret=%lx\n", ret );

    ANDROID_CALL( java_uninit, NULL );
    return ret;
}

static NTSTATUS WINAPI android_start_device(void *param, ULONG size)
{
    HANDLE handles[2];
    DWORD wait_ret;

    handles[0] = CreateEventW( NULL, TRUE, FALSE, NULL );
    handles[1] = CreateThread( NULL, 0, device_thread, handles[0], 0, NULL );
    wait_ret = WaitForMultipleObjects( 2, handles, FALSE, INFINITE );
    ERR( "amphora android_start_device WaitForMultipleObjects woke wait_ret=%lu handles={%p,%p}\n",
         wait_ret, handles[0], handles[1] );
    CloseHandle( handles[0] );
    ERR( "amphora android_start_device before NtCallbackReturn thread=%p\n", handles[1] );
    return NtCallbackReturn( &handles[1], sizeof(handles[1]), STATUS_SUCCESS );
}


static void CALLBACK register_window_callback( ULONG_PTR arg1, ULONG_PTR arg2, ULONG_PTR arg3 )
{
    struct register_window_params params = { .arg1 = arg1, .arg2 = arg2, .arg3 = arg3 };
    ANDROID_CALL( register_window, &params );
}


/***********************************************************************
 *       dll initialisation routine
 */
BOOL WINAPI DllMain( HINSTANCE inst, DWORD reason, LPVOID reserved )
{
    struct init_params params;

    if (reason != DLL_PROCESS_ATTACH) return TRUE;

    DisableThreadLibraryCalls( inst );
    if (__wine_init_unix_call())
    {
        ERR( "unix lib not loaded\n" );
        return FALSE;
    }

    params.register_window_callback = register_window_callback;
    params.start_device_callback = (UINT_PTR)android_start_device;
    {
        NTSTATUS status = ANDROID_CALL( init, &params );
        if (status) ERR( "android_init failed %#lx\n", status );
        return !status;
    }
}
