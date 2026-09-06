/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <network.h>
#include <string.h>

#include "RiivoNetSock.h"

int RiivoSockOpen(const char *dottedQuad, unsigned short port)
{
	struct sockaddr_in sin;
	s32 sock, ret;

	if (!dottedQuad || !*dottedQuad || port == 0)
		return -1;

	/* Whether the network is up is decided by the caller: IsNetworkInit is
	   declared in a C++ header (default arguments, bool), so it cannot be
	   reached from here. This file is sockets and nothing else. */

	sock = net_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (sock < 0)
		return (int) sock;

	memset(&sin, 0, sizeof(sin));
	sin.sin_family = AF_INET;
	sin.sin_port = htons(port);
	sin.sin_addr.s_addr = inet_addr(dottedQuad);

	ret = net_connect(sock, (struct sockaddr *) &sin, sizeof(sin));
	if (ret < 0)
	{
		net_close(sock);
		return (int) ret;
	}
	return (int) sock;
}

int RiivoSockWrite(int sock, const char *data, unsigned int len)
{
	unsigned int sent = 0;

	if (sock < 0 || !data)
		return -1;

	while (sent < len)
	{
		s32 n = net_write(sock, data + sent, (s32)(len - sent));
		if (n <= 0)
			return (int) (n == 0 ? -1 : n);
		sent += (unsigned int) n;
	}
	return 0;
}

void RiivoSockClose(int sock)
{
	if (sock >= 0)
		net_close(sock);
}
