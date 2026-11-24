// CFG-01 killprobe — pure POSIX timing path (no Qt anywhere near the kill
// clock). Forks the real cfg01-saver child per trial, SIGKILLs at controlled
// µs offsets after the child's ready marker, archives per-trial byte snapshots
// + dir listings + raw row metadata. Classification is a SEPARATE Qt pass
// (cfg01-classify batch) — §4.1.
//
// Usage: cfg01-killprobe <saver-bin> <evdir> <max-offset-us> <step-us> <repeats>
//   template: <evdir>/template.json (copied fresh to each trial's live file)
//   trials:   <evdir>/trials/tNNN/{data,approot,ready,config-after.json,
//             listing.txt,stderr.log}
//   rows:     <evdir>/raw-rows.jsonl (kill rows, then 3 RLIMIT rows — G3)
//
// Exit: 0 = sweep completed rig-valid; 3 = ready-marker timeout (rig-validity,
// trial not counted); 1 = setup error. NO product verdict here.
//
// D-7: unlink(ready) BEFORE each spawn (stale-marker race fix, mandatory).

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

namespace {

std::string g_saver, g_evdir;

bool copyFile(const std::string &src, const std::string &dst)
{
    int in = ::open(src.c_str(), O_RDONLY);
    if (in < 0) return false;
    int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { ::close(in); return false; }
    char buf[65536];
    ssize_t n;
    while ((n = ::read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = ::write(out, buf + off, n - off);
            if (w <= 0) { ::close(in); ::close(out); return false; }
            off += w;
        }
    }
    ::close(in); ::close(out);
    return n == 0;
}

std::string trialDir(int t)
{
    char buf[32];
    snprintf(buf, sizeof buf, "/trials/t%03d", t);
    return g_evdir + buf;
}

void archiveListing(int t)
{
    const std::string data = trialDir(t) + "/data";
    DIR *d = ::opendir(data.c_str());
    if (!d) return;
    const std::string lp = trialDir(t) + "/listing.txt";
    FILE *f = ::fopen(lp.c_str(), "w");
    struct dirent *e;
    while (f && (e = ::readdir(d)) != nullptr) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        fprintf(f, "%s\n", e->d_name);
    }
    if (f) fclose(f);
    ::closedir(d);
}

void appendRow(const std::string &json)
{
    const std::string p = g_evdir + "/raw-rows.jsonl";
    FILE *f = ::fopen(p.c_str(), "a");
    if (f) { fprintf(f, "%s\n", json.c_str()); fclose(f); }
}

long long fileSize(const std::string &p)
{
    struct stat st;
    if (::stat(p.c_str(), &st) != 0) return -1;
    return (long long)st.st_size;
}

// One kill trial. Returns false on ready-timeout (rig-validity).
bool killTrial(int t, int offsetUs)
{
    const std::string td = trialDir(t);
    const std::string data = td + "/data";
    ::mkdir(td.c_str(), 0755);
    ::mkdir(data.c_str(), 0755);
    const std::string approot = td + "/approot";
    ::mkdir(approot.c_str(), 0755);
    if (!copyFile(g_evdir + "/template.json", data + "/config.json")) {
        fprintf(stderr, "RIG ERROR: template copy failed (trial %d)\n", t);
        exit(1);
    }
    const std::string ready = td + "/ready";
    ::unlink(ready.c_str());   // D-7 mandatory

    const std::string errlog = td + "/stderr.log";
    pid_t pid = ::fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        int efd = ::open(errlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0) { ::dup2(efd, 2); ::close(efd); }
        int nfd = ::open("/dev/null", O_WRONLY);
        if (nfd >= 0) { ::dup2(nfd, 1); ::close(nfd); }
        ::execl(g_saver.c_str(), "cfg01-saver", "save", data.c_str(), (char *)nullptr);
        _exit(127);
    }

    // Poll the ready marker at 100 µs granularity, 10 s bound.
    long long waited = 0;
    struct stat st;
    while (::stat(ready.c_str(), &st) != 0) {
        ::usleep(100);
        waited += 100;
        if (waited > 10000000) {
            ::kill(pid, SIGKILL);
            int ws; ::waitpid(pid, &ws, 0);
            fprintf(stderr, "RIG-VALIDITY: ready timeout trial %d\n", t);
            return false;
        }
    }
    ::usleep(offsetUs);
    ::kill(pid, SIGKILL);
    int ws = 0;
    ::waitpid(pid, &ws, 0);

    // Snapshot live bytes + listing.
    copyFile(data + "/config.json", td + "/config-after.json");
    archiveListing(t);
    char row[512];
    snprintf(row, sizeof row,
             "{\"trial\":%d,\"kind\":\"kill\",\"offset_us\":%d,\"size\":%lld,"
             "\"exit_code\":%d,\"signal\":%d,\"ready_latency_us\":%lld}",
             t, offsetUs, fileSize(data + "/config.json"),
             WIFEXITED(ws) ? WEXITSTATUS(ws) : -1,
             WIFSIGNALED(ws) ? WTERMSIG(ws) : 0, waited);
    appendRow(row);
    return true;
}

