/* SPDX-License-Identifier: LGPL-2.1+ */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "sd-bus.h"
#include "sd-daemon.h"
#include "sd-event.h"
#include "sd-id128.h"

#include "alloc-util.h"
#include "bus-internal.h"
#include "bus-label.h"
#include "bus-message.h"
#include "bus-util.h"
#include "cap-list.h"
#include "cgroup-util.h"
#include "mountpoint-util.h"
#include "nsflags.h"
#include "parse-util.h"
#include "path-util.h"
#include "rlimit-util.h"
#include "socket-util.h"
#include "stdio-util.h"
#include "strv.h"
#include "user-util.h"

static int name_owner_change_callback(sd_bus_message *m, void *userdata, sd_bus_error *ret_error) {
        sd_event *e = userdata;

        assert(m);
        assert(e);

        sd_bus_close(sd_bus_message_get_bus(m));
        sd_event_exit(e, 0);

        return 1;
}

int bus_async_unregister_and_exit(sd_event *e, sd_bus *bus, const char *name) {
        const char *match;
        const char *unique;
        int r;

        assert(e);
        assert(bus);
        assert(name);

        /* We unregister the name here and then wait for the
         * NameOwnerChanged signal for this event to arrive before we
         * quit. We do this in order to make sure that any queued
         * requests are still processed before we really exit. */

        r = sd_bus_get_unique_name(bus, &unique);
        if (r < 0)
                return r;

        match = strjoina(
                        "sender='org.freedesktop.DBus',"
                        "type='signal',"
                        "interface='org.freedesktop.DBus',"
                        "member='NameOwnerChanged',"
                        "path='/org/freedesktop/DBus',"
                        "arg0='", name, "',",
                        "arg1='", unique, "',",
                        "arg2=''");

        r = sd_bus_add_match_async(bus, NULL, match, name_owner_change_callback, NULL, e);
        if (r < 0)
                return r;

        r = sd_bus_release_name_async(bus, NULL, name, NULL, NULL);
        if (r < 0)
                return r;

        return 0;
}

int bus_event_loop_with_idle(
                sd_event *e,
                sd_bus *bus,
                const char *name,
                usec_t timeout,
                check_idle_t check_idle,
                void *userdata) {
        bool exiting = false;
        int r, code;

        assert(e);
        assert(bus);
        assert(name);

        for (;;) {
                bool idle;

                r = sd_event_get_state(e);
                if (r < 0)
                        return r;
                if (r == SD_EVENT_FINISHED)
                        break;

                if (check_idle)
                        idle = check_idle(userdata);
                else
                        idle = true;

                r = sd_event_run(e, exiting || !idle ? (uint64_t) -1 : timeout);
                if (r < 0)
                        return r;

                if (r == 0 && !exiting && idle) {

                        r = sd_bus_try_close(bus);
                        if (r == -EBUSY)
                                continue;

                        /* Fallback for dbus1 connections: we
                         * unregister the name and wait for the
                         * response to come through for it */
                        if (r == -EOPNOTSUPP) {

                                /* Inform the service manager that we
                                 * are going down, so that it will
                                 * queue all further start requests,
                                 * instead of assuming we are already
                                 * running. */
                                sd_notify(false, "STOPPING=1");

                                r = bus_async_unregister_and_exit(e, bus, name);
                                if (r < 0)
                                        return r;

                                exiting = true;
                                continue;
                        }

                        if (r < 0)
                                return r;

                        sd_event_exit(e, 0);
                        break;
                }
        }

        r = sd_event_get_exit_code(e, &code);
        if (r < 0)
                return r;

        return code;
}

int bus_name_has_owner(sd_bus *c, const char *name, sd_bus_error *error) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *rep = NULL;
        int r, has_owner = 0;

        assert(c);
        assert(name);

        r = sd_bus_call_method(c,
                               "org.freedesktop.DBus",
                               "/org/freedesktop/dbus",
                               "org.freedesktop.DBus",
                               "NameHasOwner",
                               error,
                               &rep,
                               "s",
                               name);
        if (r < 0)
                return r;

        r = sd_bus_message_read_basic(rep, 'b', &has_owner);
        if (r < 0)
                return sd_bus_error_set_errno(error, r);

        return has_owner;
}

