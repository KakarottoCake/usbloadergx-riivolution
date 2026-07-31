/****************************************************************************
 * Riivolution support for USB Loader GX
 *
 * Port of riivultimatum/riivo/parser.py onto pugixml. Kept deliberately close
 * to the Python/Dolphin structure so the two can be diffed by eye.
 ***************************************************************************/
#include <ctype.h>
#include <stdlib.h>
#include "RiivoParser.hpp"
#include "xml/pugixml.hpp"

namespace Riivo
{
	// --------------------------------------------------------------------
	// Attribute value parsing (mirrors parse_bool / parse_int / parse_hex_string)
	// --------------------------------------------------------------------

	static bool ParseBool(const pugi::xml_attribute &attr, bool def)
	{
		if (!attr)
			return def;
		std::string v = attr.value();
		// lower-case in place
		for (size_t i = 0; i < v.size(); ++i)
			v[i] = (char)tolower((unsigned char)v[i]);
		if (v == "true" || v == "yes" || v == "1")
			return true;
		if (v == "false" || v == "no" || v == "0")
			return false;
		return def; // permissive, like Riivolution itself
	}

	//! Decimal, or hex with an 0x/-0x prefix. Returned as u32 (addresses/lengths).
	static u32 ParseInt(const pugi::xml_attribute &attr, u32 def = 0)
	{
		if (!attr)
			return def;
		const char *s = attr.value();
		while (*s == ' ' || *s == '\t')
			++s;
		if (*s == 0)
			return def;

		bool neg = false;
		if (*s == '-') { neg = true; ++s; }
		else if (*s == '+') { ++s; }

		u32 val = 0;
		if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
			val = (u32)strtoul(s, 0, 16);
		else
			val = (u32)strtoul(s, 0, 10);

		return neg ? (u32)(-(s32)val) : val;
	}

	static int HexDigit(char c)
	{
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
	}

