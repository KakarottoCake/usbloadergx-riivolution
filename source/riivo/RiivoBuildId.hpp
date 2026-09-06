/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Build identity for the boot log. The Makefile bakes the source commit in
 * with -DRIIVO_COMMIT (falling back to "unknown" outside a git checkout, so
 * the flag can never fail a build); __DATE__ names when the binary was
 * built. Together they pin a tester binary to its source - a package
 * filename or meta.xml version alone cannot do that.
 ***************************************************************************/
#ifndef RIIVO_BUILD_ID_HPP_
#define RIIVO_BUILD_ID_HPP_

#include <string>

#ifndef RIIVO_COMMIT
#define RIIVO_COMMIT "unknown"
#endif

namespace Riivo
{
	inline std::string BuildId()
	{
		std::string id = RIIVO_COMMIT;
		id += " ";
		id += __DATE__;
		return id;
	}
}

#endif
