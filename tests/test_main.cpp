#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#if defined(_MSC_VER) && defined(_DEBUG)
#include <crtdbg.h>
#endif

int main(int argc, char** argv) {
#if defined(_MSC_VER) && defined(_DEBUG)
    // Debug builds report every heap block still allocated when the process ends (after static
    // objects are destroyed) on stderr, so a leak shows up in the test output.
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    _CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_WARN, _CRTDBG_FILE_STDERR);
#endif
    doctest::Context context(argc, argv);
    return context.run();
}
