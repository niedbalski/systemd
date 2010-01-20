/*-*- Mode: C; c-basic-offset: 8 -*-*/

#include <assert.h>
#include <errno.h>
#include <string.h>

#include "set.h"
#include "name.h"
#include "macro.h"
#include "strv.h"

NameType name_type_from_string(const char *n) {
        NameType t;
        static const char* suffixes[_NAME_TYPE_MAX] = {
                [NAME_SERVICE] = ".service",
                [NAME_TIMER] = ".timer",
                [NAME_SOCKET] = ".socket",
                [NAME_MILESTONE] = ".milestone",
                [NAME_DEVICE] = ".device",
                [NAME_MOUNT] = ".mount",
                [NAME_AUTOMOUNT] = ".automount",
                [NAME_SNAPSHOT] = ".snapshot",
        };

        assert(n);

        for (t = 0; t < _NAME_TYPE_MAX; t++)
                if (endswith(n, suffixes[t]))
                        return t;

        return _NAME_TYPE_INVALID;
}

#define VALID_CHARS                             \
        "0123456789"                            \
        "abcdefghijklmnopqrstuvwxyz"            \
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"            \
        "-_"

bool name_is_valid(const char *n) {
        NameType t;
        const char *e, *i;

        assert(n);

        t = name_type_from_string(n);
        if (t < 0 || t >= _NAME_TYPE_MAX)
                return false;

        if (!(e = strrchr(n, '.')))
                return false;

        for (i = n; i < e; i++)
                if (!strchr(VALID_CHARS, *i))
                        return false;

        return true;
}

Name *name_new(Manager *m) {
        Name *n;

        assert(m);

        if (!(n = new0(Name, 1)))
                return NULL;

        if (!(n->meta.names = set_new(string_hash_func, string_compare_func))) {
                free(n);
                return NULL;
        }

        /* Not much initialization happening here at this time */
        n->meta.manager = m;
        n->meta.type = _NAME_TYPE_INVALID;
        n->meta.state = NAME_STUB;

        /* We don't link the name here, that is left for name_link() */

        return n;
}

/* FIXME: Does not rollback on failure! */
int name_link_names(Name *n, bool replace) {
        char *t;
        void *state;
        int r;

        assert(n);

        if (!n->meta.linked)
                return 0;

        /* Link all names that aren't linked yet. */

        SET_FOREACH(t, n->meta.names, state)
                if (replace) {
                        if ((r = hashmap_replace(n->meta.manager->names, t, n)) < 0)
                                return r;
                } else {
                        if ((r = hashmap_put(n->meta.manager->names, t, n)) < 0)
                                return r;
                }

        return 0;
}

int name_link(Name *n) {
        int r;

        assert(n);
        assert(!set_isempty(n->meta.names));
        assert(!n->meta.linked);

        n->meta.linked = true;

        if ((r = name_link_names(n, false) < 0)) {
                char *t;
                void *state;

                /* Rollback the registered names */
                SET_FOREACH(t, n->meta.names, state)
                        hashmap_remove_value(n->meta.manager->names, t, n);

                n->meta.linked = false;
                return r;
        }

        if (n->meta.state == NAME_STUB)
                LIST_PREPEND(Meta, n->meta.manager->load_queue, &n->meta);

        return 0;
}

static void bidi_set_free(Name *name, Set *s) {
        void *state;
        Name *other;

        assert(name);

        /* Frees the set and makes sure we are dropped from the
         * inverse pointers */

        SET_FOREACH(other, s, state) {
                NameDependency d;

                for (d = 0; d < _NAME_DEPENDENCY_MAX; d++)
                        set_remove(other->meta.dependencies[d], name);
        }

        set_free(s);
}

