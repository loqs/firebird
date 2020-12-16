/*************  history ************
*
*       COMPONENT: REMOTE       MODULE: SRVR_W32.CPP
*       generated by Marion V2.5     2/6/90
*       from dev              db        on 26-JAN-1996
*****************************************************************
*
*       20927   klaus   26-JAN-1996
*       Call ICS_enter at start
*
*       20858   klaus   17-JAN-1996
*       Get rid of extraneous header file
*
*       20841   klaus   12-JAN-1996
*       Add interprocess comm under remote component
*
*       20804   RCURRY  9-JAN-1996
*       Change priority for NP threads to normal
*
*       20768   klaus   20-DEC-1995
*       More xnet driver work
*
*       20729   klaus   8-DEC-1995
*       Begin adding xnet protocol
*
*       20716   jmayer  6-DEC-1995
*       Update to not show NamedPipes as supported on Win95.
*
*       20690   jmayer  4-DEC-1995
*       Change to start the IPC protocol when running as a service.
*
*       20682   jmayer  3-DEC-1995
*       Update to write to logfile and display msg box as a non-service.
*
*       20373   RCURRY  24-OCT-1995
*       Fix bug with license checking
*
*       20359   RMIDEKE 23-OCT-1995
*       add a semicollin
*
*       20356   rcurry  23-OCT-1995
*       Add more license file checking
*
*       20350   RCURRY  20-OCT-1995
*       add license file checking for remote protocols
*
*       20281   RCURRY  13-OCT-1995
*       fix multi thread scheduler problem
*
*       20198   RCURRY  27-SEP-1995
*       Make Windows95 and Windows NT have the same defaults
*
 * The contents of this file are subject to the Interbase Public
 * License Version 1.0 (the "License"); you may not use this file
 * except in compliance with the License. You may obtain a copy
 * of the License at http://www.Inprise.com/IPL.html
 *
 * Software distributed under the License is distributed on an
 * "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, either express
 * or implied. See the License for the specific language governing
 * rights and limitations under the License.
 *
 * The Original Code was created by Inprise Corporation
 * and its predecessors. Portions created by Inprise Corporation are
 * Copyright (C) Inprise Corporation.
 *
 * All Rights Reserved.
 * Contributor(s): ______________________________________.
 *
 * 2001.11.20: Claudio Valderrama: Honor -b in SS for high priority.
 *
*/


/*
 *      PROGRAM:        JRD Remote Server
 *      MODULE:         srvr_w32.cpp
 *      DESCRIPTION:    Windows NT remote server.
 *
 * copyright (c) 1993, 1996 by Borland International
 */

#include "firebird.h"
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

#include "fb_exception.h"
#include "gen/iberror.h"
#include "../common/ThreadStart.h"
#include "../common/os/fbsyslog.h"
#include "../common/isc_proto.h"
#include "../common/os/isc_i_proto.h"
#include "../common/config/config.h"
#include "../common/utils_proto.h"
#include "../common/classes/semaphore.h"
#include "../common/classes/FpeControl.h"
#include "../jrd/license.h"
#include "../utilities/install/install_nt.h"
#include "../remote/remote.h"
#include "../remote/server/os/win32/cntl_proto.h"
#include "../remote/inet_proto.h"
#include "../remote/server/serve_proto.h"
#include "../remote/server/ReplServer.h"
#include "../remote/server/os/win32/window_proto.h"
#include "../remote/os/win32/wnet_proto.h"
#include "../remote/server/os/win32/window.rh"
#include "../remote/os/win32/xnet_proto.h"
#include "../yvalve/gds_proto.h"

#include "firebird/Interface.h"
#include "../common/classes/ImplementHelper.h"
#include "../common/os/os_utils.h"
#include "../common/status.h"
#include "../auth/trusted/AuthSspi.h"
#include "../auth/SecurityDatabase/LegacyServer.h"
#include "../auth/SecureRemotePassword/server/SrpServer.h"


static THREAD_ENTRY_DECLARE inet_connect_wait_thread(THREAD_ENTRY_PARAM);
static THREAD_ENTRY_DECLARE wnet_connect_wait_thread(THREAD_ENTRY_PARAM);
static THREAD_ENTRY_DECLARE xnet_connect_wait_thread(THREAD_ENTRY_PARAM);
static THREAD_ENTRY_DECLARE start_connections_thread(THREAD_ENTRY_PARAM);
static THREAD_ENTRY_DECLARE process_connection_thread(THREAD_ENTRY_PARAM);
static HANDLE parse_args(LPCSTR, USHORT*);
static void service_connection(rem_port*);
static int wait_threads(const int reason, const int mask, void* arg);

