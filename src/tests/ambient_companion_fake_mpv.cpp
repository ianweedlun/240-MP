// Fake mpv fixture for the ambient companion teardown gate (C++ so it builds
// in this CXX-only project with no extra CMake language). Records its PID in a
// file and appends it to a live registry, then loops. Cooperative mode dies on
// SIGTERM, which is what a well-behaved mpv does when AmbientModeBackend stops
// it. The binary is copied to <scratch>/bin/mpv by the probe so it wins on PATH.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <unistd.h>

static void on_term(int sig) { (void)sig; _exit(0); }

int main()
{
    const char *pidfile = std::getenv("FAKE_MPV_PIDFILE");
    if (pidfile) {
        FILE *f = std::fopen(pidfile, "w");
        if (f) { std::fprintf(f, "%ld\n", (long)getpid()); std::fclose(f); }
    }
    const char *reg = std::getenv("FAKE_MPV_REGISTRY");
    if (reg) {
        FILE *f = std::fopen(reg, "a");
        if (f) { std::fprintf(f, "%ld\n", (long)getpid()); std::fclose(f); }
    }
    const char *mode = std::getenv("FAKE_MPV_MODE");
    std::signal(SIGTERM, (mode && std::strcmp(mode, "stubborn") == 0) ? SIG_IGN : on_term);
    for (;;) ::sleep(1);
    return 0;
}
