/*
Copyright (C) 2025 blackb0x

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

1. The origin of this software must not be misrepresented; you must not
claim that you wrote the original software. If you use this software
in a product, an acknowledgment in the product documentation would be
appreciated but is not required.

2. Altered source versions must be plainly marked as such, and must not be
misrepresented as being the original software.

3. This notice may not be removed or altered from any source
distribution.
*/
#include "settings/CSettings.h"
#include "xml/pugixml.hpp"
#include "gecko.h"

int editMetaArguments()
{
	if (Settings.skipSaving)
		return 0;

	char metapath[256];
	snprintf(metapath, sizeof(metapath), "%smeta.xml", Settings.ConfigPath);

	pugi::xml_document xmlDoc;
	pugi::xml_parse_result result = xmlDoc.load_file(metapath);
	if (!result)
		return 0;

	pugi::xml_node decl = xmlDoc.prepend_child(pugi::node_declaration);
	decl.append_attribute("version") = "1.0";
	decl.append_attribute("encoding") = "UTF-8";
	decl.append_attribute("standalone") = "yes";

	pugi::xml_node app = xmlDoc.child("app");
	if (!app)
		return 0;

	app.child("disabled_arguments").set_name("arguments");
	pugi::xml_node arguments = app.child("arguments");
	if (!arguments)
	{
		pugi::xml_node date = app.child("release_date");
		if (!date)
			return 0;
		arguments = app.insert_child_after("arguments", date);
	}
	else
		arguments.remove_children();

	char line[32];
	snprintf(line, sizeof(line), "--ios=%d", Settings.LoaderIOS);
	if (!arguments.append_child("arg").append_child(pugi::node_pcdata).set_value(line))
		return 0;
	snprintf(line, sizeof(line), "--bootios=%d", Settings.BootIOS);
	if (!arguments.append_child("arg").append_child(pugi::node_pcdata).set_value(line))
		return 0;
	snprintf(line, sizeof(line), "--usbport=%d", Settings.USBPort);
	if (!arguments.append_child("arg").append_child(pugi::node_pcdata).set_value(line))
		return 0;
	snprintf(line, sizeof(line), "--sdmode=%d", Settings.SDMode);
	if (!arguments.append_child("arg").append_child(pugi::node_pcdata).set_value(line))
		return 0;

	bool res = xmlDoc.save_file(metapath);
	gprintf("%s meta.xml\n", res ? "Saved" : "Failed to save");

	return res;
}
