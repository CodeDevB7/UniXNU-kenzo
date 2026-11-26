// SPDX-License-Identifier: MIT
// Pre-launchd boot policy for Xiaomi Redmi Note 3 (Kenzo, 2 GiB RAM, iPadOS 17.7.11 21H461).
// Runs right after writable RamSparseDisk Data mounts at /private/var, before /sbin/launchd exec:
//   - sheng-disable-watchdogd=1 : disables com.apple.watchdogd (stops corpse storm)
//   - sheng-lite=1              : disables 21 deadlocking/RAM-heavy network, iCloud, cellular,
//                                 Bluetooth, and photo daemons (critical for 2 GiB RAM)
//   - sirius-skip-setup=1       : writes purplebuddy/springboard/migration/DataMigration plists
//                                 and /private/var/containers dirs to boot straight to SpringBoard
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <pexpert/pexpert.h>

static const char policy_head[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\"><dict>";
static const char policy_watchdogd[] = "<key>com.apple.watchdogd</key><true/>";
static const char policy_tail[] = "</dict></plist>\n";

static const char lite_jobs[] =
    "com.apple.identityservicesd,com.apple.locationd,com.apple.CommCenter,com.apple.bluetoothd,"
    "com.apple.photoanalysisd,com.apple.mediaanalysisd,com.apple.mediastream.mstreamd,com.apple.icloud.searchpartyd,"
    "com.apple.findmy.findmybeaconingd,com.apple.familycircled,com.apple.financed,com.apple.softwareupdateservicesd,"
    "com.apple.ndoagent,com.apple.biomesyncd,com.apple.email.maild,com.apple.amsengagementd,com.apple.adid,com.apple.eyereliefd,"
    "com.apple.fairplayd.A2,com.apple.passd,com.apple.mobile.obliteration";

static char policy[2048];
static char extra_jobs[200];
volatile int kenzo_boot_policy_status = -1;

#define PLIST_HEAD \
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" \
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" \
    "<plist version=\"1.0\"><dict>"
#define PLIST_TAIL "</dict></plist>\n"

static const struct {
    const char *path;
    const char *text;
} skip_setup_files[] = {
    {
        "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.plist",
        PLIST_HEAD
        "<key>SetupDone</key><true/><key>SetupFinishedAllSteps</key><true/>"
        "<key>SetupVersion</key><integer>11</integer><key>daemonizeNeedsToRun</key><false/>"
        PLIST_TAIL
    },
    {
        "/private/var/mobile/Library/Preferences/com.apple.purplebuddy.notbackedup.plist",
        PLIST_HEAD
        "<key>ShouldRunDiagnosticsMiniBuddy</key><false/><key>ShouldRunHSA2MiniBuddy</key><false/>"
        "<key>ShouldRunKeychainSyncMiniBuddy</key><false/><key>ShouldRunMesaMiniBuddy</key><false/>"
        "<key>ShouldRunPaymentMiniBuddy</key><false/><key>ShouldRunStandardMiniBuddy</key><false/>"
        "<key>SilentICDPUpgrade</key><false/>"
        PLIST_TAIL
    },
    {
        "/private/var/mobile/Library/Preferences/com.apple.managedconfiguration.notbackedup.plist",
        PLIST_HEAD
        "<key>LockdownActivationIndicatesCloudConfigurationAvailable</key><false/>"
        PLIST_TAIL
    },
    {
        "/private/var/mobile/Library/Preferences/com.apple.springboard.plist",
        PLIST_HEAD
        "<key>BKNoWatchdogs</key><true/>"
        "<key>FBLaunchWatchdogExceptions</key><dict><key>com.apple.purplebuddy</key><real>100</real></dict>"
        "<key>MCForceShowCloudConfigurationUI</key><false/>"
        "<key>SBDisableAutoDim</key><true/><key>SBDontDimOrLockOnAC</key><true/>"
        "<key>SBDisableIdleSleep</key><true/>"
        PLIST_TAIL
    },
    {
        "/private/var/mobile/Library/Preferences/com.apple.migration.plist",
        PLIST_HEAD
        "<key>LastSystemVersion</key><string>21H461</string>"
        "<key>DMUserDataDisposition</key><dict><key>SyntheticDidUpgrade</key><false/>"
        "<key>buildVersion</key><string>21H461</string></dict>"
        PLIST_TAIL
    },
    {
        "/private/var/mobile/Library/Preferences/com.apple.DataMigration.plist",
        PLIST_HEAD
        "<key>LastSystemVersion</key><string>21H461</string>"
        "<key>DMUserDataDisposition</key><dict><key>SyntheticDidUpgrade</key><false/>"
        "<key>buildVersion</key><string>21H461</string></dict>"
        PLIST_TAIL
    },
};

static int kenzo_write_file(proc_t p, uint64_t scratch, const char *path,
                            const char *text, int uid, int gid, int mode)
{
    int error, fd;
    int32_t result = 0;
    if ((error = copyout(path, scratch, strlen(path) + 1))) {
        return error;
    }
    struct open_args op = {
        .path = scratch,
        .flags = O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW,
        .mode = mode,
    };
    if ((error = open(p, &op, &result))) {
        return error;
    }
    fd = result;
    for (size_t sent = 0, total = strlen(text); sent < total && !error;) {
        size_t piece = (total - sent) < 0x1f0 ? (total - sent) : 0x1f0;
        user_ssize_t written = 0;
        if (!(error = copyout(text + sent, scratch + 0x200, piece))) {
            struct write_args wr = { .fd = fd, .cbuf = scratch + 0x200, .nbyte = piece };
            if (!(error = write(p, &wr, &written)) && written != (user_ssize_t)piece) {
                error = EIO;
            }
        }
        sent += piece;
    }
    if (!error) {
        struct fchown_args owner = { .fd = fd, .uid = uid, .gid = gid };
        error = fchown(p, &owner, &result);
    }
    if (!error) {
        struct fchmod_args chm = { .fd = fd, .mode = mode };
        error = fchmod(p, &chm, &result);
    }
    if (!error) {
        struct fsync_args sy = { .fd = fd };
        error = fsync(p, &sy, &result);
    }
    struct close_args cl = { .fd = fd };
    int ce = sys_close(p, &cl, &result);
    return error ? error : ce;
}

static void kenzo_mkdir_p(proc_t p, uint64_t scratch, const char *path, int mode)
{
    int32_t res = 0;
    if (!copyout(path, scratch, strlen(path) + 1)) {
        struct mkdir_args md = { .path = scratch, .mode = mode };
        (void)mkdir(p, &md, &res);
    }
}

static void kenzo_unlink_if_present(proc_t p, uint64_t scratch, const char *path)
{
    int32_t res = 0;
    if (!copyout(path, scratch, strlen(path) + 1)) {
        struct unlink_args rm = { .path = scratch };
        (void)unlink(p, &rm, &res);
    }
}

static void kenzo_apply_user_prefs(proc_t p, uint64_t scratch, int skip_setup)
{
    static const char *const dirs[] = {
        "/private/var/mobile",
        "/private/var/mobile/Library",
        "/private/var/mobile/Library/Preferences",
        "/private/var/containers",
        "/private/var/containers/Bundle",
        "/private/var/containers/Data",
        "/private/var/containers/Data/System",
        "/private/var/containers/Shared",
        "/private/var/containers/Shared/SystemGroup",
        "/private/var/containers/Temp",
    };
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        kenzo_mkdir_p(p, scratch, dirs[i], 0755);
    }

    const unsigned start = skip_setup ? 0 : 3;
    const unsigned count = sizeof(skip_setup_files) / sizeof(skip_setup_files[0]);
    for (unsigned i = start; i < count; ++i) {
        int err = kenzo_write_file(p, scratch, skip_setup_files[i].path,
                                   skip_setup_files[i].text, 501, 501, 0600);
        printf("KenzoBootPolicy: pref %s rc=%d\n", skip_setup_files[i].path, err);
    }

    kenzo_unlink_if_present(p, scratch, "/private/var/mobile/Library/Preferences/com.apple.MobileGestalt.plist");
    kenzo_unlink_if_present(p, scratch, "/private/var/mobile/Library/Preferences/com.apple.mobilegestalt.plist");
}

