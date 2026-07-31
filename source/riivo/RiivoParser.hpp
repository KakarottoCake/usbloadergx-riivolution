/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Parse a Riivolution XML (via pugixml) into the Riivo::Disc data model.
 * Port of riivultimatum/riivo/parser.py.
 ***************************************************************************/
#ifndef RIIVO_PARSER_HPP_
#define RIIVO_PARSER_HPP_

#include "RiivoTypes.hpp"

namespace Riivo
{
	//! Parse the XML at `xmlPath` into `out`.
	//! Returns true on success; on failure returns false and, if `error` is
	//! non-NULL, fills it with a human-readable reason.
	bool ParseFile(const char *xmlPath, Disc &out, std::string *error = 0);
}

#endif
