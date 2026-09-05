#include <sys/stat.h>
#include "RiivoPatchGuard.hpp"
#include "RiivoConfig.hpp"
namespace {
struct Range { u32 start; u64 end; };
std::vector<Range> ranges;
bool active = false;
}
extern "C" int RiivoPatchConflict(u32 address, u32 length) {
    if (!active || !length) return 0;
    const u64 end = (u64)address + length;
    for (size_t i=0; i<ranges.size(); ++i)
        if (address < ranges[i].end && ranges[i].start < end) return 1;
    return 0;
}
namespace Riivo {
void ProtectAppliedPatch(u32 address, u32 length) {
    if (!active || !length) return;
    const u64 end = (u64)address + length;
    if (!((address >= 0x80000000 && end <= 0x81800000) ||
          (address >= 0x90000000 && end <= 0x94000000))) return;
    Range r = { address, end }; ranges.push_back(r);
}
void ConfigurePatchProtection(const ResolvedPatchSet &set, const std::string &device, bool enabled) {
    ranges.clear(); active = enabled;
    if (!enabled) return;
    for (size_t i=0; i<set.memories.size(); ++i) {
        const ResolvedMemory &m = set.memories[i];
        // A search/ocarina offset is not the write address. Actual writes
        // are registered by the memory patcher, not guessed here.
        if (m.search || m.ocarina) continue;
        u32 length = (u32)m.value.size();
        if (!m.valuefile.empty()) {
            // The bytes are read when the patch is applied, which is after the
            // card is gone, so the length has to be measured here. One that
            // cannot be measured stays unprotected: the late collision check
            // before the code handler then refuses the launch rather than
            // letting the two writes land on each other.
            struct stat info;
            length = (stat(JoinPath(device, m.root, m.valuefile).c_str(), &info) == 0
                      && info.st_size > 0) ? (u32)info.st_size : 0;
        }
        ProtectAppliedPatch(m.offset | 0x80000000, length);
    }
}
}
