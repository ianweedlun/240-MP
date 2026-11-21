// PATH-stub mixer for the P-5A volume-stall harness (checked-in).
//
// One binary, two identities: the orchestrator (run_volume_stall_case.py)
// copies it into a scratch bin dir as `amixer` and/or `wpctl` and puts that
// dir FIRST on PATH. Dispatch is on basename(argv[0]). Compiled (not a
// script) on purpose: interpreter startup (~40 ms for python) would dwarf
// the ~3 ms process-spawn cost the recorded 3970174 baseline was measured
// with, and would inflate the zero-delay control ~10x. usleep() is POSIX,
// so the delay is exact on Linux and macOS alike.
//
// Env contract (identical for both identities):
//   TORTURE_DELAY_MS    sleep this many ms BEFORE responding (default 0).
//                       Models a slow mixer; all delay lives in this stub.
//   TORTURE_MIXER_LOG   append the exact child argv (space-joined) as one
//                       line per invocation, BEFORE the sleep — failed/slow
//                       calls count too. The probe parses this for counts.
//   TORTURE_STUB_MODE   normal (default) | exit1 (nonzero exit, no output) |
//                       garbage (unparseable output, exit 0).
//   TORTURE_STUB_CARD   combined (default) | split. combined exposes one 'PCM'
//                       element carrying volume+switch (the Pi bcm2835 shape,
//                       byte-identical to the recorded baseline). split
//                       exposes 'PCM Playback Volume' + 'PCM Playback Switch'
//                       like a desktop card, with control-name validation, so
//                       the async backend's separate-switch addressing is
//                       testable without hardware (T1 P-5A ALSA conversion).
//   TORTURE_STUB_STATE  state file ("level muted" text) in the scratch dir;
//                       defaults to stub-state.txt next to the mixer log.
//                       Stateful so presses actually move the mixer:
//                       sset/set-mute update it, get/sget render it.
//                       Initial: level 50, unmuted (the lab's 50% fixture).
//
// Output shapes match what the real mixers' parsers key on:
//   amixer (src/audio/ALSAVolumeMixer.cpp): combined 'PCM' control (the Pi
//   bcm2835 family), [NN%] level rows, [on]/[off] playback switch rows.
//   wpctl (src/audio/PipeWireVolumeMixer.cpp): set-volume prints
//   'Volume: 0.NN'; set-mute is SILENT on success (the real wpctl is).
// Missing-executable mode needs no stub support: the orchestrator simply
// leaves the identity off PATH and the factory falls back to no-op.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

