/****************************************************************************
 * Thread-Safe Network Operations
 * for USB Loader GX
 *
 * HTTP operations
 * Based on dhewg/bushing, modified by dimok, made thread-safe by blackb0x
 ****************************************************************************/

#include <ogcsys.h>
#include <string.h>
#include <stdio.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

#include "networkops.h"
#include "https.h"
#include "update.h"
#include "gecko.h"
#include "settings/ProxySettings.h"

#define PORT 4299

/*** Incoming filesize ***/
u32 infilesize = 0;
u32 uncfilesize = 0;

s32 connection = -1;
static s32 socket = -1;
static bool networkinitialized = false;
static bool checkincomming = false;
static bool waitforanswer = false;
static char IP[16] = {0};
static char incommingIP[50] = {0};
char wiiloadVersion[2] = {0};

static lwp_t networkthread = LWP_THREAD_NULL;
static bool networkHalt = true;

// Mutex for all shared variables
static mutex_t net_mutex = LWP_MUTEX_NULL;

// Helper macros for locking
#define NET_LOCK() LWP_MutexLock(net_mutex)
#define NET_UNLOCK() LWP_MutexUnlock(net_mutex)

/****************************************************************************
 * Initialize_Network
 ***************************************************************************/
void Initialize_Network(int retries)
{
	NET_LOCK();
	if (networkinitialized)
	{
		NET_UNLOCK();
		return;
	}
	NET_UNLOCK();

	s32 result = if_config(IP, NULL, NULL, true, retries);

	NET_LOCK();
	if (result < 0)
	{
		networkinitialized = false;
	}
	else
	{
		getProxyInfo();
		wolfSSL_Init();
		networkinitialized = true;
		gprintf("Initialized network\n");
	}
	NET_UNLOCK();
}

/****************************************************************************
 * DeinitNetwork
 ***************************************************************************/
void DeinitNetwork(void)
{
	NET_LOCK();
	wolfSSL_Cleanup();
	net_wc24cleanup();
	net_deinit();
	networkinitialized = false;
	NET_UNLOCK();
}

/****************************************************************************
 * Check if network was initialised
 ***************************************************************************/
bool IsNetworkInit(void)
{
	NET_LOCK();
	bool ret = networkinitialized;
	NET_UNLOCK();
	return ret;
}

/****************************************************************************
 * Get network IP
 ***************************************************************************/
char *GetNetworkIP(void)
{
	NET_LOCK();
	static char ip_copy[16];
	strncpy(ip_copy, IP, sizeof(ip_copy));
	NET_UNLOCK();
	return ip_copy;
}

/****************************************************************************
 * Get incomming IP
 ***************************************************************************/
char *GetIncommingIP(void)
{
	NET_LOCK();
	static char ip_copy[50];
	strncpy(ip_copy, incommingIP, sizeof(ip_copy));
	NET_UNLOCK();
	return ip_copy;
}

/****************************************************************************
 * Read network data
 ***************************************************************************/
s32 network_read(s32 connect, u8 *buf, u32 len)
{
	NET_LOCK();
	s32 conn = (connect == NET_DEFAULT_SOCK) ? connection : connect;
	NET_UNLOCK();

	u32 read = 0;
	s32 ret = -1;

	while (read < len)
	{
		ret = net_read(conn, buf + read, len - read);
		if (ret < 0)
			return ret;
		if (!ret)
			break;
		read += ret;
	}
	return read;
}

/****************************************************************************
 * Close the connection
 ***************************************************************************/
void CloseConnection()
{
	NET_LOCK();
	if (connection >= 0)
	{
		net_close(connection);
		connection = -1;
	}
	if (waitforanswer && socket >= 0)
	{
		net_close(socket);
		socket = -1;
		waitforanswer = false;
	}
	NET_UNLOCK();
}

/****************************************************************************
 * NetworkWait
 ***************************************************************************/