int bus_check_peercred(sd_bus *c) {
        struct ucred ucred;
        int fd, r;

        assert(c);

        fd = sd_bus_get_fd(c);
        if (fd < 0)
                return fd;

        r = getpeercred(fd, &ucred);
        if (r < 0)
                return r;

        if (ucred.uid != 0 && ucred.uid != geteuid())
                return -EPERM;

        return 1;
}

int bus_connect_system_systemd(sd_bus **_bus) {
        _cleanup_(sd_bus_close_unrefp) sd_bus *bus = NULL;
        int r;

        assert(_bus);

        if (geteuid() != 0)
                return sd_bus_default_system(_bus);

        /* If we are root then let's talk directly to the system
         * instance, instead of going via the bus */

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        r = sd_bus_set_address(bus, "unix:path=/run/systemd/private");
        if (r < 0)
                return r;

        r = sd_bus_start(bus);
        if (r < 0)
                return sd_bus_default_system(_bus);

        r = bus_check_peercred(bus);
        if (r < 0)
                return r;

        *_bus = TAKE_PTR(bus);

        return 0;
}

int bus_connect_user_systemd(sd_bus **_bus) {
        _cleanup_(sd_bus_close_unrefp) sd_bus *bus = NULL;
        _cleanup_free_ char *ee = NULL;
        const char *e;
        int r;

        assert(_bus);

        e = secure_getenv("XDG_RUNTIME_DIR");
        if (!e)
                return sd_bus_default_user(_bus);

        ee = bus_address_escape(e);
        if (!ee)
                return -ENOMEM;

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        bus->address = strjoin("unix:path=", ee, "/systemd/private");
        if (!bus->address)
                return -ENOMEM;

        r = sd_bus_start(bus);
        if (r < 0)
                return sd_bus_default_user(_bus);

        r = bus_check_peercred(bus);
        if (r < 0)
                return r;

        *_bus = TAKE_PTR(bus);

        return 0;
}

int bus_print_property_value(const char *name, const char *expected_value, bool only_value, const char *value) {
        assert(name);

        if (expected_value && !streq_ptr(expected_value, value))
                return 0;

        if (only_value)
                puts(value);
        else
                printf("%s=%s\n", name, value);

        return 0;
}

int bus_print_property_valuef(const char *name, const char *expected_value, bool only_value, const char *fmt, ...) {
        va_list ap;
        int r;

        assert(name);
        assert(fmt);

        if (expected_value) {
                _cleanup_free_ char *s = NULL;

                va_start(ap, fmt);
                r = vasprintf(&s, fmt, ap);
                va_end(ap);
                if (r < 0)
                        return -ENOMEM;

                if (streq_ptr(expected_value, s)) {
                        if (only_value)
                                puts(s);
                        else
                                printf("%s=%s\n", name, s);
                }

                return 0;
        }

        if (!only_value)
                printf("%s=", name);
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        puts("");

        return 0;
}

