/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2014 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <unistd.h>

#include "util.h"
#include "special.h"
#include "mkdir.h"
#include "unit-name.h"
#include "generator.h"
#include "path-util.h"
#include "fstab-util.h"
#include "fileio.h"
#include "dropin.h"

static int write_fsck_sysroot_service(const char *dir, const char *what) {
        const char *unit;
        _cleanup_free_ char *device = NULL;
        _cleanup_fclose_ FILE *f = NULL;
        int r;

        unit = strjoina(dir, "/systemd-fsck-root.service");
        log_debug("Creating %s", unit);

        r = unit_name_from_path(what, ".device", &device);
        if (r < 0)
                return log_error_errno(r, "Failed to convert device \"%s\" to unit name: %m", what);

        f = fopen(unit, "wxe");
        if (!f)
                return log_error_errno(errno, "Failed to create unit file %s: %m", unit);

        fprintf(f,
                "# Automatically generated by %1$s\n\n"
                "[Unit]\n"
                "Documentation=man:systemd-fsck-root.service(8)\n"
                "Description=File System Check on %2$s\n"
                "DefaultDependencies=no\n"
                "BindsTo=%3$s\n"
                "After=%3$s\n"
                "Before=shutdown.target\n"
                "\n"
                "[Service]\n"
                "Type=oneshot\n"
                "RemainAfterExit=yes\n"
                "ExecStart=" SYSTEMD_FSCK_PATH " %2$s\n"
                "TimeoutSec=0\n",
                program_invocation_short_name,
                what,
                device);

        r = fflush_and_check(f);
        if (r < 0)
                return log_error_errno(r, "Failed to write unit file %s: %m", unit);

        return 0;
}

int generator_write_fsck_deps(
                FILE *f,
                const char *dir,
                const char *what,
                const char *where,
                const char *fstype) {

        int r;

        assert(f);
        assert(dir);
        assert(what);
        assert(where);

        if (!is_device_path(what)) {
                log_warning("Checking was requested for \"%s\", but it is not a device.", what);
                return 0;
        }

        if (!isempty(fstype) && !streq(fstype, "auto")) {
                r = fsck_exists(fstype);
                if (r == -ENOENT) {
                        /* treat missing check as essentially OK */
                        log_debug_errno(r, "Checking was requested for %s, but fsck.%s does not exist: %m", what, fstype);
                        return 0;
                } else if (r < 0)
                        return log_warning_errno(r, "Checking was requested for %s, but fsck.%s cannot be used: %m", what, fstype);
        }

        if (path_equal(where, "/")) {
                char *lnk;

                lnk = strjoina(dir, "/" SPECIAL_LOCAL_FS_TARGET ".wants/systemd-fsck-root.service");

                mkdir_parents(lnk, 0755);
                if (symlink(SYSTEM_DATA_UNIT_PATH "/systemd-fsck-root.service", lnk) < 0)
                        return log_error_errno(errno, "Failed to create symlink %s: %m", lnk);

        } else {
                _cleanup_free_ char *_fsck = NULL;
                const char *fsck;

                if (in_initrd() && path_equal(where, "/sysroot")) {
                        r = write_fsck_sysroot_service(dir, what);
                        if (r < 0)
                                return r;

                        fsck = "systemd-fsck-root.service";
                } else {
                        r = unit_name_from_path_instance("systemd-fsck", what, ".service", &_fsck);
                        if (r < 0)
                                return log_error_errno(r, "Failed to create fsck service name: %m");

                        fsck = _fsck;
                }

                fprintf(f,
                        "RequiresOverridable=%1$s\n"
                        "After=%1$s\n",
                        fsck);
        }

        return 0;
}

int generator_write_timeouts(
                const char *dir,
                const char *what,
                const char *where,
                const char *opts,
                char **filtered) {

        /* Allow configuration how long we wait for a device that
         * backs a mount point to show up. This is useful to support
         * endless device timeouts for devices that show up only after
         * user input, like crypto devices. */

        _cleanup_free_ char *node = NULL, *unit = NULL, *timeout = NULL;
        usec_t u;
        int r;

        r = fstab_filter_options(opts, "comment=systemd.device-timeout\0" "x-systemd.device-timeout\0",
                                 NULL, &timeout, filtered);
        if (r <= 0)
                return r;

        r = parse_sec(timeout, &u);
        if (r < 0) {
                log_warning("Failed to parse timeout for %s, ignoring: %s", where, timeout);
                return 0;
        }

        node = fstab_node_to_udev_node(what);
        if (!node)
                return log_oom();

        r = unit_name_from_path(node, ".device", &unit);
        if (r < 0)
                return log_error_errno(r, "Failed to make unit name from path: %m");

        return write_drop_in_format(dir, unit, 50, "device-timeout",
                                    "# Automatically generated by %s\n\n"
                                    "[Unit]\nJobTimeoutSec=" USEC_FMT,
                                    program_invocation_short_name,
                                    u / USEC_PER_SEC);
}