	//! Parse a hex string into bytes. Matches Dolphin's ReadHexString leniency:
	//! optional 0x prefix, and an odd digit count yields empty rather than error.
	static std::vector<u8> ParseHexString(const pugi::xml_attribute &attr)
	{
		std::vector<u8> out;
		if (!attr)
			return out;
		std::string v = attr.value();
		// trim
		size_t a = 0, b = v.size();
		while (a < b && (v[a] == ' ' || v[a] == '\t')) ++a;
		while (b > a && (v[b - 1] == ' ' || v[b - 1] == '\t')) --b;
		v = v.substr(a, b - a);
		if (v.size() >= 2 && v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
			v = v.substr(2);
		if (v.empty() || (v.size() % 2) == 1)
			return out;

		out.reserve(v.size() / 2);
		for (size_t i = 0; i + 1 < v.size(); i += 2)
		{
			int hi = HexDigit(v[i]);
			int lo = HexDigit(v[i + 1]);
			if (hi < 0 || lo < 0)
			{
				out.clear();
				return out; // non-hex char => empty, like the Python port
			}
			out.push_back((u8)((hi << 4) | lo));
		}
		return out;
	}

	// --------------------------------------------------------------------
	// Params (mirrors _read_params)
	// --------------------------------------------------------------------

	static ParamMap ReadParams(const pugi::xml_node &node, const ParamMap &inherited)
	{
		ParamMap params = inherited;
		for (pugi::xml_node p = node.child("param"); p; p = p.next_sibling("param"))
		{
			pugi::xml_attribute name = p.attribute("name");
			if (name)
				params[name.value()] = p.attribute("value").as_string("");
		}
		return params;
	}

	// --------------------------------------------------------------------
	// Patch / option / section parsing
	// --------------------------------------------------------------------

	static Patch ParsePatch(const pugi::xml_node &node)
	{
		Patch patch;
		patch.id = node.attribute("id").as_string("");
		patch.root = node.attribute("root").as_string("");

		for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
		{
			std::string tag = c.name();
			if (tag == "file")
			{
				File f;
				f.disc = c.attribute("disc").as_string("");
				f.external = c.attribute("external").as_string("");
				f.resize = ParseBool(c.attribute("resize"), true);
				f.create = ParseBool(c.attribute("create"), false);
				f.offset = ParseInt(c.attribute("offset"));
				f.fileoffset = ParseInt(c.attribute("fileoffset"));
				f.length = ParseInt(c.attribute("length"));
				patch.files.push_back(f);
			}
			else if (tag == "folder")
			{
				Folder f;
				f.disc = c.attribute("disc").as_string("");
				f.external = c.attribute("external").as_string("");
				f.resize = ParseBool(c.attribute("resize"), true);
				f.create = ParseBool(c.attribute("create"), false);
				f.recursive = ParseBool(c.attribute("recursive"), true);
				f.length = ParseInt(c.attribute("length"));
				patch.folders.push_back(f);
			}
			else if (tag == "savegame")
			{
				Savegame s;
				s.external = c.attribute("external").as_string("");
				s.clone = ParseBool(c.attribute("clone"), true);
				patch.savegames.push_back(s);
			}
			else if (tag == "memory")
			{
				Memory m;
				m.offset = ParseInt(c.attribute("offset"));
				m.value = ParseHexString(c.attribute("value"));
				m.valuefile = c.attribute("valuefile").as_string("");
				m.original = ParseHexString(c.attribute("original"));
				m.ocarina = ParseBool(c.attribute("ocarina"), false);
				m.search = ParseBool(c.attribute("search"), false);
				u32 align = ParseInt(c.attribute("align"), 1);
				m.align = align < 1 ? 1 : align;
				patch.memories.push_back(m);
			}
			// Unknown elements ignored, matching Riivolution's leniency.
		}
		return patch;
	}

	static Option ParseOption(const pugi::xml_node &node, const ParamMap &inherited)
	{
		Option option;
		option.name = node.attribute("name").as_string("");
		option.id = node.attribute("id").as_string("");
		option.selectedChoice = (int)ParseInt(node.attribute("default"), 0);

		ParamMap optionParams = ReadParams(node, inherited);

		for (pugi::xml_node cn = node.child("choice"); cn; cn = cn.next_sibling("choice"))
		{
			Choice choice;
			choice.name = cn.attribute("name").as_string("");
			ParamMap choiceParams = ReadParams(cn, optionParams);
			for (pugi::xml_node ref = cn.child("patch"); ref; ref = ref.next_sibling("patch"))
			{
				std::string pid = ref.attribute("id").as_string("");
				if (!pid.empty())
				{
					PatchRef pr;
					pr.id = pid;
					pr.params = ReadParams(ref, choiceParams);
					choice.patchRefs.push_back(pr);
				}
			}
			option.choices.push_back(choice);
		}
		return option;
	}

	//! Index of option nodes by id, for <macro> cloning.
	typedef std::map<std::string, pugi::xml_node> OptionIndex;

	static Section ParseSection(const pugi::xml_node &node, const OptionIndex &byId, std::string *error)
	{
		Section section;
		section.name = node.attribute("name").as_string("");

		for (pugi::xml_node c = node.first_child(); c; c = c.next_sibling())
		{
			std::string tag = c.name();
			if (tag == "option")
			{
				section.options.push_back(ParseOption(c, ParamMap()));
			}
			else if (tag == "macro")
			{
				std::string sourceId = c.attribute("id").as_string("");
				OptionIndex::const_iterator it = byId.find(sourceId);
				if (it == byId.end())
				{
					if (error && error->empty())
						*error = "<macro> references unknown option id '" + sourceId + "'";
					continue; // skip the bad macro but keep parsing
				}
				Option option = ParseOption(it->second, ReadParams(c, ParamMap()));
				pugi::xml_attribute nameAttr = c.attribute("name");
				if (nameAttr)
					option.name = nameAttr.value();
				option.id.clear(); // the clone is a distinct option
				pugi::xml_attribute defAttr = c.attribute("default");
				if (defAttr)
					option.selectedChoice = (int)ParseInt(defAttr, 0);
				section.options.push_back(option);
			}
		}
		return section;
	}

	// --------------------------------------------------------------------
	// Entry point
	// --------------------------------------------------------------------

	bool ParseFile(const char *xmlPath, Disc &out, std::string *error)
	{
		pugi::xml_document doc;
		pugi::xml_parse_result res = doc.load_file(xmlPath);
		if (!res)
		{
			if (error)
				*error = std::string(xmlPath) + ": " + res.description();
			return false;
		}

		pugi::xml_node root = doc.child("wiidisc");
		if (!root)
		{
			if (error)
				*error = "root element is not <wiidisc>";
			return false;
		}

		out = Disc();
		out.xmlPath = xmlPath;
		out.version = (int)ParseInt(root.attribute("version"), 0);
		pugi::xml_attribute rootAttr = root.attribute("root");
		out.root = rootAttr ? rootAttr.value() : "/riivolution";
		if (out.root.empty())
			out.root = "/riivolution";

		if (out.version != 1)
		{
			if (error)
				*error = "unsupported wiidisc version (only version 1 exists)";
			return false;
		}

		// <id>
		pugi::xml_node idNode = root.child("id");
		if (idNode)
		{
			GameFilter &f = out.filter;
			pugi::xml_attribute a;
			if ((a = idNode.attribute("game")))      { f.hasGame = true; f.game = a.value(); }
			if ((a = idNode.attribute("developer"))) { f.hasDeveloper = true; f.developer = a.value(); }
			if ((a = idNode.attribute("disc")))      { f.hasDisc = true; f.disc = (int)ParseInt(a); }
			if ((a = idNode.attribute("version")))   { f.hasVersion = true; f.version = (int)ParseInt(a); }
			for (pugi::xml_node r = idNode.child("region"); r; r = r.next_sibling("region"))
			{
				pugi::xml_attribute type = r.attribute("type");
				if (type && *type.value())
					f.regions.push_back(type.value());
			}
		}

		// Macros can reference options in any section, so index them first.
		OptionIndex byId;
		for (pugi::xml_node opts = root.child("options"); opts; opts = opts.next_sibling("options"))
			for (pugi::xml_node sec = opts.child("section"); sec; sec = sec.next_sibling("section"))
				for (pugi::xml_node opt = sec.child("option"); opt; opt = opt.next_sibling("option"))
				{
					pugi::xml_attribute oid = opt.attribute("id");
					if (oid && *oid.value())
						byId[oid.value()] = opt;
				}

		for (pugi::xml_node opts = root.child("options"); opts; opts = opts.next_sibling("options"))
			for (pugi::xml_node sec = opts.child("section"); sec; sec = sec.next_sibling("section"))
				out.sections.push_back(ParseSection(sec, byId, error));

		for (pugi::xml_node p = root.child("patch"); p; p = p.next_sibling("patch"))
			out.patches.push_back(ParsePatch(p));

		return true;
	}
}