// G3 RLIMIT trial: no kill; saver runs to completion under RLIMIT_FSIZE=1024.
void rlimitTrial(int t)
{
    const std::string td = trialDir(t);
    const std::string data = td + "/data";
    ::mkdir(td.c_str(), 0755);
    ::mkdir(data.c_str(), 0755);
    ::mkdir((td + "/approot").c_str(), 0755);
    if (!copyFile(g_evdir + "/template.json", data + "/config.json")) exit(1);
    ::unlink((td + "/ready").c_str());

    const std::string errlog = td + "/stderr.log";
    pid_t pid = ::fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid == 0) {
        int efd = ::open(errlog.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (efd >= 0) { ::dup2(efd, 2); ::close(efd); }
        int nfd = ::open("/dev/null", O_WRONLY);
        if (nfd >= 0) { ::dup2(nfd, 1); ::close(nfd); }
        ::setenv("CFG01_FSIZE_LIMIT", "1024", 1);
        ::execl(g_saver.c_str(), "cfg01-saver", "save", data.c_str(), (char *)nullptr);
        _exit(127);
    }
    int ws = 0;
    ::waitpid(pid, &ws, 0);
    copyFile(data + "/config.json", td + "/config-after.json");
    archiveListing(t);
    char row[512];
    snprintf(row, sizeof row,
             "{\"trial\":%d,\"kind\":\"rlimit\",\"offset_us\":-1,\"size\":%lld,"
             "\"exit_code\":%d,\"signal\":%d,\"ready_latency_us\":-1}",
             t, fileSize(data + "/config.json"),
             WIFEXITED(ws) ? WEXITSTATUS(ws) : -1,
             WIFSIGNALED(ws) ? WTERMSIG(ws) : 0);
    appendRow(row);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 6) {
        fprintf(stderr, "usage: cfg01-killprobe <saver-bin> <evdir> <max-us> <step-us> <repeats>\n");
        return 1;
    }
    g_saver = argv[1];
    g_evdir = argv[2];
    const int maxUs = atoi(argv[3]);
    const int stepUs = atoi(argv[4]);
    const int repeats = atoi(argv[5]);
    if (stepUs <= 0 || repeats <= 0 || maxUs < 0) return 1;

    ::mkdir((g_evdir + "/trials").c_str(), 0755);
    ::unlink((g_evdir + "/raw-rows.jsonl").c_str());

    int t = 0;
    for (int off = 0; off <= maxUs; off += stepUs) {
        for (int r = 0; r < repeats; ++r) {
            if (!killTrial(t, off)) return 3;
            ++t;
        }
    }
    const int killCount = t;
    for (int r = 0; r < 3; ++r) rlimitTrial(t++);
    printf("KILLPROBE done: %d kill trials + %d rlimit trials (grid 0..%d us step %d x%d)\n",
           killCount, 3, maxUs, stepUs, repeats);
    return 0;
}