static int bus_print_property(const char *name, const char *expected_value, sd_bus_message *m, bool value, bool all) {
        char type;
        const char *contents;
        int r;

        assert(name);
        assert(m);

        r = sd_bus_message_peek_type(m, &type, &contents);
        if (r < 0)
                return r;

        switch (type) {

        case SD_BUS_TYPE_STRING: {
                const char *s;

                r = sd_bus_message_read_basic(m, type, &s);
                if (r < 0)
                        return r;

                if (all || !isempty(s)) {
                        bool good;

                        /* This property has a single value, so we need to take
                         * care not to print a new line, everything else is OK. */
                        good = !strchr(s, '\n');
                        bus_print_property_value(name, expected_value, value, good ? s : "[unprintable]");
                }

                return 1;
        }

        case SD_BUS_TYPE_BOOLEAN: {
                int b;

                r = sd_bus_message_read_basic(m, type, &b);
                if (r < 0)
                        return r;

                if (expected_value && parse_boolean(expected_value) != b)
                        return 1;

                bus_print_property_value(name, NULL, value, yes_no(b));
                return 1;
        }

        case SD_BUS_TYPE_UINT64: {
                uint64_t u;

                r = sd_bus_message_read_basic(m, type, &u);
                if (r < 0)
                        return r;

                /* Yes, heuristics! But we can change this check
                 * should it turn out to not be sufficient */

                if (endswith(name, "Timestamp") ||
                    STR_IN_SET(name, "NextElapseUSecRealtime", "LastTriggerUSec", "TimeUSec", "RTCTimeUSec")) {
                        char timestamp[FORMAT_TIMESTAMP_MAX];
                        const char *t;

                        t = format_timestamp(timestamp, sizeof(timestamp), u);
                        if (t || all)
                                bus_print_property_value(name, expected_value, value, strempty(t));

                } else if (strstr(name, "USec")) {
                        char timespan[FORMAT_TIMESPAN_MAX];

                        (void) format_timespan(timespan, sizeof(timespan), u, 0);
                        bus_print_property_value(name, expected_value, value, timespan);

                } else if (streq(name, "RestrictNamespaces")) {
                        _cleanup_free_ char *s = NULL;
                        const char *result;

                        if ((u & NAMESPACE_FLAGS_ALL) == 0)
                                result = "yes";
                        else if (FLAGS_SET(u, NAMESPACE_FLAGS_ALL))
                                result = "no";
                        else {
                                r = namespace_flags_to_string(u, &s);
                                if (r < 0)
                                        return r;

                                result = strempty(s);
                        }

                        bus_print_property_value(name, expected_value, value, result);

                } else if (streq(name, "MountFlags")) {
                        const char *result;

                        result = mount_propagation_flags_to_string(u);
                        if (!result)
                                return -EINVAL;

                        bus_print_property_value(name, expected_value, value, result);

                } else if (STR_IN_SET(name, "CapabilityBoundingSet", "AmbientCapabilities")) {
                        _cleanup_free_ char *s = NULL;

                        r = capability_set_to_string_alloc(u, &s);
                        if (r < 0)
                                return r;

                        bus_print_property_value(name, expected_value, value, s);

                } else if ((STR_IN_SET(name, "CPUWeight", "StartupCPUWeight", "IOWeight", "StartupIOWeight") && u == CGROUP_WEIGHT_INVALID) ||
                           (STR_IN_SET(name, "CPUShares", "StartupCPUShares") && u == CGROUP_CPU_SHARES_INVALID) ||
                           (STR_IN_SET(name, "BlockIOWeight", "StartupBlockIOWeight") && u == CGROUP_BLKIO_WEIGHT_INVALID) ||
                           (STR_IN_SET(name, "MemoryCurrent", "TasksCurrent") && u == (uint64_t) -1) ||
                           (endswith(name, "NSec") && u == (uint64_t) -1))

                        bus_print_property_value(name, expected_value, value, "[not set]");

                else if ((STR_IN_SET(name, "DefaultMemoryLow", "DefaultMemoryMin", "MemoryLow", "MemoryHigh", "MemoryMax", "MemorySwapMax", "MemoryLimit") && u == CGROUP_LIMIT_MAX) ||
                         (STR_IN_SET(name, "TasksMax", "DefaultTasksMax") && u == (uint64_t) -1) ||
                         (startswith(name, "Limit") && u == (uint64_t) -1) ||
                         (startswith(name, "DefaultLimit") && u == (uint64_t) -1))

                        bus_print_property_value(name, expected_value, value, "infinity");
                else if (STR_IN_SET(name, "IPIngressBytes", "IPIngressPackets", "IPEgressBytes", "IPEgressPackets") && u == (uint64_t) -1)
                        bus_print_property_value(name, expected_value, value, "[no data]");
                else
                        bus_print_property_valuef(name, expected_value, value, "%"PRIu64, u);

                return 1;
        }

        case SD_BUS_TYPE_INT64: {
                int64_t i;

                r = sd_bus_message_read_basic(m, type, &i);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%"PRIi64, i);
                return 1;
        }

        case SD_BUS_TYPE_UINT32: {
                uint32_t u;

                r = sd_bus_message_read_basic(m, type, &u);
                if (r < 0)
                        return r;

                if (strstr(name, "UMask") || strstr(name, "Mode"))
                        bus_print_property_valuef(name, expected_value, value, "%04o", u);

                else if (streq(name, "UID")) {
                        if (u == UID_INVALID)
                                bus_print_property_value(name, expected_value, value, "[not set]");
                        else
                                bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);
                } else if (streq(name, "GID")) {
                        if (u == GID_INVALID)
                                bus_print_property_value(name, expected_value, value, "[not set]");
                        else
                                bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);
                } else
                        bus_print_property_valuef(name, expected_value, value, "%"PRIu32, u);

                return 1;
        }

        case SD_BUS_TYPE_INT32: {
                int32_t i;

                r = sd_bus_message_read_basic(m, type, &i);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%"PRIi32, i);
                return 1;
        }

        case SD_BUS_TYPE_DOUBLE: {
                double d;

                r = sd_bus_message_read_basic(m, type, &d);
                if (r < 0)
                        return r;

                bus_print_property_valuef(name, expected_value, value, "%g", d);
                return 1;
        }

        case SD_BUS_TYPE_ARRAY:
                if (streq(contents, "s")) {
                        bool first = true;
                        const char *str;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, contents);
                        if (r < 0)
                                return r;

                        while ((r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &str)) > 0) {
                                bool good;

                                if (first && !value)
                                        printf("%s=", name);

                                /* This property has multiple space-separated values, so
                                 * neither spaces nor newlines can be allowed in a value. */
                                good = str[strcspn(str, " \n")] == '\0';

                                printf("%s%s", first ? "" : " ", good ? str : "[unprintable]");

                                first = false;
                        }
                        if (r < 0)
                                return r;

                        if (first && all && !value)
                                printf("%s=", name);
                        if (!first || all)
                                puts("");

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;

                        return 1;

                } else if (streq(contents, "y")) {
                        const uint8_t *u;
                        size_t n;

                        r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, (const void**) &u, &n);
                        if (r < 0)
                                return r;

                        if (all || n > 0) {
                                unsigned i;

                                if (!value)
                                        printf("%s=", name);

                                for (i = 0; i < n; i++)
                                        printf("%02x", u[i]);

                                puts("");
                        }

                        return 1;

                } else if (streq(contents, "u")) {
                        uint32_t *u;
                        size_t n;

                        r = sd_bus_message_read_array(m, SD_BUS_TYPE_UINT32, (const void**) &u, &n);
                        if (r < 0)
                                return r;

                        if (all || n > 0) {
                                unsigned i;

                                if (!value)
                                        printf("%s=", name);

                                for (i = 0; i < n; i++)
                                        printf("%08x", u[i]);

                                puts("");
                        }

                        return 1;
                }

                break;
        }

        return 0;
}

