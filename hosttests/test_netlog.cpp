// The collector address, as typed by hand into a file on the card.
//
// The socket half needs a console and is not reachable here. This is the half
// that fails quietly: a spec that parses to the wrong thing sends a boot log
// to some other machine, or opens nothing and says nothing. Every rejection
// below is a case where guessing would have been worse than refusing.
#include <stdio.h>
#include <string>
#include "riivo/RiivoNet.hpp"

using namespace Riivo;

static int checks, failed;
static void ck(bool ok, const char *name)
{
	++checks;
	if (!ok)
	{
		++failed;
		printf("FAIL: %s\n", name);
	}
}

static bool accepts(const char *spec, const char *wantHost, unsigned wantPort)
{
	std::string h;
	u16 p = 0;
	if (!ParseCollector(spec, h, p))
		return false;
	return h == wantHost && p == (u16) wantPort;
}

static bool rejects(const char *spec)
{
	std::string h;
	u16 p = 0;
	if (ParseCollector(spec, h, p))
		return false;
	// A refusal must not leave half an answer behind for a caller to use.
	return h.empty() && p == 0;
}

int main()
{
	ck(accepts("10.0.0.82:7000", "10.0.0.82", 7000), "plain address and port");
	ck(accepts("192.168.1.5:1", "192.168.1.5", 1), "lowest port");
	ck(accepts("192.168.1.5:65535", "192.168.1.5", 65535), "highest port");

	// The file is written in a text editor, so it arrives with a newline and
	// possibly indentation. Refusing those would look like a broken feature.
	ck(accepts("10.0.0.82:7000\n", "10.0.0.82", 7000), "trailing newline");
	ck(accepts("10.0.0.82:7000\r\n", "10.0.0.82", 7000), "trailing CRLF");
	ck(accepts("  10.0.0.82:7000  ", "10.0.0.82", 7000), "surrounding spaces");
	ck(accepts("\t10.0.0.82:7000\t\n", "10.0.0.82", 7000), "tabs");

	ck(rejects(""), "empty");
	ck(rejects("   \n"), "whitespace only");
	ck(rejects("10.0.0.82"), "no port");
	ck(rejects("10.0.0.82:"), "empty port");
	ck(rejects(":7000"), "empty address");
	ck(rejects("10.0.0.82:0"), "port zero");
	ck(rejects("10.0.0.82:65536"), "port past the range");
	ck(rejects("10.0.0.82:99999999"), "port far past the range");

	// strtol would take these; the loop does not.
	ck(rejects("10.0.0.82:70a0"), "letters inside the port");
	ck(rejects("10.0.0.82:7000junk"), "trailing junk after the port");
	ck(rejects("10.0.0.82:-1"), "negative port");
	ck(rejects("10.0.0.82: 7000"), "space inside the port");

	// inet_addr takes a dotted quad. A hostname would parse as an address and
	// then fail to connect for a reason nobody could read.
	ck(rejects("localhost:7000"), "hostname");
	ck(rejects("my-pc.local:7000"), "hostname with dots and a dash");
	ck(rejects("10.0.0:7000"), "three parts");
	ck(rejects("10.0.0.82.5:7000"), "five parts");

	printf("%d checks, %d failure(s)\n", checks, failed);
	return failed ? 1 : 0;
}
