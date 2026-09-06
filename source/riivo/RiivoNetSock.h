/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * The three socket calls RiivoNet needs, behind a C seam.
 *
 * Why the seam: <network.h> and the repository's bundled portlibs headers
 * both define socklen_t and struct sockaddr_storage. C accepts the identical
 * typedef twice; C++ does not, and rejects the file outright. Every other
 * socket user in this tree (https.c, networkops.cpp via ogcsys) is C or
 * avoids the header, so this file is C for exactly the same reason.
 ***************************************************************************/
#ifndef RIIVO_NET_SOCK_H_
#define RIIVO_NET_SOCK_H_

#ifdef __cplusplus
extern "C"
{
#endif

	//! Connect to a dotted-quad address. Returns a socket, or < 0 on any
	//! failure, including the network not being up.
	int RiivoSockOpen(const char *dottedQuad, unsigned short port);

	//! Write all of `len`. Returns 0 on success, < 0 if the peer went away.
	int RiivoSockWrite(int sock, const char *data, unsigned int len);

	void RiivoSockClose(int sock);

#ifdef __cplusplus
}
#endif

#endif