int bus_message_print_all_properties(
                sd_bus_message *m,
                bus_message_print_t func,
                char **filter,
                bool value,
                bool all,
                Set **found_properties) {

        int r;

        assert(m);

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                _cleanup_free_ char *name_with_equal = NULL;
                const char *name, *contents, *expected_value = NULL;

                r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &name);
                if (r < 0)
                        return r;

                if (found_properties) {
                        r = set_ensure_allocated(found_properties, &string_hash_ops);
                        if (r < 0)
                                return log_oom();

                        r = set_put(*found_properties, name);
                        if (r < 0 && r != -EEXIST)
                                return log_oom();
                }

                name_with_equal = strjoin(name, "=");
                if (!name_with_equal)
                        return log_oom();

                if (!filter || strv_find(filter, name) ||
                    (expected_value = strv_find_startswith(filter, name_with_equal))) {
                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        if (func)
                                r = func(name, expected_value, m, value, all);
                        if (!func || r == 0)
                                r = bus_print_property(name, expected_value, m, value, all);
                        if (r < 0)
                                return r;
                        if (r == 0) {
                                if (all && !expected_value)
                                        printf("%s=[unprintable]\n", name);
                                /* skip what we didn't read */
                                r = sd_bus_message_skip(m, contents);
                                if (r < 0)
                                        return r;
                        }

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;
                } else {
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0)
                                return r;
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }
        if (r < 0)
                return r;

        r = sd_bus_message_exit_container(m);
        if (r < 0)
                return r;

        return 0;
}

int bus_print_all_properties(
                sd_bus *bus,
                const char *dest,
                const char *path,
                bus_message_print_t func,
                char **filter,
                bool value,
                bool all,
                Set **found_properties) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        _cleanup_(sd_bus_error_free) sd_bus_error error = SD_BUS_ERROR_NULL;
        int r;

        assert(bus);
        assert(path);

        r = sd_bus_call_method(bus,
                        dest,
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        &error,
                        &reply,
                        "s", "");
        if (r < 0)
                return r;

        return bus_message_print_all_properties(reply, func, filter, value, all, found_properties);
}

int bus_map_id128(sd_bus *bus, const char *member, sd_bus_message *m, sd_bus_error *error, void *userdata) {
        sd_id128_t *p = userdata;
        const void *v;
        size_t n;
        int r;

        r = sd_bus_message_read_array(m, SD_BUS_TYPE_BYTE, &v, &n);
        if (r < 0)
                return r;

        if (n == 0)
                *p = SD_ID128_NULL;
        else if (n == 16)
                memcpy((*p).bytes, v, n);
        else
                return -EINVAL;

        return 0;
}

