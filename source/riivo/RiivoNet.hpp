/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Streams the boot log to a listener on the local network, one line at a
 * time, as it is written.
 *
 * Why this exists: the log on the card is only readable after the fact, and
 * the failures worth diagnosing are the ones where there is no "after" - the
 * console sits on a black screen and the card has to be pulled to learn
 * anything. A line already sent is a line that survives a hang, so the last
 * one received names the phase that stalled.
 *
 * Plaintext TCP on purpose. The HTTPS path in source/network exists for
 * fetching covers and needs wolfSSL, SNI and a 2 KB request buffer; none of
 * that earns its place for shipping text to a machine on the same LAN, and
 * a TLS handshake per line would cost more than the phases being measured.
 * Nothing secret is sent and nothing is stored in the binary: the listener's
 * address comes from riivolution/collector.txt on the card, so a build with
 * no such file never opens a socket.
 ***************************************************************************/
#ifndef RIIVO_NET_HPP_
#define RIIVO_NET_HPP_

#include <string>
#include "RiivoTypes.hpp"

namespace Riivo
{
	//! Split "10.0.0.82:7000" into address and port. Rejects an empty or
	//! malformed spec rather than guessing a default, because a typo that
	//! silently pointed at some other host would be worse than not sending.
	//! Surrounding whitespace and a trailing newline are tolerated: the file
	//! is written by hand in a text editor.
	bool ParseCollector(const std::string &spec, std::string &host, u16 &port);

	//! Connect, once. Does nothing without a spec or before the network is
	//! up, and gives up permanently on the first failure - a diagnostic aid
	//! must never be able to delay or block a boot it is only watching.
	void OpenCollector(const std::string &spec);

	//! Send text if connected. A failed write closes and goes quiet for the
	//! rest of the boot; the card log is still the record of truth.
	void SendCollector(const std::string &text);

	bool CollectorOpen();

	//! Flush and close. Called before the devices go down, so what is queued
	//! leaves while there is still an IOS to send it.
	void CloseCollector();
}

#endif
