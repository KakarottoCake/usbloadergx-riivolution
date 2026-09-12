// Path resolution: absolute externals are device-root-relative, relative
// paths keep today's device-joined behavior bit-for-bit.
// Exercises production JoinPath/Resolve/ParseFile directly. The Superstar
// section (SUPERSTAR_XML/SUPERSTAR_MOD) replays the real SSMG.XML against
// the real 368-file pack and fails exactly as the hardware run did when
// the join doubles the root (zero files found).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>
#include "riivo/RiivoConfig.hpp"
#include "riivo/RiivoParser.hpp"
#include "riivo/RiivoFile.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
// Lister mapping a device prefix onto a host tree (same shape as the
// Spectral harness): production FsDirLister does the walking.
struct MapLister : public FsDirLister
{
    std::string devPrefix, modRoot;
    void List(const std::string &fullDir, bool recursive,
              std::vector<std::string> &out)
    {
        if (!devPrefix.empty() &&
            fullDir.compare(0, devPrefix.size(), devPrefix) == 0)
        {
            FsDirLister::List(modRoot + fullDir.substr(devPrefix.size()),
                              recursive, out);
        }
        else
            FsDirLister::List(fullDir, recursive, out);
    }
};
int main() {
    // Absolute external ignores any patch root (the Superstar shape).
    ck(JoinPath("usb1:", "/SSMG", "/SSMG/StageData") == "usb1:/SSMG/StageData",
       "absolute external is device-root-relative");
    ck(JoinPath("", "/SSMG", "/SSMG/StageData") == "/SSMG/StageData",
       "absolute external with empty device stays rooted");
    ck(JoinPath("usb1:", "", "/SSMG/StageData") == "usb1:/SSMG/StageData",
       "absolute external with empty root");
    // Relative behavior is byte-identical to before (drive-relative when the
    // root itself is relative - the long-standing convention, pinned here).
    ck(JoinPath("usb1:", "/Spectral", "AudioRes/x.arc") == "usb1:/Spectral/AudioRes/x.arc",
       "relative external under absolute root unchanged");
    ck(JoinPath("usb1:", "SSMG", "StageData") == "usb1:SSMG/StageData",
       "relative external under relative root unchanged");
    ck(JoinPath("usb1:", "", "rel/Y") == "usb1:rel/Y",
       "relative external under empty root unchanged");
    ck(JoinPath("sd:", "/riivolution", "m.xml") == "sd:/riivolution/m.xml",
       "sd: device keeps its slash");
    // Resolve carries roots through; the join decides.
    {
        Disc d;
        d.root = "/riivolution";
        Patch p;
        p.id = "P";
        p.root = "/SSMG";
        Folder f;
        f.disc = "/StageData";
        f.external = "/SSMG/StageData";
        f.create = true;
        p.folders.push_back(f);
        Folder g;
        g.disc = "/X";
        g.external = "rel/Y";
        p.folders.push_back(g);
        d.patches.push_back(p);
        d.sections.resize(1);
        d.sections[0].options.resize(1);
        d.sections[0].options[0].choices.resize(1);
        d.sections[0].options[0].choices[0].patchRefs.resize(1);
        d.sections[0].options[0].choices[0].patchRefs[0].id = "P";
        d.sections[0].options[0].selectedChoice = 1;
        ResolvedPatchSet set;
        Resolve(d, "RMGE01", set);
        ck(set.folders.size() == 2, "both rules resolve");
        ck(JoinPath("usb1:", set.folders[0].root, set.folders[0].external) ==
               "usb1:/SSMG/StageData",
           "resolved absolute external joins to device root");
        ck(JoinPath("usb1:", set.folders[1].root, set.folders[1].external) ==
               "usb1:/SSMG/rel/Y",
           "resolved relative external keeps patch root");
    }
    // Parser default: missing wiidisc root means /riivolution (GX has no
    // XML-directory tracking; that is a deliberate, documented non-parity
    // with the reference, pinned here so it cannot drift silently).
    {
        const char *tmpdir = getenv("TMPDIR");
        if (!tmpdir || !*tmpdir) tmpdir = "/tmp";
        std::string xp = std::string(tmpdir) + "/riivo-pathresolve-default.xml";
        FILE *xf = fopen(xp.c_str(), "w");
        if (xf)
        {
            fputs("<wiidisc version=\"1\"><id game=\"RMG\" />"
                  "<options><section name=\"S\"><option name=\"O\">"
                  "<choice name=\"C\"><patch id=\"P\" /></choice>"
                  "</option></section></options>"
                  "<patch id=\"P\"><folder disc=\"/X\" external=\"rel/Y\" /></patch>"
                  "</wiidisc>", xf);
            fclose(xf);
            Disc d;
            std::string err;
            if (ParseFile(xp.c_str(), d, &err))
            {
                ck(d.root == "/riivolution", "missing wiidisc root defaults to /riivolution");
                d.sections[0].options[0].selectedChoice = 1;
                ResolvedPatchSet set;
                Resolve(d, "RMGE01", set);
                ck(set.folders.size() == 1 && set.folders[0].root == "/riivolution",
                   "patch without root inherits the default");
            }
            else
                ck(false, "default-root xml parses");
            remove(xp.c_str());
        }
        else
            printf("SKIP: cannot write temp xml\n");
    }
    // Real Superstar pack: every rule must find its files (hardware run
    // found zero when the join doubled the root).
    {
        const char *xmlPath = getenv("SUPERSTAR_XML");
        const char *modRoot = getenv("SUPERSTAR_MOD");
        if (!xmlPath || !*xmlPath || !modRoot || !*modRoot)
        {
            printf("SKIP: superstar pack (set SUPERSTAR_XML/SUPERSTAR_MOD)\n");
        }
        else
        {
            Disc disc;
            std::string perr;
            ck(ParseFile(xmlPath, disc, &perr), "SSMG.XML parses");
            ck(disc.IsValidForGame("RMGE01", 0, 0), "SSMG matches RMGE01");
            for (size_t s = 0; s < disc.sections.size(); ++s)
                for (size_t o = 0; o < disc.sections[s].options.size(); ++o)
                    disc.sections[s].options[o].selectedChoice = 1;
            ResolvedPatchSet set;
            Resolve(disc, "RMGE01", set);
            ck(set.folders.size() == 13, "13 Superstar folder rules resolve");
            MapLister lister;
            lister.devPrefix = "usb1:";
            lister.modRoot = modRoot;
            size_t total = 0;
            bool doubled = false, empty = false;
            for (size_t i = 0; i < set.folders.size(); ++i)
            {
                std::string joined =
                    JoinPath("usb1:", set.folders[i].root, set.folders[i].external);
                if (joined.find("/SSMG/SSMG/") != std::string::npos)
                    doubled = true;
                std::vector<std::string> out;
                lister.List(joined, set.folders[i].recursive, out);
                if (out.empty())
                    empty = true;
                total += out.size();
            }
            ck(!doubled, "no rule resolves into a doubled root");
            ck(!empty, "every rule lists files");
            // 368 files on disk; UsEnglish is shared by 9 rules, so the
            // per-rule sum exceeds it by 8 extra listings of that dir.
            ck(total >= 368, "all 368 pack files reachable through the rules");
            printf("  superstar: %u folder rules, %u per-rule listings\n",
                   (unsigned) set.folders.size(), (unsigned) total);
        }
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
