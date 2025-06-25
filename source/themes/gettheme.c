#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <gctypes.h>
#include <ogc/gx.h>
#include "settings/CSettings.h"

enum
{
	ALIGN_LEFT, ALIGN_RIGHT, ALIGN_CENTER, ALIGN_TOP, ALIGN_BOTTOM, ALIGN_MIDDLE
};

typedef struct _MSG
{
	u32 id;
	char* msgstr;
	struct _MSG *next;
} MSG;
static MSG *baseMSG=0;


#define HASHWORDBITS 32

/* Defines the so called `hashpjw' function by P.J. Weinberger
   [see Aho/Sethi/Ullman, COMPILERS: Principles, Techniques and Tools,
   1986, 1987 Bell Telephone Laboratories, Inc.]  */
static inline u32 hash_string (const char *str_param)
{
	u32 hval, g;
	const char *str = str_param;

	/* Compute the hash value for the given string.  */
	hval = 0;
	while (*str != '\0')
	{
		hval <<= 4;
		hval += (u8) *str++;
		g = hval & ((u32) 0xf << (HASHWORDBITS - 4));
		if (g != 0)
		{
			hval ^= g >> (HASHWORDBITS - 8);
			hval ^= g;
		}
	}
	return hval;
}


static MSG *findMSG(u32 id)
{
	MSG *msg;
	for(msg=baseMSG; msg; msg=msg->next)
	{
		if(msg->id == id)
			return msg;
	}
	return NULL;
}

static MSG *setMSG(const char *msgid, const char *msgstr)
{
	if(GetLayoutVersion() >= 2 && (!msgstr || msgstr[0] == '\0'))
		return NULL;

	u32 id = hash_string(msgid);
	MSG *msg = findMSG(id);
	if(!msg)
	{
		msg = (MSG *)malloc(sizeof(MSG));
		msg->id		= id;
		msg->msgstr = NULL;
		msg->next	= baseMSG;
		baseMSG		= msg;
	}
	if(msg)
	{
		if(msgstr)
		{
			if(msg->msgstr) free(msg->msgstr);
			msg->msgstr = strdup(msgstr);
		}
		return msg;
	}
	return NULL;
}

static inline void ClearPrefixes(char * msg)
{
	if(!msg)
		return;

	const char * ptr = msg;

	int i = 0;

	while(ptr[0] != '\0')
	{
		if(ptr[0] == '\\' && (ptr[1] == '\\' || ptr[1] == '"'))
		{
			++ptr;
		}

		msg[i] = ptr[0];

		++i;
		++ptr;
	}

	msg[i] = '\0';
}

void ThemeCleanUp(void)
{
	while(baseMSG)
	{
		MSG *nextMsg =baseMSG->next;
		free(baseMSG->msgstr);
		free(baseMSG);
		baseMSG = nextMsg;
	}
}

bool LoadTheme(const char* themeFile)
{
	FILE *f;
	char line[200];
	char *lastID=NULL;

	f = fopen(themeFile, "r");
	if(!f)
		return false;

	while (fgets(line, sizeof(line), f))
	{
		// lines starting with # are comments
		if (line[0] == '#')
			continue;
		else if (strncmp(line, "msgid \"", 7) == 0)
		{
			char *msgid, *end;
			if(lastID) { free(lastID); lastID=NULL;}
			msgid = &line[7];
			end = strrchr(msgid, '"');
			if(end && end-msgid>1)
			{
				*end = 0;
				ClearPrefixes(msgid);
				lastID = strdup(msgid);
			}
		}
		else if (strncmp(line, "msgstr \"", 8) == 0)
		{
			char *msgstr, *end;

			if(lastID == NULL)
				continue;

			msgstr = &line[8];
			end = strrchr(msgstr, '"');
			if(end && end-msgstr>1)
			{
				*end = 0;
				ClearPrefixes(msgstr);
				setMSG(lastID, msgstr);
			}
			free(lastID);
			lastID=NULL;
		}
	}

	fclose(f);
	return true;
}