static int append_job(const char **cursor)
{
    const char *c = *cursor;
    const char *e = c;
    while (*e && *e != ',') {
        ++e;
    }
    if (e > c) {
        if (strlcat(policy, "<key>", sizeof(policy)) >= sizeof(policy)) {
            return EINVAL;
        }
        size_t at = strlen(policy);
        if (at + (size_t)(e - c) + 1 >= sizeof(policy)) {
            return EINVAL;
        }
        memcpy(policy + at, c, (size_t)(e - c));
        policy[at + (size_t)(e - c)] = 0;
        if (strlcat(policy, "</key><true/>", sizeof(policy)) >= sizeof(policy)) {
            return EINVAL;
        }
    }
    *cursor = *e ? e + 1 : e;
    return 0;
}

int kenzo_boot_policy(void *process, uint64_t scratch)
{
    proc_t p = (proc_t)process;
    int disable_wdt = 0;
    int lite = 0;
    int skip_setup = 0;
    int error = 0;
    int fd = -1;
    int32_t result = 0;

    PE_parse_boot_argn("sheng-disable-watchdogd", &disable_wdt, sizeof(disable_wdt));
    PE_parse_boot_argn("sheng-lite", &lite, sizeof(lite));
    PE_parse_boot_argn("sirius-skip-setup", &skip_setup, sizeof(skip_setup));
    if (!PE_parse_boot_argn("sheng-disable-jobs", extra_jobs, sizeof(extra_jobs))) {
        extra_jobs[0] = 0;
    }
    extra_jobs[sizeof(extra_jobs) - 1] = 0;

    kenzo_apply_user_prefs(p, scratch, skip_setup == 1);

    if (disable_wdt != 1 && lite != 1 && !extra_jobs[0]) {
        kenzo_boot_policy_status = 0;
        return 0;
    }

    for (const char *c = extra_jobs; *c; ++c) {
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') ||
              (*c >= '0' && *c <= '9') || *c == '.' || *c == '-' || *c == ',')) {
            kenzo_boot_policy_status = EINVAL;
            return EINVAL;
        }
    }

    policy[0] = 0;
    strlcat(policy, policy_head, sizeof(policy));
    if (disable_wdt == 1) {
        strlcat(policy, policy_watchdogd, sizeof(policy));
    }
    if (lite == 1) {
        for (const char *c = lite_jobs; *c;) {
            if ((error = append_job(&c))) {
                kenzo_boot_policy_status = error;
                return error;
            }
        }
    }
    for (const char *c = extra_jobs; *c;) {
        if ((error = append_job(&c))) {
            kenzo_boot_policy_status = error;
            return error;
        }
    }
    strlcat(policy, policy_tail, sizeof(policy));

    kenzo_mkdir_p(p, scratch, "/private/var/db", 0755);
    kenzo_mkdir_p(p, scratch, "/private/var/db/com.apple.xpc.launchd", 0755);

    error = kenzo_write_file(p, scratch, "/private/var/db/com.apple.xpc.launchd/disabled.plist",
                             policy, 0, 0, 0644);
    if (!error) {
        const char *marker = "/private/var/db/com.apple.xpc.launchd/disabled.migrated";
        if (!(error = copyout(marker, scratch, strlen(marker) + 1))) {
            struct open_args mark = {
                .path = scratch,
                .flags = O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW,
                .mode = 0644,
            };
            int merr = open(p, &mark, &result);
            if (!merr) {
                fd = result;
                struct close_args cl = { .fd = fd };
                error = sys_close(p, &cl, &result);
            } else if (merr != EEXIST) {
                error = merr;
            }
        }
    }
    kenzo_boot_policy_status = error;
    printf("KenzoBootPolicy: launchd disabled.plist rc=%d (wdt=%d lite=%d skip_setup=%d)\n",
           error, disable_wdt == 1, lite == 1, skip_setup == 1);
    return error;
}

static int kenzo_is_ui_proc(const char *name)
{
    if (!name || !name[0]) {
        return 0;
    }
    return strcmp(name, "SpringBoard") == 0 ||
           strcmp(name, "backboardd") == 0 ||
           strcmp(name, "Setup") == 0 ||
           strcmp(name, "PosterBoard") == 0 ||
           strcmp(name, "iconservicesagent") == 0 ||
           strcmp(name, "com.apple.datamigrator") == 0 ||
           strcmp(name, "datamigrator") == 0;
}

void kenzo_trace_exec(int pid, const char *name)
{
    if (kenzo_is_ui_proc(name)) {
        printf("KENZO EXEC: pid=%d %s\n", pid, name);
    }
}

void kenzo_trace_exit(int pid, const char *name, int rv, unsigned ns, unsigned long long code)
{
    const int sig = rv & 0x7f;
    if (kenzo_is_ui_proc(name) || ns == 1U || (sig != 0 && sig != 0x7f)) {
        printf("KENZO EXIT: pid=%d %s rv=0x%x sig=%d ns=%u code=0x%llx\n",
               pid, (name && name[0]) ? name : "?", rv, sig, ns, code);
    }
}