static int map_basic(sd_bus *bus, const char *member, sd_bus_message *m, unsigned flags, sd_bus_error *error, void *userdata) {
        char type;
        int r;

        r = sd_bus_message_peek_type(m, &type, NULL);
        if (r < 0)
                return r;

        switch (type) {

        case SD_BUS_TYPE_STRING:
        case SD_BUS_TYPE_OBJECT_PATH: {
                const char **p = userdata;
                const char *s;

                r = sd_bus_message_read_basic(m, type, &s);
                if (r < 0)
                        return r;

                if (isempty(s))
                        s = NULL;

                if (flags & BUS_MAP_STRDUP)
                        return free_and_strdup((char **) userdata, s);

                *p = s;
                return 0;
        }

        case SD_BUS_TYPE_ARRAY: {
                _cleanup_strv_free_ char **l = NULL;
                char ***p = userdata;

                r = bus_message_read_strv_extend(m, &l);
                if (r < 0)
                        return r;

                return strv_extend_strv(p, l, false);
        }

        case SD_BUS_TYPE_BOOLEAN: {
                int b;

                r = sd_bus_message_read_basic(m, type, &b);
                if (r < 0)
                        return r;

                if (flags & BUS_MAP_BOOLEAN_AS_BOOL)
                        *(bool*) userdata = b;
                else
                        *(int*) userdata = b;

                return 0;
        }

        case SD_BUS_TYPE_INT32:
        case SD_BUS_TYPE_UINT32: {
                uint32_t u, *p = userdata;

                r = sd_bus_message_read_basic(m, type, &u);
                if (r < 0)
                        return r;

                *p = u;
                return 0;
        }

        case SD_BUS_TYPE_INT64:
        case SD_BUS_TYPE_UINT64: {
                uint64_t t, *p = userdata;

                r = sd_bus_message_read_basic(m, type, &t);
                if (r < 0)
                        return r;

                *p = t;
                return 0;
        }

        case SD_BUS_TYPE_DOUBLE: {
                double d, *p = userdata;

                r = sd_bus_message_read_basic(m, type, &d);
                if (r < 0)
                        return r;

                *p = d;
                return 0;
        }}

        return -EOPNOTSUPP;
}

int bus_message_map_all_properties(
                sd_bus_message *m,
                const struct bus_properties_map *map,
                unsigned flags,
                sd_bus_error *error,
                void *userdata) {

        int r;

        assert(m);
        assert(map);

        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_ARRAY, "{sv}");
        if (r < 0)
                return r;

        while ((r = sd_bus_message_enter_container(m, SD_BUS_TYPE_DICT_ENTRY, "sv")) > 0) {
                const struct bus_properties_map *prop;
                const char *member;
                const char *contents;
                void *v;
                unsigned i;

                r = sd_bus_message_read_basic(m, SD_BUS_TYPE_STRING, &member);
                if (r < 0)
                        return r;

                for (i = 0, prop = NULL; map[i].member; i++)
                        if (streq(map[i].member, member)) {
                                prop = &map[i];
                                break;
                        }

                if (prop) {
                        r = sd_bus_message_peek_type(m, NULL, &contents);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_enter_container(m, SD_BUS_TYPE_VARIANT, contents);
                        if (r < 0)
                                return r;

                        v = (uint8_t *)userdata + prop->offset;
                        if (map[i].set)
                                r = prop->set(sd_bus_message_get_bus(m), member, m, error, v);
                        else
                                r = map_basic(sd_bus_message_get_bus(m), member, m, flags, error, v);
                        if (r < 0)
                                return r;

                        r = sd_bus_message_exit_container(m);
                        if (r < 0)
                                return r;
                } else {
                        r = sd_bus_message_skip(m, "v");
                        if (r < 0)
                                return r;
                }

                r = sd_bus_message_exit_container(m);
                if (r < 0)
                        return r;
        }
        if (r < 0)
                return r;

        return sd_bus_message_exit_container(m);
}

