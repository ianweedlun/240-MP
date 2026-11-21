// Fake mixer child for the MixerProcessRunner unit tests (T1 P-5A).
// Stands in for amixer/wpctl so the seam is exercised with real subprocess
// lifecycle (spawn, delay, exit codes, garbage output) and no audio hardware.
//
// Usage: fake-mixer-child <delay-ms> <exit-code> <stdout-text> [stderr-text]
// Sleeps <delay-ms>, writes the texts verbatim, exits with <exit-code>.
// Pure stdlib on purpose: no Qt, no allocator surprises under the sanitizers.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int main(int argc, char **argv) {
    const int delayMs = argc > 1 ? std::atoi(argv[1]) : 0;
    const int exitCode = argc > 2 ? std::atoi(argv[2]) : 0;
    if (delayMs > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    if (argc > 3)
        std::fputs(argv[3], stdout);
    if (argc > 4)
        std::fputs(argv[4], stderr);
    return exitCode;
}