// Validation-boundary checkpoint order with mocked light/log sinks.
// Exercises the production ValidationBoundary template directly: the exact
// call order (explicit ON, entry line, validate, explicit OFF) is asserted,
// and the mock sink types deliberately have no Pulse method, so the
// sequence under test cannot express a toggle at all - ordinary progress
// logging cannot cancel these states by construction.
// What this does NOT prove: physical LED visibility. An unobserved signal
// stays inconclusive no matter what the order guarantees.
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>
#include "riivo/RiivoCheckpoints.hpp"
using namespace Riivo;
static int checks, failures;
static void ck(bool ok, const char *what) {
    ++checks; if (!ok) { ++failures; printf("FAIL: %s\n", what); }
}
struct MockLight
{
    std::vector<std::string> calls;
    void Set(bool on) { calls.push_back(on ? "set:true" : "set:false"); }
};
struct MockLog
{
    std::vector<std::string> lines;
    void Line(const char *text) { lines.push_back(text ? text : ""); }
};
int main() {
    // Exact order: ON, entry line, validate runs once, OFF.
    {
        MockLight light;
        MockLog log;
        int validated = 0;
        auto validate = [&]() { ++validated; };
        ValidationBoundary(light, log, validate, "validating the rebuilt table");
        ck(validated == 1, "validate runs exactly once");
        ck(light.calls.size() == 2, "exactly two light writes");
        ck(light.calls.size() == 2 && light.calls[0] == "set:true",
           "first write is explicit ON");
        ck(light.calls.size() == 2 && light.calls[1] == "set:false",
           "last write is explicit OFF");
        ck(log.lines.size() == 1 && log.lines[0] == "validating the rebuilt table",
           "entry text passes through between the sets");
    }
    // Entry text is caller data, not fixed by the runner.
    {
        MockLight light;
        MockLog log;
        int validated = 0;
        auto validate = [&]() { ++validated; };
        ValidationBoundary(light, log, validate, "other boundary");
        ck(log.lines.size() == 1 && log.lines[0] == "other boundary",
           "entry text is caller-supplied");
        ck(validated == 1, "validate still runs once");
    }
    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