namespace {

struct State
{
    int level = 50;
    bool muted = false;
};

std::string statePath(const char *mixerLog)
{
    const char *env = std::getenv("TORTURE_STUB_STATE");
    if (env && *env) return env;
    std::string dir = mixerLog ? mixerLog : "";
    const size_t slash = dir.find_last_of('/');
    dir = (slash == std::string::npos) ? std::string(".") : dir.substr(0, slash);
    return dir + "/stub-state.txt";
}

State loadState(const std::string &path)
{
    State s;
    FILE *f = std::fopen(path.c_str(), "r");
    if (f) {
        int level = 0, muted = 0;
        if (std::fscanf(f, "%d %d", &level, &muted) == 2) {
            s.level = level;
            s.muted = muted != 0;
        }
        std::fclose(f);
    }
    return s;
}

void saveState(const std::string &path, const State &s)
{
    FILE *f = std::fopen(path.c_str(), "w");
    if (f) {
        std::fprintf(f, "%d %d\n", s.level, s.muted ? 1 : 0);
        std::fclose(f);
    }
}

void logArgv(int argc, char **argv)
{
    const char *log = std::getenv("TORTURE_MIXER_LOG");
    if (!log || !*log) return;
    FILE *f = std::fopen(log, "a");
    if (!f) return;
    for (int i = 1; i < argc; ++i)
        std::fprintf(f, "%s%s", i > 1 ? " " : "", argv[i]);
    std::fprintf(f, "\n");
    std::fclose(f);
}

void applyDelay()
{
    const char *env = std::getenv("TORTURE_DELAY_MS");
    const int ms = env ? std::atoi(env) : 0;
    if (ms > 0) usleep(static_cast<useconds_t>(ms) * 1000);
}

// Returns true when the run should stop with the given exit code (fault
// modes); false means "normal mode, continue".
bool faultMode(int *exitCode)
{
    const char *mode = std::getenv("TORTURE_STUB_MODE");
    if (!mode) return false;
    if (std::strcmp(mode, "exit1") == 0) { *exitCode = 1; return true; }
    if (std::strcmp(mode, "garbage") == 0) {
        // Must not contain anything the parsers key on: no '[NN%]', no
        // '[on]/[off]', no 'PCM'/'Master', no 'Volume'/'Mute'.
        std::fputs("!! garbage: not a mixer response !!\n", stdout);
        *exitCode = 0;
        return true;
    }
    return false;
}

void amixerBlock(const State &s)
{
    const char *sw = s.muted ? "off" : "on";
    std::printf("Simple mixer control 'PCM',0\n"
                "  Capabilities: pvolume pvolume-joined pswitch pswitch-joined\n"
                "  Playback channels: Front Left - Front Right\n"
                "  Front Left: Playback %d [%d%%] [%s]\n"
                "  Front Right: Playback %d [%d%%] [%s]\n",
                s.level, s.level, sw, s.level, s.level, sw);
}

bool splitCard()
{
    const char *env = std::getenv("TORTURE_STUB_CARD");
    return env && std::strcmp(env, "split") == 0;
}

// Split-card element blocks: the volume half carries no switch rows, the
// switch half no level rows — exactly how a desktop card names them.
void amixerVolumeBlock(const State &s)
{
    std::printf("Simple mixer control 'PCM Playback Volume',0\n"
                "  Capabilities: pvolume\n"
                "  Playback channels: Front Left - Front Right\n"
                "  Front Left: Playback %d [%d%%]\n"
                "  Front Right: Playback %d [%d%%]\n",
                s.level, s.level, s.level, s.level);
}

void amixerSwitchBlock(const State &s)
{
    std::printf("Simple mixer control 'PCM Playback Switch',0\n"
                "  Capabilities: pswitch pswitch-joined\n"
                "  Playback channels: Mono\n"
                "  Mono: Playback [%s]\n",
                s.muted ? "off" : "on");
}

int runAmixer(int argc, char **argv, const std::string &path)
{
    State s = loadState(path);
    const char *cmd = argc > 1 ? argv[1] : "";
    const bool split = splitCard();
    const char *ctl = argc > 2 ? argv[2] : "";
    const bool ctlIsVolume = split && std::strcmp(ctl, "PCM Playback Volume") == 0;
    const bool ctlIsSwitch = split && std::strcmp(ctl, "PCM Playback Switch") == 0;
    if (std::strcmp(cmd, "scontrols") == 0) {
        if (split)
            std::printf("Simple mixer control 'PCM Playback Volume',0\n"
                        "Simple mixer control 'PCM Playback Switch',0\n");
        else
            std::printf("Simple mixer control 'PCM',0\n");
    } else if (std::strcmp(cmd, "get") == 0 || std::strcmp(cmd, "sget") == 0) {
        if (split) {
            if (ctlIsVolume) amixerVolumeBlock(s);
            else if (ctlIsSwitch) amixerSwitchBlock(s);
            else {
                std::fprintf(stderr, "amixer-stub: unknown control '%s'\n", ctl);
                return 1;
            }
        } else {
            amixerBlock(s);
        }
    } else if (std::strcmp(cmd, "sset") == 0 && argc >= 4) {
        if (split && !ctlIsVolume) {
            std::fprintf(stderr, "amixer-stub: unknown control '%s'\n", ctl);
            return 1;
        }
        int v = 0;
        if (std::sscanf(argv[3], "%d%%", &v) != 1) {
            std::fprintf(stderr, "amixer-stub: bad percentage\n");
            return 1;
        }
        s.level = v < 0 ? 0 : (v > 100 ? 100 : v);
        saveState(path, s);
        if (split) amixerVolumeBlock(s);
        else amixerBlock(s);
    } else if (std::strcmp(cmd, "set") == 0 && argc >= 4 &&
               (std::strcmp(argv[3], "mute") == 0 || std::strcmp(argv[3], "unmute") == 0)) {
        if (split && !ctlIsSwitch) {
            std::fprintf(stderr, "amixer-stub: unknown control '%s'\n", ctl);
            return 1;
        }
        s.muted = std::strcmp(argv[3], "mute") == 0;
        saveState(path, s);
        if (split) amixerSwitchBlock(s);
        else amixerBlock(s);
    } else {
        std::fprintf(stderr, "amixer-stub: unknown command '%s'\n", cmd);
        return 1;
    }
    return 0;
}

int runWpctl(int argc, char **argv, const std::string &path)
{
    State s = loadState(path);
    const char *cmd = argc > 1 ? argv[1] : "";
    if (std::strcmp(cmd, "set-volume") == 0 && argc >= 4) {
        const double frac = std::atof(argv[3]);
        int v = static_cast<int>(frac * 100.0 + 0.5);
        s.level = v < 0 ? 0 : (v > 100 ? 100 : v);
        saveState(path, s);
        std::printf("Volume: %.2f\n", s.level / 100.0);
    } else if (std::strcmp(cmd, "get-volume") == 0) {
        std::printf("Volume: %.2f\n", s.level / 100.0);
    } else if (std::strcmp(cmd, "set-mute") == 0 && argc >= 4 &&
               (std::strcmp(argv[3], "true") == 0 || std::strcmp(argv[3], "false") == 0)) {
        s.muted = std::strcmp(argv[3], "true") == 0;
        saveState(path, s);
        // Real wpctl is silent on success; keep it that way.
    } else {
        std::fprintf(stderr, "wpctl-stub: unknown command '%s'\n", cmd);
        return 1;
    }
    return 0;
}

} // namespace

int main(int argc, char **argv)
{
    logArgv(argc, argv);
    applyDelay();
    int exitCode = 0;
    if (faultMode(&exitCode)) return exitCode;

    const std::string path = statePath(std::getenv("TORTURE_MIXER_LOG"));
    const char *base = std::strrchr(argv[0], '/');
    base = base ? base + 1 : argv[0];
    if (std::strcmp(base, "amixer") == 0) return runAmixer(argc, argv, path);
    if (std::strcmp(base, "wpctl") == 0) return runWpctl(argc, argv, path);
    std::fprintf(stderr, "mixer-stub: invoked as '%s', expected amixer or wpctl\n", base);
    return 1;
}