void name_free(Name *name) {
        NameDependency d;
        char *t;

        assert(name);

        /* Detach from next 'bigger' objects */
        if (name->meta.linked) {
                char *t;
                void *state;

                SET_FOREACH(t, name->meta.names, state)
                        hashmap_remove_value(name->meta.manager->names, t, name);

                if (name->meta.state == NAME_STUB)
                        LIST_REMOVE(Meta, name->meta.manager->load_queue, &name->meta);
        }

        /* Free data and next 'smaller' objects */
        if (name->meta.job)
                job_free(name->meta.job);

        for (d = 0; d < _NAME_DEPENDENCY_MAX; d++)
                bidi_set_free(name, name->meta.dependencies[d]);

        switch (name->meta.type) {

                case NAME_SOCKET: {
                        unsigned i;
                        Socket *s = SOCKET(name);

                        for (i = 0; i < s->n_fds; i++)
                                close_nointr(s->fds[i]);
                        break;
                }

                case NAME_DEVICE: {
                        Device *d = DEVICE(name);

                        free(d->sysfs);
                        break;
                }

                case NAME_MOUNT: {
                        Mount *m = MOUNT(name);

                        free(m->path);
                        break;
                }

                case NAME_AUTOMOUNT: {
                        Automount *a = AUTOMOUNT(name);

                        free(a->path);
                        break;
                }

                default:
                        ;
        }

        free(name->meta.description);

        while ((t = set_steal_first(name->meta.names)))
                free(t);
        set_free(name->meta.names);

        free(name);
}

bool name_is_ready(Name *name) {

        assert(name);

        if (name->meta.state != NAME_LOADED)
                return false;

        assert(name->meta.type < _NAME_TYPE_MAX);

        switch (name->meta.type) {
                case NAME_SERVICE: {
                        Service *s = SERVICE(name);

                        return
                                s->state == SERVICE_RUNNING ||
                                s->state == SERVICE_RELOAD_PRE ||
                                s->state == SERVICE_RELOAD ||
                                s->state == SERVICE_RELOAD_POST;
                }

                case NAME_TIMER: {
                        Timer *t = TIMER(name);

                        return
                                t->state == TIMER_WAITING ||
                                t->state == TIMER_RUNNING;
                }

                case NAME_SOCKET: {
                        Socket *s = SOCKET(name);

                        return
                                s->state == SOCKET_LISTENING ||
                                s->state == SOCKET_RUNNING;
                }

                case NAME_MILESTONE:
                        return MILESTONE(name)->state == MILESTONE_ACTIVE;

                case NAME_DEVICE:
                        return DEVICE(name)->state == DEVICE_AVAILABLE;

                case NAME_MOUNT:
                        return MOUNT(name)->state == MOUNT_MOUNTED;

                case NAME_AUTOMOUNT: {
                        Automount *a = AUTOMOUNT(name);

                        return
                                a->state == AUTOMOUNT_WAITING ||
                                a->state == AUTOMOUNT_RUNNING;
                }

                case NAME_SNAPSHOT:
                        return SNAPSHOT(name)->state == SNAPSHOT_ACTIVE;


                case _NAME_TYPE_MAX:
                case _NAME_TYPE_INVALID:
                        ;
        }

        assert_not_reached("Unknown name type.");
        return false;
}

static int ensure_in_set(Set **s, void *data) {
        int r;

        assert(s);
        assert(data);

        if (!*s)
                if (!(*s = set_new(trivial_hash_func, trivial_compare_func)))
                        return -ENOMEM;

        if ((r = set_put(*s, data) < 0))
                if (r != -EEXIST)
                        return r;

        return 0;
}

/* FIXME: Does not rollback on failure! */
int name_augment(Name *n) {
        int r;
        void* state;
        Name *other;

        assert(n);

        /* Adds in the missing links to make all dependencies
         * bidirectional. */

        SET_FOREACH(other, n->meta.dependencies[NAME_BEFORE], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_AFTER], n) < 0))
                        return r;
        SET_FOREACH(other, n->meta.dependencies[NAME_AFTER], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_BEFORE], n) < 0))
                        return r;

        SET_FOREACH(other, n->meta.dependencies[NAME_CONFLICTS], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_CONFLICTS], n) < 0))
                        return r;

        SET_FOREACH(other, n->meta.dependencies[NAME_REQUIRES], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_REQUIRED_BY], n) < 0))
                        return r;
        SET_FOREACH(other, n->meta.dependencies[NAME_REQUISITE], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_REQUIRED_BY], n) < 0))
                        return r;

        SET_FOREACH(other, n->meta.dependencies[NAME_WANTS], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_WANTED_BY], n) < 0))
                        return r;
        SET_FOREACH(other, n->meta.dependencies[NAME_SOFT_REQUIRES], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_WANTED_BY], n) < 0))
                        return r;
        SET_FOREACH(other, n->meta.dependencies[NAME_SOFT_REQUISITE], state)
                if ((r = ensure_in_set(&other->meta.dependencies[NAME_WANTED_BY], n) < 0))
                        return r;

        return r;
}

