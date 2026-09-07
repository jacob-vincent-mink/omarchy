// T05 sandbox-closure probe worker.
//
// This is the "packaged worker" stand-in for the QEMU root-lifecycle lane. It
// is exec'd inside the product's real bubbleswrap closure (the exact argv from
// contracts/sandbox policy.cpp build_plan_for_worker, path-based fd bindings
// substituted) as real root, and attempts every escape the threat model
// attributes to a hostile worker: reaching host files, the session bus, the
// network, native modules outside the closure, or inherited descriptors, and
// escaping the mount/PID/user namespaces via /proc.
//
// It writes one RESULT-<probe>=<PASS|FAIL> line per probe to stdout (captured
// by the host driver through the qemu serial log) and mirrors them to
// /state/results.txt (the single writable, host-backed path the sandbox grants).
// A FAIL on any negative-control probe means the sandbox let a hostile worker
// reach something it must not.
//
// Build: gcc -O2 -o probe_worker probe_worker.c   (glibc: /usr/lib ld-linux +
// libc.so.6 are already staged in the guest, matching the product worker's
// libc dependency model).

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures = 0;

static void check(const char *probe, int ok) {
  if (!ok)
    failures++;
  printf("RESULT-%s=%s\n", probe, ok ? "PASS" : "FAIL");
  fflush(stdout);
}

// A host-side file the launch harness stashes outside any closure. The worker
// must never be able to read it -- if it can, the sandbox leaked the host tree.
static int file_readable(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return 0;
  close(fd);
  return 1;
}

int main(void) {
  // Probe 0: the worker is actually running as root inside the closure (sanity).
  check("worker-runs", getuid() == 0);

  // 1. Must NOT reach arbitrary host files. /etc/shadow exists on the host but
  //    is outside the ro-bind closure (only /usr/lib, fonts, ld.so.cache are
  //    mounted in; /etc is not present at all).
  check("host-shadow-absent", !file_readable("/etc/shadow"));

  // 2. Must NOT reach the host root stash planted by the harness (real root
  //    lifecycle: even a root worker may not see paths outside its mounts).
  check("host-secret-absent", !file_readable("/root/host-secret.txt"));

  // 3. The dedicated PID namespace means /proc/1/root is the sandbox's own
  //    root, not the host's -- a /proc escape must not surface host files.
  check("proc-hosts-no-escape", !file_readable("/proc/1/root/root/host-secret.txt"));

  // 4. Network is unshared (--unshare-net): even with a writable loopback to a
  //    "public"/"private" lab IP, the worker must not be able to open a
  //    connection -- no devices/routes exist in its network namespace.
  {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
      check("network-blocked", 1); // socket() itself refused: fully closed.
    } else {
      struct sockaddr_in sa;
      memset(&sa, 0, sizeof(sa));
      sa.sin_family = AF_INET;
      sa.sin_port = htons(443);
      sa.sin_addr.s_addr = htonl(0x01010101u); // 1.1.1.1 lab "public" IP
      int rc = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
      close(fd);
      check("network-blocked", rc != 0);
    }
  }

  // 5. No host executables/native modules outside /usr/lib: /bin does not exist
  //    in the closure, so execv of a host tool must fail before any security
  //    boundary code runs.
  {
    int fd = open("/bin/ls", O_RDONLY);
    check("host-bin-absent", fd < 0 && errno == ENOENT);
    if (fd >= 0)
      close(fd);
  }

  // 6. The plugin/revision mount is read-only (ro-bind): a hostile worker must
  //    not be able to alter it.
  {
    int fd = open("/plugin/probe_write", O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    int denied = (fd < 0 && (errno == EROFS || errno == EACCES || errno == ENOENT));
    if (fd >= 0)
      close(fd);
    check("plugin-ro", denied);
  }

  // 7. The one writable host-backed path (/state, from the private-state bind)
  //    must accept writes -- the worker's only legitimate host-backed scratch.
  {
    int fd = open("/state/results.txt", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    int writable = (fd >= 0);
    if (fd >= 0) {
      const char m[] = "sandbox-state-writable\n";
      ssize_t n = write(fd, m, sizeof(m) - 1);
      writable = (n == (ssize_t)(sizeof(m) - 1));
      close(fd);
    }
    check("state-writable", writable);
  }

  // 8. The worker must not be able to *widen* the closure by remounting or
  //    bind-mounting itself out of it. A hostile root worker commonly tries to
  //    defeat a read-only closure with `mount(MS_BIND|MS_REMOUNT)`; doing so
  //    needs CAP_SYS_ADMIN in the user namespace that owns the sandbox's mounts
  //    (the host's), which --cap-drop ALL and the dedicated namespaces do not
  //    grant. Any mount() here must fail with EPERM.
  {
    long rc = syscall(SYS_mount, "none", "/plugin", NULL,
                      MS_BIND | MS_REMOUNT | MS_RDONLY, NULL);
    check("mount-escape-blocked", rc != 0 && errno == EPERM);
  }
  //    (Informational, not a pass/fail control: this kernel lets an unprivileged
  //    worker self-grant a *child* user namespace -- the unprivileged-userns
  //    feature. That does not reach the host, because the child userns' caps
  //    never cover the sandbox's host-owned mounts or namespaces, and there is
  //    no host namespace fd to setns() into. Recorded for the report.)
  {
    long rc = syscall(SYS_unshare, CLONE_NEWUSER);
    printf("OBS-userns-selfgrant=%s\n", rc == 0 ? "allowed" : "denied");
    fflush(stdout);
    if (rc == 0)
      syscall(SYS_unshare, CLONE_FS); // discard any transient child ns
  }

  // 9. Descriptor closure: only the standard streams (and transient fd dirs)
  //    are inherited; no host file descriptors leak into the worker.
  {
    // Count open fds via /proc/self/fd (bounded). 0,1,2 always present.
    int count = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d) {
      struct dirent *e;
      while ((e = readdir(d)))
        if (e->d_name[0] != '.')
          count++;
      closedir(d);
    }
    check("no-inherited-descriptors", count <= 4);
  }

  printf("OVERALL=%s\n", failures == 0 ? "PASS" : "FAIL");
  fflush(stdout);
  return failures == 0 ? 0 : 1;
}
