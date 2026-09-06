/****************************************************************************
 * Riivolution support for USB Loader GX
 ***************************************************************************/
#include <string.h>
#include <stdio.h>
#include "RiivoNet.hpp"
#include "gecko.h"

#ifndef RIIVO_HOST_TEST
#include "RiivoNetSock.h"
#include "network/networkops.h"
#endif

namespace Riivo
{
	static int g_sock = -1;
	//! Latched on the first failure. Retrying per line would turn a listener
	//! that is not there into a connect attempt for every step of the boot.
	static bool g_dead = false;

	bool ParseCollector(const std::string &spec, std::string &host, u16 &port)
	{
		host.clear();
		port = 0;

		size_t b = spec.find_first_not_of(" \t\r\n");
		if (b == std::string::npos)
			return false;
		size_t e = spec.find_last_not_of(" \t\r\n");
		const std::string s = spec.substr(b, e - b + 1);

		//! Rightmost colon: an address never contains one here (IPv4 only,
		//! because that is what net_connect below takes), but being explicit
		//! costs nothing and documents the assumption.
		const size_t colon = s.find_last_of(':');
		if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size())
			return false;

		const std::string h = s.substr(0, colon);
		const std::string p = s.substr(colon + 1);

		//! Digits only, and inside the port range. strtol would accept
		//! "7000junk" and a negative sign.
		unsigned long v = 0;
		for (size_t i = 0; i < p.size(); ++i)
		{
			if (p[i] < '0' || p[i] > '9')
				return false;
			v = v * 10 + (unsigned long) (p[i] - '0');
			if (v > 65535)
				return false;
		}
		if (v == 0)
			return false;

		//! A dotted quad, checked loosely: four parts, digits and dots only.
		//! net_connect rejects anything malformed anyway; this only keeps an
		//! obvious typo (a hostname) from looking like it worked.
		int dots = 0;
		for (size_t i = 0; i < h.size(); ++i)
		{
			if (h[i] == '.')
				++dots;
			else if (h[i] < '0' || h[i] > '9')
				return false;
		}
		if (dots != 3)
			return false;

		host = h;
		port = (u16) v;
		return true;
	}

	bool CollectorOpen()
	{
		return g_sock >= 0;
	}

#ifdef RIIVO_HOST_TEST

	//! The socket half needs a console; the parsing and the give-up policy do
	//! not, and those are what can be got wrong quietly.
	void OpenCollector(const std::string &spec)
	{
		std::string host;
		u16 port = 0;
		if (spec.empty() || g_dead || !ParseCollector(spec, host, port))
			g_dead = true;
	}
	void SendCollector(const std::string &) {}
	void CloseCollector() { g_sock = -1; g_dead = false; }

#else

	void OpenCollector(const std::string &spec)
	{
		if (g_sock >= 0 || g_dead)
			return;

		std::string host;
		u16 port = 0;
		if (spec.empty())
		{
			g_dead = true;
			return;
		}
		if (!ParseCollector(spec, host, port))
		{
			gprintf("Riivo net: collector spec is not addr:port, ignored\n");
			g_dead = true;
			return;
		}

		//! The network thread is started at menu entry. If it has not come
		//! up there is nothing to wait for here, and waiting is exactly what
		//! a diagnostic aid must never do.
		if (!IsNetworkInit())
		{
			gprintf("Riivo net: network is not up, log streaming off\n");
			g_dead = true;
			return;
		}

		const int sock = RiivoSockOpen(host.c_str(), port);
		if (sock < 0)
		{
			//! Nothing listening is the normal case for anyone who left a
			//! collector.txt behind. Say so once and never try again.
			gprintf("Riivo net: no collector at %s:%u (%d), streaming off\n",
					host.c_str(), (unsigned) port, sock);
			g_dead = true;
			return;
		}

		g_sock = sock;
		gprintf("Riivo net: streaming the boot log to %s:%u\n",
				host.c_str(), (unsigned) port);
	}

	void SendCollector(const std::string &text)
	{
		if (g_sock < 0 || text.empty())
			return;
		if (RiivoSockWrite(g_sock, text.data(), (unsigned int) text.size()) < 0)
		{
			//! The listener went away mid-boot. The card log still has
			//! everything; drop the socket rather than retry into a
			//! connection that is gone.
			gprintf("Riivo net: collector write failed, streaming off\n");
			RiivoSockClose(g_sock);
			g_sock = -1;
			g_dead = true;
		}
	}

	void CloseCollector()
	{
		if (g_sock < 0)
			return;
		RiivoSockClose(g_sock);
		g_sock = -1;
	}

#endif
}