int NetworkWait()
{
	NET_LOCK();
	if (!checkincomming)
	{
		NET_UNLOCK();
		return -3;
	}
	NET_UNLOCK();

	struct sockaddr_in sin;
	struct sockaddr_in client_address;
	socklen_t addrlen = sizeof(client_address);

	s32 local_socket = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (local_socket == INVALID_SOCKET)
		return local_socket;

	sin.sin_family = AF_INET;
	sin.sin_port = htons(PORT);
	sin.sin_addr.s_addr = htonl(INADDR_ANY);

	int flags = net_fcntl(local_socket, F_GETFL, 0);
	net_fcntl(local_socket, F_SETFL, flags | 4);


	if (net_bind(local_socket, (struct sockaddr *)&sin, sizeof(sin)) < 0)
	{
		net_close(local_socket);
		return -1;
	}

	if (net_listen(local_socket, 3) < 0)
	{
		net_close(local_socket);
		return -1;
	}

	s32 local_connection = net_accept(local_socket, (struct sockaddr *)&client_address, &addrlen);

	NET_LOCK();
	socket = local_socket;
	connection = local_connection;
	snprintf(incommingIP, sizeof(incommingIP), "%s", inet_ntoa(client_address.sin_addr));
	NET_UNLOCK();

	if (local_connection < 0)
	{
		net_close(local_socket);
		NET_LOCK();
		socket = -1;
		NET_UNLOCK();
		return -4;
	}
	else
	{
		unsigned char haxx[9];
		net_read(local_connection, &haxx, 8);
		NET_LOCK();
		wiiloadVersion[0] = haxx[4];
		wiiloadVersion[1] = haxx[5];
		NET_UNLOCK();

		net_read(local_connection, &infilesize, 4);

		if (haxx[4] > 0 || haxx[5] > 4)
			net_read(local_connection, &uncfilesize, 4);

		NET_LOCK();
		waitforanswer = true;
		checkincomming = false;
		networkHalt = true;
		NET_UNLOCK();
	}

	return 1;
}

/****************************************************************************
 * HaltNetwork
 ***************************************************************************/
void HaltNetworkThread()
{
	NET_LOCK();
	networkHalt = true;
	checkincomming = false;
	bool need_close = waitforanswer;
	NET_UNLOCK();

	if (need_close)
		CloseConnection();

	// wait for thread to finish
	gprintf("HaltNetworkThread\n");
	while (!LWP_ThreadIsSuspended(networkthread))
		usleep(100);

//	LWP_SuspendThread(networkthread);
}

/****************************************************************************
 * ResumeNetworkThread
 ***************************************************************************/
void ResumeNetworkThread()
{
	NET_LOCK();
	networkHalt = false;
	NET_UNLOCK();
	if (LWP_ThreadIsSuspended(networkthread))
		LWP_ResumeThread(networkthread);
}

/****************************************************************************
 * Resume NetworkWait
 ***************************************************************************/
void ResumeNetworkWait()
{
	NET_LOCK();
	networkHalt = true;
	checkincomming = true;
	waitforanswer = true;
	infilesize = 0;
	connection = -1;
	NET_UNLOCK();
	LWP_ResumeThread(networkthread);
}

/*********************************************************************************
 * Networkthread for background network initialize
 *********************************************************************************/
static void *networkinitcallback(void *arg)
{
	while (1)
	{
		NET_LOCK();
		bool halt = networkHalt;
		bool check = checkincomming;
		NET_UNLOCK();

		if (!check && halt)
			LWP_SuspendThread(networkthread);

		Initialize_Network(5);

		NET_LOCK();
		bool initialized = networkinitialized;
		NET_UNLOCK();

		if (initialized)
		{
			NET_LOCK();
			networkHalt = true;
			NET_UNLOCK();
		}

		NET_LOCK();
		check = checkincomming;
		NET_UNLOCK();

		if (check)
			NetworkWait();

		usleep(100000);
	}
	return NULL;
}

/****************************************************************************
 * InitNetworkThread with priority 0 (idle)
 ***************************************************************************/
void InitNetworkThread()
{
	if (net_mutex == LWP_MUTEX_NULL)
		LWP_MutexInit(&net_mutex, false);
	LWP_CreateThread(&networkthread, networkinitcallback, NULL, NULL, 16384, 0);
}

/****************************************************************************
 * ShutdownThread
 ***************************************************************************/
void ShutdownNetworkThread()
{
	LWP_JoinThread(networkthread, NULL);
	networkthread = LWP_THREAD_NULL;
	if (net_mutex != LWP_MUTEX_NULL)
	{
		LWP_MutexDestroy(net_mutex);
		net_mutex = LWP_MUTEX_NULL;
	}
}