int bus_map_all_properties(
                sd_bus *bus,
                const char *destination,
                const char *path,
                const struct bus_properties_map *map,
                unsigned flags,
                sd_bus_error *error,
                sd_bus_message **reply,
                void *userdata) {

        _cleanup_(sd_bus_message_unrefp) sd_bus_message *m = NULL;
        int r;

        assert(bus);
        assert(destination);
        assert(path);
        assert(map);
        assert(reply || (flags & BUS_MAP_STRDUP));

        r = sd_bus_call_method(
                        bus,
                        destination,
                        path,
                        "org.freedesktop.DBus.Properties",
                        "GetAll",
                        error,
                        &m,
                        "s", "");
        if (r < 0)
                return r;

        r = bus_message_map_all_properties(m, map, flags, error, userdata);
        if (r < 0)
                return r;

        if (reply)
                *reply = sd_bus_message_ref(m);

        return r;
}

int bus_connect_transport(BusTransport transport, const char *host, bool user, sd_bus **ret) {
        _cleanup_(sd_bus_close_unrefp) sd_bus *bus = NULL;
        int r;

        assert(transport >= 0);
        assert(transport < _BUS_TRANSPORT_MAX);
        assert(ret);

        assert_return((transport == BUS_TRANSPORT_LOCAL) == !host, -EINVAL);
        assert_return(transport == BUS_TRANSPORT_LOCAL || !user, -EOPNOTSUPP);

        switch (transport) {

        case BUS_TRANSPORT_LOCAL:
                if (user)
                        r = sd_bus_default_user(&bus);
                else {
                        if (sd_booted() <= 0) {
                                /* Print a friendly message when the local system is actually not running systemd as PID 1. */
                                log_error("System has not been booted with systemd as init system (PID 1). Can't operate.");

                                return -EHOSTDOWN;
                        }
                        r = sd_bus_default_system(&bus);
                }
                break;

        case BUS_TRANSPORT_REMOTE:
                r = sd_bus_open_system_remote(&bus, host);
                break;

        case BUS_TRANSPORT_MACHINE:
                r = sd_bus_open_system_machine(&bus, host);
                break;

        default:
                assert_not_reached("Hmm, unknown transport type.");
        }
        if (r < 0)
                return r;

        r = sd_bus_set_exit_on_disconnect(bus, true);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(bus);

        return 0;
}

int bus_connect_transport_systemd(BusTransport transport, const char *host, bool user, sd_bus **bus) {
        int r;

        assert(transport >= 0);
        assert(transport < _BUS_TRANSPORT_MAX);
        assert(bus);

        assert_return((transport == BUS_TRANSPORT_LOCAL) == !host, -EINVAL);
        assert_return(transport == BUS_TRANSPORT_LOCAL || !user, -EOPNOTSUPP);

        switch (transport) {

        case BUS_TRANSPORT_LOCAL:
                if (user)
                        r = bus_connect_user_systemd(bus);
                else {
                        if (sd_booted() <= 0)
                                /* Print a friendly message when the local system is actually not running systemd as PID 1. */
                                return log_error_errno(SYNTHETIC_ERRNO(EHOSTDOWN),
                                                       "System has not been booted with systemd as init system (PID 1). Can't operate.");
                        r = bus_connect_system_systemd(bus);
                }
                break;

        case BUS_TRANSPORT_REMOTE:
                r = sd_bus_open_system_remote(bus, host);
                break;

        case BUS_TRANSPORT_MACHINE:
                r = sd_bus_open_system_machine(bus, host);
                break;

        default:
                assert_not_reached("Hmm, unknown transport type.");
        }

        return r;
}

int bus_property_get_bool(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        int b = *(bool*) userdata;

        return sd_bus_message_append_basic(reply, 'b', &b);
}

int bus_property_set_bool(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *value,
                void *userdata,
                sd_bus_error *error) {

        int b, r;

        r = sd_bus_message_read(value, "b", &b);
        if (r < 0)
                return r;

        *(bool*) userdata = b;
        return 0;
}

int bus_property_get_id128(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        sd_id128_t *id = userdata;

        if (sd_id128_is_null(*id)) /* Add an empty array if the ID is zero */
                return sd_bus_message_append(reply, "ay", 0);
        else
                return sd_bus_message_append_array(reply, 'y', id->bytes, 16);
}

#if __SIZEOF_SIZE_T__ != 8
int bus_property_get_size(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        uint64_t sz = *(size_t*) userdata;

        return sd_bus_message_append_basic(reply, 't', &sz);
}
#endif

#if __SIZEOF_LONG__ != 8
int bus_property_get_long(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        int64_t l = *(long*) userdata;

        return sd_bus_message_append_basic(reply, 'x', &l);
}