// Themes are handled like this to maintain compatibility with older themes
void LoadNewTheme()
{
	setMSG("-50 - game bannergrid layout pos y", "-74");
	setMSG("r=237 g=237 b=237 a=255 - banner icon frame color", "r=228 g=228 b=228 a=255");
	setMSG("20 - game grid layout pos y", "-18");
	setMSG("0 - game browser scrollbar pos x", "-2");
	setMSG("r=55 g=190 b=237 a=255 - hdd info color", "r=133 g=133 b=133 a=255");
	setMSG("400 - hdd info pos y", "413");
	setMSG("r=55 g=190 b=237 a=255 - game count color", "r=133 g=133 b=133 a=255");
	setMSG("420 - game count pos y", "436");
	setMSG("371 - settings btn pos y", "360");
	setMSG("64 - settings btn pos x", "56");
	setMSG("371 - home menu btn pos y", "360");
	setMSG("489 - home menu btn pos x", "492");
	setMSG("355 - power off btn pos y", "403");
	setMSG("576 - power off btn pos x", "156");
	setMSG("160 - sd card btn pos x", "20");
	setMSG("395 - sd card btn pos y", "369");
	setMSG("26 - cover/download btn pos x", "20");
	setMSG("58 - cover/download btn pos y", "48");
	setMSG("305 - gameID btn pos y", "289");
	setMSG("68 - gameID btn pos x", "38");
	setMSG("r=138 g=138 b=138 a=240 - clock color", "r=153 g=153 b=153 a=240");
	setMSG("335 - clock pos y", "314");

	setMSG("168 - list layout favorite btn pos x", "258");
	setMSG("214 - list layout favorite btn pos x widescreen", "268");
	setMSG("13 - list layout favorite btn pos y", "367");
	setMSG("208 - list layout search btn pos x", "178");
	setMSG("246 - list layout search btn pos x widescreen", "203");
	setMSG("13 - list layout search btn pos y", "367");
	setMSG("248 - list layout abc/sort btn pos x", "223");
	setMSG("278 - list layout abc/sort btn pos x widescreen", "240");
	setMSG("13 - list layout abc/sort btn pos y", "367");
	setMSG("288 - list layout loadermode btn pos x", "586");
	setMSG("310 - list layout loadermode btn pos x widescreen", "586");
	setMSG("13 - list layout loadermode btn pos y", "369");
	setMSG("328 - list layout category btn pos x", "291");
	setMSG("342 - list layout category btn pos x widescreen", "295");
	setMSG("13 - list layout category btn pos y", "367");
	setMSG("502 - list layout lock btn pos x widescreen", "322");
	setMSG("528 - list layout lock btn pos x", "325");
	setMSG("13 - list layout lock btn pos y", "367");
	setMSG("534 - list layout dvd btn pos x widescreen", "402");
	setMSG("568 - list layout dvd btn pos x", "427");
	setMSG("13 - list layout dvd btn pos y", "367");

	setMSG("200 - game list layout pos x", "218");
	setMSG("49 - game list layout pos y", "29");

	setMSG("100 - grid layout favorite btn pos x", "258");
	setMSG("144 - grid layout favorite btn pos x widescreen", "268");
	setMSG("13 - grid layout favorite btn pos y", "367");
	setMSG("140 - grid layout search btn pos x", "178");
	setMSG("176 - grid layout search btn pos x widescreen", "203");
	setMSG("13 - grid layout search btn pos y", "367");
	setMSG("180 - grid layout abc/sort btn pos x", "223");
	setMSG("208 - grid layout abc/sort btn pos x widescreen", "240");
	setMSG("13 - grid layout abc/sort btn pos y", "367");
	setMSG("220 - grid layout loadermode btn pos x", "586");
	setMSG("240 - grid layout loadermode btn pos x widescreen", "586");
	setMSG("13 - grid layout loadermode btn pos y", "369");
	setMSG("260 - grid layout category btn pos x", "291");
	setMSG("272 - grid layout category btn pos x widescreen", "295");
	setMSG("13 - grid layout category btn pos y", "367");
	setMSG("432 - grid layout lock btn pos x widescreen", "322");
	setMSG("460 - grid layout lock btn pos x", "325");
	setMSG("13 - grid layout lock btn pos y", "367");
	setMSG("464 - grid layout dvd btn pos x widescreen", "402");
	setMSG("500 - grid layout dvd btn pos x", "427");
	setMSG("13 - grid layout dvd btn pos y", "367");

	setMSG("100 - carousel layout favorite btn pos x", "258");
	setMSG("144 - carousel layout favorite btn pos x widescreen", "268");
	setMSG("13 - carousel layout favorite btn pos y", "367");
	setMSG("140 - carousel layout search btn pos x", "178");
	setMSG("176 - carousel layout search btn pos x widescreen", "203");
	setMSG("13 - carousel layout search btn pos y", "367");
	setMSG("180 - carousel layout abc/sort btn pos x", "223");
	setMSG("208 - carousel layout abc/sort btn pos x widescreen", "240");
	setMSG("13 - carousel layout abc/sort btn pos y", "367");
	setMSG("220 - carousel layout loadermode btn pos x", "586");
	setMSG("240 - carousel layout loadermode btn pos x widescreen", "586");
	setMSG("13 - carousel layout loadermode btn pos y", "369");
	setMSG("260 - carousel layout category btn pos x", "291");
	setMSG("272 - carousel layout category btn pos x widescreen", "295");
	setMSG("13 - carousel layout category btn pos y", "367");
	setMSG("432 - carousel layout lock btn pos x widescreen", "322");
	setMSG("460 - carousel layout lock btn pos x", "325");
	setMSG("13 - carousel layout lock btn pos y", "367");
	setMSG("464 - carousel layout dvd btn pos x widescreen", "402");
	setMSG("500 - carousel layout dvd btn pos x", "427");
	setMSG("13 - carousel layout dvd btn pos y", "367");
	setMSG("-20 - game carousel layout pos y", "-44");

	setMSG("100 - bannergrid layout favorite btn pos x", "258");
	setMSG("144 - bannergrid layout favorite btn pos x widescreen", "268");
	setMSG("13 - bannergrid layout favorite btn pos y", "367");
	setMSG("140 - bannergrid layout search btn pos x", "178");
	setMSG("176 - bannergrid layout search btn pos x widescreen", "203");
	setMSG("13 - bannergrid layout search btn pos y", "367");
	setMSG("180 - bannergrid layout abc/sort btn pos x", "223");
	setMSG("208 - bannergrid layout abc/sort btn pos x widescreen", "240");
	setMSG("13 - bannergrid layout abc/sort btn pos y", "367");
	setMSG("220 - bannergrid layout loadermode btn pos x", "586");
	setMSG("240 - bannergrid layout loadermode btn pos x widescreen", "586");
	setMSG("13 - bannergrid layout loadermode btn pos y", "369");
	setMSG("260 - bannergrid layout category btn pos x", "291");
	setMSG("272 - bannergrid layout category btn pos x widescreen", "295");
	setMSG("13 - bannergrid layout category btn pos y", "367");
	setMSG("432 - bannergrid layout lock btn pos x widescreen", "322");
	setMSG("460 - bannergrid layout lock btn pos x", "325");
	setMSG("13 - bannergrid layout lock btn pos y", "367");
	setMSG("464 - bannergrid layout dvd btn pos x widescreen", "402");
	setMSG("500 - bannergrid layout dvd btn pos x", "427");
	setMSG("13 - bannergrid layout dvd btn pos y", "367");

	setMSG("68 - region info text pos x", "38");
	setMSG("30 - region info text pos y", "28");
}