static HINSTANCE hInst;

static TEXT protocol_inet[128];
static TEXT protocol_wnet[128];
static TEXT instance[MAXPATHLEN];
static USHORT server_flag = 0;
static bool server_shutdown = false;

using namespace Firebird;

class ThreadCounter
{
public:
	ThreadCounter()
	{
		++m_count;
	}

	~ThreadCounter()
	{
		--m_count;
		m_semaphore.release();
	}

	static bool wait()
	{
		while (m_count.value() > 0)
		{
			if (!m_semaphore.tryEnter(10))
				break;
		}

		return (m_count.value() == 0);
	}

private:
	static AtomicCounter m_count;
	static Semaphore m_semaphore;
};

AtomicCounter ThreadCounter::m_count;
Semaphore ThreadCounter::m_semaphore;


int WINAPI WinMain(HINSTANCE hThisInst, HINSTANCE /*hPrevInst*/, LPSTR lpszArgs, int nWndMode)
{
/**************************************
 *
 *      W i n M a i n
 *
 **************************************
 *
 * Functional description
 *      Run the server with NT named
 *      pipes and/or TCP/IP sockets.
 *
 **************************************/
	hInst = hThisInst;

	// We want server to crash without waiting for feedback from the user
	try
	{
		if (!Config::getBugcheckAbort())
			SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
	}
	catch (fatal_exception& e)
	{
		MessageBox(NULL, e.what(), "Firebird server failure",
			MB_OK | MB_ICONHAND | MB_SYSTEMMODAL  | MB_DEFAULT_DESKTOP_ONLY);
		return STARTUP_ERROR; // see /common/common.h
	}
	catch (status_exception& e)
	{
		TEXT buffer[BUFFER_LARGE];
		const ISC_STATUS* vector = e.value();
		if (! (vector && fb_interpret(buffer, sizeof(buffer), &vector)))
			strcpy(buffer, "Unknown internal failure");

		MessageBox(NULL, buffer, "Firebird server failure",
			MB_OK | MB_ICONHAND | MB_SYSTEMMODAL  | MB_DEFAULT_DESKTOP_ONLY);
		return STARTUP_ERROR; // see /common/common.h
	}

	// Check for missing firebird.conf
	if (Config::missFirebirdConf())
	{
		const char* anyError = "Missing master config file firebird.conf";
		Syslog::Record(Syslog::Error, anyError);
		MessageBox(NULL, anyError, "Firebird server failure",
			MB_OK | MB_ICONHAND | MB_SYSTEMMODAL  | MB_DEFAULT_DESKTOP_ONLY);
		return STARTUP_ERROR;
	}

	const DWORD_PTR affinity = Config::getCpuAffinityMask();
	if (affinity)
		SetProcessAffinityMask(GetCurrentProcess(), affinity);

	protocol_inet[0] = 0;
	protocol_wnet[0] = 0;

	strcpy(instance, FB_DEFAULT_INSTANCE);

	if (Config::getServerMode() != MODE_CLASSIC)
		server_flag = SRVR_multi_client;

	const HANDLE connection_handle = parse_args(lpszArgs, &server_flag);

	// get priority class from the config file
	int priority = Config::getProcessPriorityLevel();

	// override it, if necessary
	if (server_flag & SRVR_high_priority) {
		priority = 1;
	}

	// set priority class
	if (priority > 0) {
		SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
	}
	else if (priority < 0) {
		SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
	}

	{
		Firebird::MasterInterfacePtr master;
		master->serverMode(server_flag & SRVR_multi_client ? 1 : 0);
	}

	TEXT mutex_name[MAXPATHLEN];
	fb_utils::snprintf(mutex_name, sizeof(mutex_name), SERVER_MUTEX, instance);
	fb_utils::prefix_kernel_object_name(mutex_name, sizeof(mutex_name));
	CreateMutex(ISC_get_security_desc(), FALSE, mutex_name);

	// Initialize the service

	ISC_signal_init();
	FpeControl::maskAll();

	int nReturnValue = 0;
	ISC_STATUS_ARRAY status_vector;
	fb_utils::init_status(status_vector);

	{ // scope for interface ptr
		PluginManagerInterfacePtr pi;
		//Auth::registerLegacyServer(pi);
		Auth::registerSrpServer(pi);
#ifdef TRUSTED_AUTH
		Auth::registerTrustedServer(pi);
#endif
	}

	fb_shutdown_callback(0, wait_threads, fb_shut_finish, NULL);

	if (connection_handle != INVALID_HANDLE_VALUE)
	{
		rem_port* port = 0;

		try
		{
			if (server_flag & SRVR_inet)
			{
				port = INET_reconnect((SOCKET) connection_handle);

				if (port)
				{
					SRVR_multi_thread(port, server_flag);
					port = NULL;
				}
			}
			else if (server_flag & SRVR_wnet)
				port = WNET_reconnect(connection_handle);
			else if (server_flag & SRVR_xnet)
				port = XNET_reconnect((ULONG_PTR) connection_handle);

			if (port)
				service_connection(port);
		}
		catch (const Exception& ex)
		{
			iscLogException("Server error", ex);
		}

		fb_shutdown(5 * 1000 /*5 seconds*/, fb_shutrsn_no_connection);
	}
	else if (!(server_flag & SRVR_non_service))
	{
		string service_name;
		service_name.printf(REMOTE_SERVICE, instance);

		CNTL_init(start_connections_thread, instance);

		const SERVICE_TABLE_ENTRY service_table[] =
		{
			{const_cast<char*>(service_name.c_str()), CNTL_main_thread},
			{NULL, NULL}
		};

		// BRS There is a error in MinGW (3.1.0) headers
		// the parameter of StartServiceCtrlDispatcher is declared const in msvc headers
#if defined(MINGW)
		if (!StartServiceCtrlDispatcher(const_cast<SERVICE_TABLE_ENTRY*>(service_table)))
		{
#else
		if (!StartServiceCtrlDispatcher(service_table))
		{
#endif
			if (GetLastError() != ERROR_CALL_NOT_IMPLEMENTED) {
				CNTL_shutdown_service("StartServiceCtrlDispatcher failed");
			}
			server_flag |= SRVR_non_service;
		}
	}
	else
	{
		start_connections_thread(0);
		nReturnValue = WINDOW_main(hThisInst, nWndMode, server_flag);
	}

#ifdef DEBUG_GDS_ALLOC
	// In Debug mode - this will report all server-side memory leaks
	// due to remote access

	PathName name = fb_utils::getPrefix(IConfigManager::DIR_LOG, "memdebug.log");
	FILE* file = os_utils::fopen(name.c_str(), "w+t");
	if (file)
	{
		fprintf(file, "Global memory pool allocated objects\n");
		getDefaultMemoryPool()->print_contents(file);
		fclose(file);
	}
#endif

	return nReturnValue;
}


THREAD_ENTRY_DECLARE process_connection_thread(THREAD_ENTRY_PARAM arg)
{
/**************************************
 *
 *      p r o c e s s _ c o n n e c t i o n _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	ThreadCounter counter;

	service_connection((rem_port*) arg);
	return 0;
}


static THREAD_ENTRY_DECLARE inet_connect_wait_thread(THREAD_ENTRY_PARAM)
{
/**************************************
 *
 *      i n e t _ c o n n e c t _ w a i t _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	ThreadCounter counter;

	while (!server_shutdown)
	{
		rem_port* port = NULL;
		try
		{
			port = INET_connect(protocol_inet, NULL, server_flag, 0, NULL);
		}
		catch (const Exception& ex)
		{
			iscLogException("INET_connect", ex);
		}

		if (!port)
			break;

		if (server_flag & SRVR_multi_client)
		{
			SRVR_multi_thread(port, server_flag);
			break;
		}

		try
		{
			Thread::start(process_connection_thread, port, THREAD_medium);
		}
		catch (const Exception& )
		{
			gds__log("INET: can't start worker thread, connection terminated");
			port->disconnect(NULL, NULL);
		}
	}
	return 0;
}


static THREAD_ENTRY_DECLARE wnet_connect_wait_thread(THREAD_ENTRY_PARAM)
{
/**************************************
 *
 *      w n e t _ c o n n e c t _ w a i t _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	ThreadCounter counter;

	while (!server_shutdown)
	{
		rem_port* port = NULL;

		try
		{
			port = WNET_connect(protocol_wnet, NULL, server_flag, NULL);
		}
		catch (const Exception& ex)
		{
			SimpleStatusVector<> status_vector;
			ex.stuffException(status_vector);

			if (status_vector[1] == isc_net_server_shutdown)
				break;

			iscLogException("WNET_connect", ex);
		}

		if (port)
		{
			try
			{
				Thread::start(process_connection_thread, port, THREAD_medium);
			}
			catch (const Exception&)
			{
				gds__log("WNET: can't start worker thread, connection terminated");
				port->disconnect(NULL, NULL);
			}
		}
	}

	return 0;
}


static THREAD_ENTRY_DECLARE xnet_connect_wait_thread(THREAD_ENTRY_PARAM)
{
/**************************************
 *
 *      x n e t _ c o n n e c t _ w a i t _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *   Starts xnet server side interprocess thread
 *
 **************************************/
	ThreadCounter counter;

	while (!server_shutdown)
	{
		rem_port* port = NULL;

		try
		{
			port = XNET_connect(NULL, server_flag, NULL);
		}
		catch (const Exception& ex)
		{
			SimpleStatusVector<> status_vector;
			ex.stuffException(status_vector);

			if (status_vector[1] == isc_net_server_shutdown)
				break;

			iscLogException("XNET_connect", ex);
		}

		if (port)
		{
			try
			{
				Thread::start(process_connection_thread, port, THREAD_medium);
			}
			catch (const Exception&)
			{
				gds__log("XNET: can't start worker thread, connection terminated");
				port->disconnect(NULL, NULL);
			}
		}
	}

	return 0;
}


static void service_connection(rem_port* port)
{
/**************************************
 *
 *      s e r v i c e _ c o n n e c t i o n
 *
 **************************************
 *
 * Functional description
 *
 **************************************/

	SRVR_main(port, server_flag & ~SRVR_multi_client);
}


static THREAD_ENTRY_DECLARE start_connections_thread(THREAD_ENTRY_PARAM)
{
/**************************************
 *
 *      s t a r t _ c o n n e c t i o n s _ t h r e a d
 *
 **************************************
 *
 * Functional description
 *
 **************************************/
	ThreadCounter counter;
	FbLocalStatus localStatus;

	if (server_flag & SRVR_inet)
	{
		try
		{
			Thread::start(inet_connect_wait_thread, 0, THREAD_medium);
		}
		catch (const Exception& ex)
		{
			iscLogException("INET: can't start listener thread", ex);
		}
	}

	if (server_flag & SRVR_wnet)
	{
		try
		{
			Thread::start(wnet_connect_wait_thread, 0, THREAD_medium);
		}
		catch (const Exception& ex)
		{
			iscLogException("WNET: can't start listener thread", ex);
		}
	}

	if (server_flag & SRVR_xnet)
	{
		try
		{
			Thread::start(xnet_connect_wait_thread, 0, THREAD_medium);
		}
		catch (const Exception& ex)
		{
			iscLogException("XNET: can't start listener thread", ex);
		}
	}

	REPL_server(&localStatus, false, &server_shutdown);

	return 0;
}


static HANDLE parse_args(LPCSTR lpszArgs, USHORT* pserver_flag)
{
/**************************************
 *
 *      p a r s e _ a r g s
 *
 **************************************
 *
 * Functional description
 *      WinMain gives us a stupid command string, not
 *      a cool argv.  Parse through the string and
 *      set the options.
 * Returns
 *      a connection handle if one was passed in,
 *      INVALID_HANDLE_VALUE otherwise.
 *
 **************************************/
	bool delimited = false;

	HANDLE connection_handle = INVALID_HANDLE_VALUE;

	const TEXT* p = lpszArgs;
	while (*p)
	{
		TEXT c;
		if (*p++ == '-')
		{
			while ((*p) && (c = *p++) && (c != ' '))
			{
				switch (UPPER(c))
				{
				case 'A':
					*pserver_flag |= SRVR_non_service;
					break;

				case 'B':
					*pserver_flag |= SRVR_high_priority;
					break;

				case 'D':
					*pserver_flag |= (SRVR_debug | SRVR_non_service);
					break;

				case 'H':
					while (*p && *p == ' ')
						p++;
					if (*p)
					{
						TEXT buffer[32];
						char* pp = buffer;
						while (*p && *p != ' ' && (pp - buffer < sizeof(buffer) - 1))
						{
							if (*p == '@')
							{
								p++;
								*pp++ = '\0';
								connection_handle = (HANDLE) _atoi64(buffer);
								pp = buffer;
							}
							else
								*pp++ = *p++;
						}
						*pp++ = '\0';

						if (connection_handle == INVALID_HANDLE_VALUE)
						{
							connection_handle = (HANDLE) _atoi64(buffer);
						}
						else
						{
							const DWORD parent_id = atol(buffer);
							const HANDLE parent_handle = OpenProcess(PROCESS_DUP_HANDLE, FALSE, parent_id);
							if (!parent_handle)
							{
								gds__log("SERVER: OpenProcess failed. Errno = %d, parent PID = %d", GetLastError(), parent_id);
								exit(FINI_ERROR);
							}

							if (!DuplicateHandle(parent_handle, connection_handle, GetCurrentProcess(), &connection_handle,
									0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE))
							{
								gds__log("SERVER: DuplicateHandle failed. Errno = %d, parent PID = %d", GetLastError(), parent_id);
								exit(FINI_ERROR);
							}

							CloseHandle(parent_handle);
						}
					}
					break;

				case 'I':
					*pserver_flag |= SRVR_inet;
					break;

				case 'N':
					*pserver_flag |= SRVR_no_icon;
					break;

				case 'P':	// Specify a port or named pipe other than the default
					while (*p && *p == ' ')
						p++;
					if (*p)
					{
						// Assumed the buffer size for both protocols may differ
						// in the future, hence I did generic code.
						char* pi = protocol_inet;
						const char* piend = protocol_inet + sizeof(protocol_inet) - 1;
						char* pw = protocol_wnet;
						const char* pwend = protocol_wnet + sizeof(protocol_wnet) - 1;

						*pi++ = '/';
						*pw++ = '\\';
						*pw++ = '\\';
						*pw++ = '.';
						*pw++ = '@';
						while (*p && *p != ' ')
						{
							if (pi < piend)
								*pi++ = *p;
							if (pw < pwend)
								*pw++ = *p++;
						}
						*pi++ = '\0';
						*pw++ = '\0';
					}
					break;

				case 'R':
					*pserver_flag &= ~SRVR_high_priority;
					break;

				case 'S':
					delimited = false;
					while (*p && *p == ' ')
						p++;
					if (*p && *p == '"')
					{
						p++;
						delimited = true;
					}
					if (delimited)
					{
						char* pi = instance;
						const char* pend = instance + sizeof(instance) - 1;
						while (*p && *p != '"' && pi < pend) {
							*pi++ = *p++;
						}
						*pi++ = '\0';
						if (*p && *p == '"')
							p++;
					}
					else
					{
						if (*p && *p != '-')
						{
							char* pi = instance;
							const char* pend = instance + sizeof(instance) - 1;
							while (*p && *p != ' ' && pi < pend) {
								*pi++ = *p++;
							}
							*pi++ = '\0';
						}
					}
					break;

				case 'W':
					*pserver_flag |= SRVR_wnet;
					break;

				case 'X':
					*pserver_flag |= SRVR_xnet;
					break;

				case 'Z':
					// CVC: printf doesn't work because we don't have a console attached.
					//printf("Firebird remote server version %s\n",  FB_VERSION);
					MessageBox(NULL, FB_VERSION, "Firebird server version",
						MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_DEFAULT_DESKTOP_ONLY);
					exit(FINI_OK);

				default:
					// In case of something unrecognized, just
					// continue, since we have already taken it off
					// of p.
					break;
				}
			}
		}
	}

	if ((*pserver_flag & (SRVR_inet | SRVR_wnet | SRVR_xnet)) == 0)
	{
		*pserver_flag |= SRVR_wnet;
		*pserver_flag |= SRVR_inet;
		*pserver_flag |= SRVR_xnet;
	}

	return connection_handle;
}

static int wait_threads(const int, const int, void*)
{
	server_shutdown = true;

	if (!ThreadCounter::wait()) {
		gds__log("Timeout expired during remote server shutdown");
	}

	return FB_SUCCESS;
}