int bus_property_get_ulong(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        uint64_t ul = *(unsigned long*) userdata;

        return sd_bus_message_append_basic(reply, 't', &ul);
}
#endif

/**
 * bus_path_encode_unique() - encode unique object path
 * @b: bus connection or NULL
 * @prefix: object path prefix
 * @sender_id: unique-name of client, or NULL
 * @external_id: external ID to be chosen by client, or NULL
 * @ret_path: storage for encoded object path pointer
 *
 * Whenever we provide a bus API that allows clients to create and manage
 * server-side objects, we need to provide a unique name for these objects. If
 * we let the server choose the name, we suffer from a race condition: If a
 * client creates an object asynchronously, it cannot destroy that object until
 * it received the method reply. It cannot know the name of the new object,
 * thus, it cannot destroy it. Furthermore, it enforces a round-trip.
 *
 * Therefore, many APIs allow the client to choose the unique name for newly
 * created objects. There're two problems to solve, though:
 *    1) Object names are usually defined via dbus object paths, which are
 *       usually globally namespaced. Therefore, multiple clients must be able
 *       to choose unique object names without interference.
 *    2) If multiple libraries share the same bus connection, they must be
 *       able to choose unique object names without interference.
 * The first problem is solved easily by prefixing a name with the
 * unique-bus-name of a connection. The server side must enforce this and
 * reject any other name. The second problem is solved by providing unique
 * suffixes from within sd-bus.
 *
 * This helper allows clients to create unique object-paths. It uses the
 * template '/prefix/sender_id/external_id' and returns the new path in
 * @ret_path (must be freed by the caller).
 * If @sender_id is NULL, the unique-name of @b is used. If @external_id is
 * NULL, this function allocates a unique suffix via @b (by requesting a new
 * cookie). If both @sender_id and @external_id are given, @b can be passed as
 * NULL.
 *
 * Returns: 0 on success, negative error code on failure.
 */
int bus_path_encode_unique(sd_bus *b, const char *prefix, const char *sender_id, const char *external_id, char **ret_path) {
        _cleanup_free_ char *sender_label = NULL, *external_label = NULL;
        char external_buf[DECIMAL_STR_MAX(uint64_t)], *p;
        int r;

        assert_return(b || (sender_id && external_id), -EINVAL);
        assert_return(object_path_is_valid(prefix), -EINVAL);
        assert_return(ret_path, -EINVAL);

        if (!sender_id) {
                r = sd_bus_get_unique_name(b, &sender_id);
                if (r < 0)
                        return r;
        }

        if (!external_id) {
                xsprintf(external_buf, "%"PRIu64, ++b->cookie);
                external_id = external_buf;
        }

        sender_label = bus_label_escape(sender_id);
        if (!sender_label)
                return -ENOMEM;

        external_label = bus_label_escape(external_id);
        if (!external_label)
                return -ENOMEM;

        p = path_join(prefix, sender_label, external_label);
        if (!p)
                return -ENOMEM;

        *ret_path = p;
        return 0;
}

/**
 * bus_path_decode_unique() - decode unique object path
 * @path: object path to decode
 * @prefix: object path prefix
 * @ret_sender: output parameter for sender-id label
 * @ret_external: output parameter for external-id label
 *
 * This does the reverse of bus_path_encode_unique() (see its description for
 * details). Both trailing labels, sender-id and external-id, are unescaped and
 * returned in the given output parameters (the caller must free them).
 *
 * Note that this function returns 0 if the path does not match the template
 * (see bus_path_encode_unique()), 1 if it matched.
 *
 * Returns: Negative error code on failure, 0 if the given object path does not
 *          match the template (return parameters are set to NULL), 1 if it was
 *          parsed successfully (return parameters contain allocated labels).
 */
int bus_path_decode_unique(const char *path, const char *prefix, char **ret_sender, char **ret_external) {
        const char *p, *q;
        char *sender, *external;

        assert(object_path_is_valid(path));
        assert(object_path_is_valid(prefix));
        assert(ret_sender);
        assert(ret_external);

        p = object_path_startswith(path, prefix);
        if (!p) {
                *ret_sender = NULL;
                *ret_external = NULL;
                return 0;
        }

        q = strchr(p, '/');
        if (!q) {
                *ret_sender = NULL;
                *ret_external = NULL;
                return 0;
        }

        sender = bus_label_unescape_n(p, q - p);
        external = bus_label_unescape(q + 1);
        if (!sender || !external) {
                free(sender);
                free(external);
                return -ENOMEM;
        }

        *ret_sender = sender;
        *ret_external = external;
        return 1;
}