int getThemeInt(const char *msgid)
{
	MSG *msg = findMSG(hash_string(msgid));
	if(msg) return atoi(msg->msgstr);
	return atoi(msgid);
}

float getThemeFloat(const char *msgid)
{
	MSG *msg = findMSG(hash_string(msgid));
	if(msg) return atof(msg->msgstr);
	return atof(msgid);
}

int getThemeAlignment(const char *msgid)
{
	MSG *msg = findMSG(hash_string(msgid));

	const char * string = msgid;
	if(msg)
		string = msg->msgstr;

	while(*string == ' ') string++;

	if(strncasecmp(string, "left", strlen("left")) == 0)
		return ALIGN_LEFT;

	else if(strncasecmp(string, "right", strlen("right")) == 0)
		return ALIGN_RIGHT;

	else if(strncasecmp(string, "center", strlen("center")) == 0)
		return ALIGN_CENTER;

	else if(strncasecmp(string, "top", strlen("top")) == 0)
		return ALIGN_TOP;

	else if(strncasecmp(string, "bottom", strlen("bottom")) == 0)
		return ALIGN_BOTTOM;

	else if(strncasecmp(string, "middle", strlen("middle")) == 0)
		return ALIGN_MIDDLE;

	return -1;
}

GXColor getThemeColor(const char *msgid)
{
	MSG *msg = findMSG(hash_string(msgid));

	const char * string = msgid;
	if(msg)
		string = msg->msgstr;

	GXColor color = (GXColor) {0, 0, 0, 0};

	while(*string == ' ') string++;

	while(*string != '\0')
	{
		if(*string == 'r')
		{
			string++;
			while(*string == ' ' || *string == '=' || *string == ',') string++;

			if(*string == '\0')
				break;

			color.r = atoi(string) & 0xFF;
		}
		else if(*string == 'g')
		{
			string++;
			while(*string == ' ' || *string == '=' || *string == ',') string++;

			if(*string == '\0')
				break;

			color.g = atoi(string) & 0xFF;
		}
		else if(*string == 'b')
		{
			string++;
			while(*string == ' ' || *string == '=' || *string == ',') string++;

			if(*string == '\0')
				break;

			color.b = atoi(string) & 0xFF;
		}
		else if(*string == 'a')
		{
			string++;
			while(*string == ' ' || *string == '=' || *string == ',') string++;

			if(*string == '\0')
				break;

			color.a = atoi(string) & 0xFF;
		}
		else if(*string == '-')
		{
			break;
		}

		++string;
	}

	return color;
}
