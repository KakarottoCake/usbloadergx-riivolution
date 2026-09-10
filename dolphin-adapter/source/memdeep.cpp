/****************************************************************************
 * v7 deep full-prep-path mirror (PROD-MIRROR where noted).
 *
 * (Header comment lives on RunDeepWindow below; this file holds the
 * deep-tree image builder. Approximation ledger: synthetic strings
 * whose depth/name-length distributions were measured from the real
 * SB4E01 FST + local Spectral tree (HANDOFF) - no game/mod bytes
 * committed. Directory topology matches measured per-depth counts
 * within ~2%; file sizes are deterministic pseudo-sizes.)
 ***************************************************************************/
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <gccore.h>

static const char *kStems[] = {
	"IslandFleetGalaxyMap", "PeachCastleGalaxyMap", "TitleLogo",
	"ProductMapObjDataTable", "SaveIconBanner", "FileSelectMap",
	"StageDataGalaxyLight", "ObjectDataTableArc", "LayoutDataScreen",
	"ParticleDataEffect", "SystemDataConfig", "ScenarioDataTable",
	"AudioResStreamData", "LocalizeDataText", "CustomCodeModule",
	"GalaxyMapScenario", "MapObjDataTable", "BannerSaveIcon",
	"LightScenarioData", "ScreenLayoutData", "EffectParticle",
	"ConfigSystemData", "TextLocalizeData", "ModuleCustomCode",
	"StreamAudioRes", "DataStageGalaxy", "DataObjectTable",
	"TableScenarioMap",
};
static const char *kExts[] = {
	".arc", ".brres", ".bmg", ".tpl", ".breff", ".bin",
};
static const int kNStems = 28;
static const int kNExts = 6;

struct MwNode
{
	bool isDir;
	std::string name;
	u32 a, b; // files: offset>>2, length; dirs: filled at emit
	std::vector<MwNode> kids;
};

static void MwName(char *dst, size_t cap, int stem, int variant,
				   const char *ext)
{
	snprintf(dst, cap, "%s%03d%s", kStems[stem % kNStems], variant, ext);
}

//! Emit `dir`'s children pre-order with correct directory ends.
//! Returns nothing; `self` is dir's own index (0 for root).
static void MwEmitDir(const MwNode &dir, u32 self, std::vector<u8> &entries,
					  std::string &strings)
{
	for (size_t i = 0; i < dir.kids.size(); ++i)
	{
		const MwNode &c = dir.kids[i];
		const u32 here = (u32) (entries.size() / 12);
		const u32 no = (u32) strings.size();
		strings += c.name;
		strings += '\0';
		entries.resize(entries.size() + 12);
		u8 *e = &entries[here * 12];
		e[0] = c.isDir ? 1 : 0;
		e[1] = no >> 16; e[2] = no >> 8; e[3] = no;
		if (c.isDir)
		{
			e[4] = self >> 24; e[5] = self >> 16;
			e[6] = self >> 8; e[7] = self;
			e[8] = e[9] = e[10] = e[11] = 0; // patched below
			MwEmitDir(c, here, entries, strings);
			const u32 end = (u32) (entries.size() / 12);
			u8 *p = &entries[here * 12];
			p[8] = end >> 24; p[9] = end >> 16;
			p[10] = end >> 8; p[11] = end;
		}
		else
		{
			e[4] = c.a >> 24; e[5] = c.a >> 16;
			e[6] = c.a >> 8; e[7] = c.a;
			e[8] = c.b >> 24; e[9] = c.b >> 16;
			e[10] = c.b >> 8; e[11] = c.b;
		}
	}
}

static int MwFileNo = 0;

static MwNode MwFile(const char *name, int i)
{
	MwNode n;
	n.isDir = false;
	n.name = name;
	n.a = (0x100000u + (u32) i * 0x8000u) >> 2;
	n.b = 0x4000;
	return n;
}

//! Deep base tree: 15 L1 dirs; ~144 files + 3 L2 dirs each; ~36 files +
//! 1 L3 dir each (first 40 L2s); ~15 files per L3. Totals ≈ measured
//! {1:15, 2:2210, 3:1668, 4:599} within ~2%. `baseFiles` collects
//! lowercase disc paths of depth-2 files for the replacement subset.
static MwNode MwDeepTree(std::vector<std::string> &baseFiles)
{
	MwNode root;
	root.isDir = true;
	char name[64], path[160];
	int f = 0;
	for (int d1 = 0; d1 < 15; ++d1)
	{
		MwNode l1;
		l1.isDir = true;
		snprintf(name, sizeof(name), "DirL1_%02d", d1);
		l1.name = name;
		for (int i = 0; i < 144; ++i, ++f)
		{
			MwName(name, sizeof(name), f, f % 200, kExts[f % kNExts]);
			snprintf(path, sizeof(path), "/dirl1_%02d/%s", d1, name);
			for (char *c = path; *c; ++c)
				if (*c >= 'A' && *c <= 'Z')
					*c += 32;
			if ((int) baseFiles.size() < 400)
				baseFiles.push_back(path);
			l1.kids.push_back(MwFile(name, f));
		}
		for (int d2 = 0; d2 < 3; ++d2)
		{
			MwNode l2;
			l2.isDir = true;
			snprintf(name, sizeof(name), "SubL2_%02d_%d", d1, d2);
			l2.name = name;
			for (int i = 0; i < 36; ++i, ++f)
			{
				MwName(name, sizeof(name), f + 5000, f % 200,
					   kExts[(f + 1) % kNExts]);
				l2.kids.push_back(MwFile(name, f));
			}
			if (d1 * 3 + d2 < 40)
			{
				MwNode l3;
				l3.isDir = true;
				snprintf(name, sizeof(name), "SubL3_%02d", d1 * 3 + d2);
				l3.name = name;
				for (int i = 0; i < 15; ++i, ++f)
				{
					MwName(name, sizeof(name), f + 9000, f % 200,
						   kExts[(f + 2) % kNExts]);
					l3.kids.push_back(MwFile(name, f));
				}
				l2.kids.push_back(l3);
			}
			l1.kids.push_back(l2);
		}
		root.kids.push_back(l1);
	}
	MwFileNo = f;
	return root;
}

//! Public entry: build the deep image and the replacement pool.
void MwDeepBuild(std::vector<u8> &img, std::vector<std::string> &baseFiles)
{
	MwNode root = MwDeepTree(baseFiles);
	std::vector<u8> entries;
	entries.resize(12);
	std::string strings;
	strings += '\0';
	MwEmitDir(root, 0, entries, strings);
	u32 total = 0;
	for (size_t i = 0; i < entries.size(); i += 12)
		++total;
	u8 *r = &entries[0];
	r[0] = 1;
	r[1] = r[2] = r[3] = 0;
	r[4] = r[5] = r[6] = r[7] = 0;
	r[8] = total >> 24; r[9] = total >> 16;
	r[10] = total >> 8; r[11] = total;
	img.clear();
	img.insert(img.end(), entries.begin(), entries.end());
	img.insert(img.end(), strings.begin(), strings.end());
}