int bus_property_get_rlimit(
                sd_bus *bus,
                const char *path,
                const char *interface,
                const char *property,
                sd_bus_message *reply,
                void *userdata,
                sd_bus_error *error) {

        const char *is_soft;
        struct rlimit *rl;
        uint64_t u;
        rlim_t x;

        assert(bus);
        assert(reply);
        assert(userdata);

        is_soft = endswith(property, "Soft");

        rl = *(struct rlimit**) userdata;
        if (rl)
                x = is_soft ? rl->rlim_cur : rl->rlim_max;
        else {
                struct rlimit buf = {};
                const char *s, *p;
                int z;

                /* Chop off "Soft" suffix */
                s = is_soft ? strndupa(property, is_soft - property) : property;

                /* Skip over any prefix, such as "Default" */
                assert_se(p = strstr(s, "Limit"));

                z = rlimit_from_string(p + 5);
                assert(z >= 0);

                (void) getrlimit(z, &buf);
                x = is_soft ? buf.rlim_cur : buf.rlim_max;
        }

        /* rlim_t might have different sizes, let's map RLIMIT_INFINITY to (uint64_t) -1, so that it is the same on all
         * archs */
        u = x == RLIM_INFINITY ? (uint64_t) -1 : (uint64_t) x;

        return sd_bus_message_append(reply, "t", u);
}

int bus_track_add_name_many(sd_bus_track *t, char **l) {
        int r = 0;
        char **i;

        assert(t);

        /* Continues adding after failure, and returns the first failure. */

        STRV_FOREACH(i, l) {
                int k;

                k = sd_bus_track_add_name(t, *i);
                if (k < 0 && r >= 0)
                        r = k;
        }

        return r;
}

int bus_open_system_watch_bind_with_description(sd_bus **ret, const char *description) {
        _cleanup_(sd_bus_close_unrefp) sd_bus *bus = NULL;
        const char *e;
        int r;

        assert(ret);

        /* Match like sd_bus_open_system(), but with the "watch_bind" feature and the Connected() signal
         * turned on. */

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        if (description) {
                r = sd_bus_set_description(bus, description);
                if (r < 0)
                        return r;
        }

        e = secure_getenv("DBUS_SYSTEM_BUS_ADDRESS");
        if (!e)
                e = DEFAULT_SYSTEM_BUS_ADDRESS;

        r = sd_bus_set_address(bus, e);
        if (r < 0)
                return r;

        r = sd_bus_set_bus_client(bus, true);
        if (r < 0)
                return r;

        r = sd_bus_negotiate_creds(bus, true, SD_BUS_CREDS_UID|SD_BUS_CREDS_EUID|SD_BUS_CREDS_EFFECTIVE_CAPS);
        if (r < 0)
                return r;

        r = sd_bus_set_watch_bind(bus, true);
        if (r < 0)
                return r;

        r = sd_bus_set_connected_signal(bus, true);
        if (r < 0)
                return r;

        r = sd_bus_start(bus);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(bus);

        return 0;
}

int bus_reply_pair_array(sd_bus_message *m, char **l) {
        _cleanup_(sd_bus_message_unrefp) sd_bus_message *reply = NULL;
        char **k, **v;
        int r;

        assert(m);

        /* Reply to the specified message with a message containing a dictionary put together from the
         * specified strv */

        r = sd_bus_message_new_method_return(m, &reply);
        if (r < 0)
                return r;

        r = sd_bus_message_open_container(reply, 'a', "{ss}");
        if (r < 0)
                return r;

        STRV_FOREACH_PAIR(k, v, l) {
                r = sd_bus_message_append(reply, "{ss}", *k, *v);
                if (r < 0)
                        return r;
        }

        r = sd_bus_message_close_container(reply);
        if (r < 0)
                return r;

        return sd_bus_send(NULL, reply, NULL);
}

static void bus_message_unref_wrapper(void *m) {
        sd_bus_message_unref(m);
}

const struct hash_ops bus_message_hash_ops = {
        .hash = trivial_hash_func,
        .compare = trivial_compare_func,
        .free_value = bus_message_unref_wrapper,
};