static int ensure_merge(Set **s, Set *other) {

        if (!other)
                return 0;

        if (*s)
                return set_merge(*s, other);

        if (!(*s = set_copy(other)))
                return -ENOMEM;

        return 0;
}

/* FIXME: Does not rollback on failure! */
int name_merge(Name *name, Name *other) {
        int r;
        NameDependency d;

        assert(name);
        assert(other);
        assert(name->meta.manager == other->meta.manager);

        /* This merges 'other' into 'name'. FIXME: This does not
         * rollback on failure. */

        if (name->meta.type != other->meta.type)
                return -EINVAL;

        if (other->meta.state != NAME_STUB)
                return -EINVAL;

        /* Merge names */
        if ((r = ensure_merge(&name->meta.names, other->meta.names)) < 0)
                return r;

        /* Merge dependencies */
        for (d = 0; d < _NAME_DEPENDENCY_MAX; d++)
                if ((r = ensure_merge(&name->meta.dependencies[d], other->meta.dependencies[d])) < 0)
                        return r;

        /* Hookup new deps and names */
        if (name->meta.linked) {
                if ((r = name_augment(name)) < 0)
                        return r;

                if ((r = name_link_names(name, true)) < 0)
                        return r;
        }

        return 0;
}

const char* name_id(Name *n) {
        assert(n);

        return set_first(n->meta.names);
}

void name_dump(Name *n, FILE *f) {

        static const char* const state_table[_NAME_STATE_MAX] = {
                [NAME_STUB] = "stub",
                [NAME_LOADED] = "loaded",
                [NAME_FAILED] = "failed"
        };

        static const char* const socket_state_table[_SOCKET_STATE_MAX] = {
                [SOCKET_DEAD] = "dead",
                [SOCKET_BEFORE] = "before",
                [SOCKET_START_PRE] = "start-pre",
                [SOCKET_START] = "start",
                [SOCKET_START_POST] = "start-post",
                [SOCKET_LISTENING] = "listening",
                [SOCKET_RUNNING] = "running",
                [SOCKET_STOP_PRE] = "stop-pre",
                [SOCKET_STOP] = "stop",
                [SOCKET_STOP_POST] = "stop-post",
                [SOCKET_MAINTAINANCE] = "maintainance"
        };

        void *state;
        char *t;

        assert(n);

        fprintf(f,
                "Name %s\n"
                "\tDescription: %s\n"
                "\tName State: %s\n",
                name_id(n),
                n->meta.description ? n->meta.description : name_id(n),
                state_table[n->meta.state]);

        fprintf(f, "\tNames: ");
        SET_FOREACH(t, n->meta.names, state)
                fprintf(f, "%s ", t);
        fprintf(f, "\n");

        switch (n->meta.type) {
                case NAME_SOCKET: {
                        int r;
                        char *s = NULL;
                        const char *t;

                        if ((r = address_print(&n->socket.address, &s)) < 0)
                                t = strerror(-r);
                        else
                                t = s;

                        fprintf(f,
                                "\tAddress: %s\n"
                                "\tSocket State: %s\n",
                                t,
                                socket_state_table[n->socket.state]);

                        free(s);
                        break;
                }

                default:
                        ;
        }

        if (n->meta.job) {
                fprintf(f, "\t");
                job_dump(n->meta.job, f);
        }
}
