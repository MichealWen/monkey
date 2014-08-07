/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Monkey HTTP Server
 *  ==================
 *  Copyright 2001-2014 Monkey Software LLC <eduardo@monkey.io>
 *  Copyright (C) 2012, Lauri Kasanen <cand@gmx.com>
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

/* Only built for the library */
#ifdef SHAREDLIB

#include <stdio.h>
#include <stdarg.h>
#include <limits.h>
#include <dlfcn.h>

#include <monkey/mk_lib.h>
#include <monkey/mk_utils.h>
#include <monkey/mk_memory.h>
#include <monkey/mk_config.h>
#include <monkey/mk_info.h>
#include <monkey/mk_string.h>
#include <monkey/mk_plugin.h>
#include <monkey/mk_clock.h>
#include <monkey/mk_mimetype.h>
#include <monkey/mk_server.h>
#include <monkey/mk_vhost.h>
#include <monkey/mk_stats.h>

static struct host *mklib_host_find(const char *name)
{
    struct host *entry_host;
    const struct mk_list *head_vhost;

    mk_list_foreach(head_vhost, &config->hosts) {
        entry_host = mk_list_entry(head_vhost, struct host, _head);

        if (strcmp(name, entry_host->file) == 0) return entry_host;
    }

    return NULL;
}


static void mklib_run(void *p)
{
    int remote_fd, ret;
    const mklib_ctx ctx = p;

    mk_utils_worker_rename("libmonkey");
    mk_socket_set_tcp_defer_accept(config->server_fd);

    while (1) {

        if (!ctx->lib_running) {
            sleep(1);
            continue;
        }

        remote_fd = mk_socket_accept(config->server_fd);
        if (remote_fd == -1) continue;

        ret = mk_sched_add_client(remote_fd);
        if (ret == -1) mk_socket_close(remote_fd);
    }
}

static int load_networking(const char *path)
{
    void *handle;
    struct plugin *p;
    int ret;

    handle = mk_plugin_load(path);
    if (!handle) return MKLIB_FALSE;

    p = mk_plugin_alloc(handle, path);
    if (!p) {
        dlclose(handle);
        return MKLIB_FALSE;
    }

    ret = p->init(&api, "");
    if (ret < 0) {
        mk_plugin_free(p);
        dlclose(handle);
        return MKLIB_FALSE;
    }

    mk_plugin_register(p);
    return MKLIB_TRUE;
}

int mklib_callback_set(mklib_ctx ctx, const enum mklib_cb cb, void *func)
{
    /* Function is allowed to be NULL, to reset it) */
    if (!ctx || !cb || ctx->lib_running) return MKLIB_FALSE;

    switch(cb) {
        case MKCB_IPCHECK:
            ctx->ipf = func;
        break;
        case MKCB_URLCHECK:
            ctx->urlf = func;
        break;
        case MKCB_DATA:
            ctx->dataf = func;
        break;
        case MKCB_CLOSE:
            ctx->closef = func;
        break;
        default:
            return MKLIB_FALSE;
        break;
    }

    return MKLIB_TRUE;
}

/* Returns NULL on error. All pointer arguments may be NULL and the port/plugins
 * may be 0 for the defaults in each case.
 *
 * With no address, bind to all.
 * With no port, use 2001.
 * With no plugins, default to MKLIB_LIANA only.
 * With no documentroot, the default vhost won't access files.
 */
mklib_ctx mklib_init(const char *address, const unsigned int port,
                     const unsigned int plugins, const char *documentroot)
{
#ifdef PYTHON_BINDINGS
    PyEval_InitThreads();
#endif
    mklib_ctx a = mk_mem_malloc_z(sizeof(struct mklib_ctx_t));
    if (!a) return NULL;

    config = mk_mem_malloc_z(sizeof(struct server_config));
    if (!config) goto out;

    config->serverconf = mk_string_dup(MONKEY_PATH_CONF);
    mk_config_set_init_values();

    mk_kernel_init();
    mk_kernel_features();

    /*
     * If the worker numbers have not be set, set the number based on
     * the number of CPU cores
     */
    if (config->workers < 1) {
        config->workers = sysconf(_SC_NPROCESSORS_ONLN);
    }

    mk_sched_init();
    mk_plugin_init();

    a->plugdir = PLUGDIR;

    char tmppath[PATH_MAX];

    if (plugins & MKLIB_LIANA_SSL) {
        config->transport_layer = mk_string_dup("liana_ssl");
        snprintf(tmppath, PATH_MAX, "%s/monkey-liana_ssl.so", a->plugdir);
        if (!load_networking(tmppath)) goto out_config;
    }
    else {
        config->transport_layer = mk_string_dup("liana");
        snprintf(tmppath, PATH_MAX, "%s/monkey-liana.so", a->plugdir);
        if (!load_networking(tmppath)) goto out_config;
    }

    if (!plg_netiomap) goto out_config;
    mk_plugin_preworker_calls();

    if (port) config->serverport = port;
    if (address) config->listen_addr = mk_string_dup(address);
    else config->listen_addr = mk_string_dup(config->listen_addr);

    unsigned long len;
    struct host *host = mk_mem_malloc_z(sizeof(struct host));
    /* We hijack this field for the vhost name */
    host->file = mk_string_dup("default");
    mk_list_init(&host->error_pages);
    mk_list_init(&host->server_names);
    mk_string_build(&host->host_signature, &len, "libmonkey");
    mk_string_build(&host->header_host_signature.data,
                    &host->header_host_signature.len,
                    "Server: %s", host->host_signature);

    struct host_alias *alias = mk_mem_malloc_z(sizeof(struct host_alias));
    alias->name = mk_string_dup(config->listen_addr);
    alias->len = strlen(config->listen_addr);
    mk_list_add(&alias->_head, &host->server_names);

    mk_list_add(&host->_head, &config->hosts);
    config->nhosts++;

    if (documentroot) {
        host->documentroot.data = mk_string_dup(documentroot);
        host->documentroot.len = strlen(documentroot);
    }
    else {
        host->documentroot.data = mk_string_dup("/dev/null");
        host->documentroot.len = sizeof("/dev/null") - 1;
    }

    config->server_software.data = mk_string_dup("");
    config->server_software.len = 0;
    config->default_mimetype = mk_string_dup(MIMETYPE_DEFAULT_TYPE);
    config->mimes_conf_file = MK_DEFAULT_MIMES_CONF_FILE;
    mk_mimetype_read_config();

    config->worker_capacity = mk_server_worker_capacity(config->workers);
    config->max_load = (config->worker_capacity * config->workers);

    /* Server listening socket */
    config->server_fd = mk_socket_server(config->serverport, config->listen_addr, MK_FALSE);

    /* Clock thread */
    mk_clock_sequential_init();
    a->clock = mk_utils_worker_spawn((void *) mk_clock_worker_init, NULL);

    mk_mem_pointers_init();
    mk_thread_keys_init();

    return a;

    out_config:
    free(config);

    out:
    free(a);

    return NULL;
}

/* NULL-terminated config call, consisting of pairs of config item and argument.
 * Returns MKLIB_FALSE on failure. */
int mklib_config(mklib_ctx ctx, ...)
{
    if (!ctx || ctx->lib_running) return MKLIB_FALSE;

    unsigned long len;
    int i;
    char *s;
    va_list va;

    va_start(va, ctx);

    i = va_arg(va, int);
    while (i) {
        const enum mklib_mkc e = i;

        switch(e) {
            case MKC_WORKERS:
                i = va_arg(va, int);
                config->workers = i;
                config->worker_capacity = mk_server_worker_capacity(config->workers);
                config->max_load = (config->worker_capacity * config->workers);

                free(sched_list);
                mk_sched_init();
            break;
            case MKC_TIMEOUT:
                i = va_arg(va, int);
                config->timeout = i;
            break;
            case MKC_USERDIR:
                s = va_arg(va, char *);
                if (config->user_dir) free(config->user_dir);
                config->user_dir = mk_string_dup(s);
            break;
            case MKC_INDEXFILE:
                s = va_arg(va, char *);
                if (config->index_files) mk_string_split_free(config->index_files);
                config->index_files = mk_string_split_line(s);
            break;
            case MKC_HIDEVERSION:
                i = va_arg(va, int);
                config->server_software.data = NULL;

                /* Basic server information */
                if (!i) {
                    mk_string_build(&config->server_software.data,
                                    &len, "libmonkey/%s (%s)", VERSION, OS);
                }
                else {
                    mk_string_build(&config->server_software.data, &len, "libmonkey");
                }
                config->server_software.len = len;

                /* Mark it so for the default vhost */
                struct mk_list *hosts = &config->hosts;
                struct host *def = mk_list_entry_first(hosts, struct host, _head);
                free(def->host_signature);
                free(def->header_host_signature.data);
                def->header_host_signature.data = NULL;

                def->host_signature = mk_string_dup(config->server_software.data);
                mk_string_build(&def->header_host_signature.data,
                                &def->header_host_signature.len,
                                "Server: %s", def->host_signature);
            break;
            case MKC_RESUME:
                i = va_arg(va, int);
                config->resume = i ? MK_TRUE : MK_FALSE;
            break;
            case MKC_KEEPALIVE:
                i = va_arg(va, int);
                config->keep_alive = i ? MK_TRUE : MK_FALSE;
            break;
            case MKC_KEEPALIVETIMEOUT:
                i = va_arg(va, int);
                config->keep_alive_timeout = i;
            break;
            case MKC_MAXKEEPALIVEREQUEST:
                i = va_arg(va, int);
                config->max_keep_alive_request = i;
            break;
            case MKC_MAXREQUESTSIZE:
                i = va_arg(va, int);
                config->max_request_size = i;
            break;
            case MKC_SYMLINK:
                i = va_arg(va, int);
                config->symlink = i ? MK_TRUE : MK_FALSE;
            break;
            case MKC_DEFAULTMIMETYPE:
                s = va_arg(va, char *);
                free(config->default_mimetype);
                config->default_mimetype = NULL;
                mk_string_build(&config->default_mimetype, &len, "%s\r\n", s);
                mk_ptr_set(&mimetype_default->type, config->default_mimetype);
            break;
            default:
                mk_warn("Unknown config option");
            break;
        }

        i = va_arg(va, int);
    }

    va_end(va);
    return MKLIB_TRUE;
}

/*
 * NULL-terminated config call retrieving monkey configuration.
 * Returns MKLIB_FALSE on failure
 */
int mklib_get_config(mklib_ctx ctx, ...)
{
    int i, *ip;
    va_list va;
    char *s;

    if (!ctx)
        return MKLIB_FALSE;

    va_start(va, ctx);

    i = va_arg(va, int);
    while (i) {
        const enum mklib_mkc e = i;

        switch(e) {
            case MKC_WORKERS:
                ip = va_arg(va, int *);
                *ip = config->workers;
                break;
            case MKC_TIMEOUT:
                ip = va_arg(va, int *);
                *ip = config->timeout;
                break;
            case MKC_USERDIR:
                s = va_arg(va, char *);
                if (config->user_dir)
                    memcpy(s, config->user_dir, strlen(config->user_dir) + 1);
                else
                    s[0] = 0;
                break;
            case MKC_RESUME:
                ip = va_arg(va, int *);
                *ip = config->resume;
                break;
            case MKC_KEEPALIVE:
                ip = va_arg(va, int *);
                *ip = config->keep_alive;
                break;
            case MKC_KEEPALIVETIMEOUT:
                ip = va_arg(va, int *);
                *ip = config->keep_alive_timeout;
                break;
            case MKC_MAXKEEPALIVEREQUEST:
                ip = va_arg(va, int *);
                *ip = config->max_keep_alive_request;
                break;
            case MKC_MAXREQUESTSIZE:
                ip = va_arg(va, int *);
                *ip = config->max_request_size;
                break;
            case MKC_SYMLINK:
                ip = va_arg(va, int *);
                *ip = config->symlink;
                break;
            case MKC_DEFAULTMIMETYPE:
                s = va_arg(va, char *);
                memcpy(s, config->default_mimetype, strlen(config->default_mimetype) + 1);
                break;
            default:
                mk_warn("Unknown config option");
        }

        i = va_arg(va, int);
    }

    return MKLIB_TRUE;
}

/* NULL-terminated config call creating a vhost with *name. Returns MKLIB_FALSE
 * on failure. */
int mklib_vhost_config(mklib_ctx ctx, const char *name, ...)
{
    if (!ctx) return MKLIB_FALSE;

    /* Does it exist already? */
    struct host *h = mklib_host_find(name);
    if (h) return MKLIB_FALSE;

    const struct host *defaulth = mklib_host_find("default");
    if (!defaulth) return MKLIB_FALSE;


    h = mk_mem_malloc_z(sizeof(struct host));
    h->file = mk_string_dup(name);

    h->documentroot.data = mk_string_dup("/dev/null");
    h->documentroot.len = sizeof("/dev/null") - 1;

    mk_list_init(&h->error_pages);
    mk_list_init(&h->server_names);

    char *s;
    int i;
    va_list va;

    va_start(va, name);

    i = va_arg(va, int);
    while (i) {
        const enum mklib_mkv e = i;

        switch(e) {
            case MKV_SERVERNAME:
                s = va_arg(va, char *);

                struct mk_list *head, *list = mk_string_split_line(s);

                mk_list_foreach(head, list) {
                    struct mk_string_line *entry = mk_list_entry(head,
                                                                 struct mk_string_line,
                                                                 _head);
                    if (entry->len > MK_HOSTNAME_LEN - 1) {
                        continue;
                    }

                    struct host_alias *alias = mk_mem_malloc_z(sizeof(struct host_alias));
                    alias->name = mk_string_tolower(entry->val);
                    alias->len = entry->len;
                    mk_list_add(&alias->_head, &h->server_names);
                }

                mk_string_split_free(list);
            break;
            case MKV_DOCUMENTROOT:
                s = va_arg(va, char *);
                free(h->documentroot.data);
                h->documentroot.data = mk_string_dup(s);
                h->documentroot.len = strlen(s);
            break;
            default:
                mk_warn("Unknown config option");
            break;
        }

        i = va_arg(va, int);
    }

    h->host_signature = mk_string_dup(defaulth->host_signature);
    h->header_host_signature.data = mk_string_dup(defaulth->header_host_signature.data);
    h->header_host_signature.len = defaulth->header_host_signature.len;

    mk_list_add(&h->_head, &config->hosts);
    config->nhosts++;

    va_end(va);
    return MKLIB_TRUE;
}

/* Start the server. */
int mklib_start(mklib_ctx ctx)
{
    unsigned int i;
    const unsigned int workers = config->workers;
    if (!ctx || ctx->lib_running)
        return MKLIB_FALSE;

    mk_plugin_core_process();

    ctx->workers = mk_mem_malloc_z(sizeof(pthread_t) * config->workers);

    ctx->worker_info = mk_mem_malloc_z(sizeof(struct mklib_worker_info *) * (workers + 1));
    for(i = 0; i < workers; i++) {
        ctx->worker_info[i] = mk_mem_malloc_z(sizeof(struct mklib_worker_info));
        mk_sched_launch_thread(config->worker_capacity, &ctx->workers[i], ctx);
    }

    /* Wait until all workers report as ready */
    while (1) {
        unsigned int ready = 0;

        pthread_mutex_lock(&mutex_worker_init);
        for (i = 0; i < workers; i++) {
            if (sched_list[i].initialized)
                ready++;
        }
        pthread_mutex_unlock(&mutex_worker_init);

        if (ready == workers) break;
        usleep(10000);
    }

    for(i = 0; i < workers; i++)
        ctx->worker_info[i]->pid = sched_list[i].pid;

    ctx->lib_running = 1;
    ctx->tid = mk_utils_worker_spawn(mklib_run, ctx);

    return MKLIB_TRUE;
}

/* Stop the server and free mklib_ctx. */
int mklib_stop(mklib_ctx ctx)
{
    if (!ctx || !ctx->lib_running) return MKLIB_FALSE;

    ctx->lib_running = 0;
    //pthread_cancel(ctx->tid);

    int i;
    for (i = 0; i < config->workers; i++) {
        pthread_cancel(ctx->workers[i]);
        free(ctx->worker_info[i]);
    }
    free(ctx->worker_info);

    mk_plugin_exit_all();
    mk_config_free_all();
    free(ctx->workers);
    free(ctx);

    return MKLIB_TRUE;
}

struct mklib_vhost **mklib_vhost_list(mklib_ctx ctx)
{
    static struct mklib_vhost **lst = NULL;
    struct host *entry_host;
    struct host_alias *alias;
    struct mk_list *head_vhost, *head_aliases;
    unsigned int i, total = 0, namecount;
    const char *names[50];

    if (!ctx) return NULL;

    /* Free it if it exists */
    if (lst) {
        for (i = 0; lst[i]; i++) {
            free((char *) lst[i]->server_names);
            free(lst[i]);
        }

        free(lst);
    }

    /* How many are there? */
    mk_list_foreach(head_vhost, &config->hosts) {
        total++;
    }
    total++;

    lst = mk_mem_malloc_z(sizeof(struct mklib_vhost *) * total);

    total = 0;

    /* Set up the list to return */
    mk_list_foreach(head_vhost, &config->hosts) {
        entry_host = mk_list_entry(head_vhost, struct host, _head);

        lst[total] = mk_mem_malloc_z(sizeof(struct mklib_vhost));

        lst[total]->name = entry_host->file;
        lst[total]->document_root = entry_host->documentroot.data;

        namecount = 0;
        unsigned int total_len = 1;
        mk_list_foreach(head_aliases, &entry_host->server_names) {
            alias = mk_list_entry(head_aliases, struct host_alias, _head);
            names[namecount] = alias->name;
            namecount++;
            total_len += alias->len + 1;
        }

	char *servernames = mk_mem_malloc_z(total_len);
        for (i = 0; i < namecount; i++) {
            strcat(servernames, names[i]);
            strcat(servernames, " ");
        }

        lst[total]->server_names = servernames;

        total++;
    }

    return lst;
}

struct mklib_worker_info **mklib_scheduler_worker_info(mklib_ctx ctx)
{
    unsigned int i;
    const unsigned int workers = config->workers;

    if (!ctx || !ctx->lib_running) return NULL;

    for (i = 0; i < workers; i++) {
        ctx->worker_info[i]->accepted_connections = sched_list[i].accepted_connections;
        ctx->worker_info[i]->closed_connections = sched_list[i].closed_connections;
    }

    return ctx->worker_info;
}

void mklib_print_worker_info(struct mklib_worker_info *mwi UNUSED_PARAM)
{
#ifdef STATS
    struct stats *stats = mwi->stats;
    printf("Stat info for worker: %d\n", mwi->pid);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_session_create", stats->mk_session_create[0], stats->mk_session_create[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_session_get", stats->mk_session_get[0], stats->mk_session_get[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_http_method_get", stats->mk_http_method_get[0], stats->mk_http_method_get[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_http_request_end", stats->mk_http_request_end[0], stats->mk_http_request_end[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_http_range_parse", stats->mk_http_range_parse[0], stats->mk_http_range_parse[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_http_init", stats->mk_http_init[0], stats->mk_http_init[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_sched_get_connection", stats->mk_sched_get_connection[0], stats->mk_sched_get_connection[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_sched_remove_client", stats->mk_sched_remove_client[0], stats->mk_sched_remove_client[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_plugin_stage_run", stats->mk_plugin_stage_run[0], stats->mk_plugin_stage_run[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_plugin_event_read", stats->mk_plugin_event_read[0], stats->mk_plugin_event_read[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_plugin_event_write", stats->mk_plugin_event_write[0], stats->mk_plugin_event_write[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_header_send", stats->mk_header_send[0], stats->mk_header_send[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_conn_read", stats->mk_conn_read[0], stats->mk_conn_read[1]);
    printf("%-25s: %8lld times:%10lld nanoseconds\n", "mk_conn_write", stats->mk_conn_write[0], stats->mk_conn_write[1]);
    printf("\n");
#else
    printf("Stat info for worker: %d\n", mwi->pid);
    printf("Open connections: %lld\n", mwi->accepted_connections);
    printf("Closed connections: %lld\n", mwi->closed_connections);
    printf("No more stats available, use \"./configure --stats\"\n");
    printf("\n");
#endif
}

/* Return a list of all mimetypes */
struct mklib_mime **mklib_mimetype_list(mklib_ctx ctx)
{
    if (!ctx) return NULL;

    static struct mklib_mime **lst = NULL;
    unsigned int i;
    static unsigned n_mime_items = 0;
    struct mk_list *head;

    if (lst) {
        for (i = 0; i < n_mime_items; i++) {
            free(lst[i]);
        }

        free(lst);
    }

    lst = mk_mem_malloc_z((mk_list_size(&mimetype_list) + 1) * sizeof(struct mklib_mime *));
    n_mime_items = 0;

    mk_list_foreach(head, &mimetype_list) {
        struct mimetype *entry = mk_list_entry(head, struct mimetype, _head);

        lst[n_mime_items] = mk_mem_malloc_z(sizeof(struct mklib_mime));
        lst[n_mime_items]->name = entry->name;
        lst[n_mime_items]->type = entry->type.data;
        n_mime_items++;
    }
    lst[n_mime_items] = NULL;

    return lst;
}

/* Add a new mimetype */
int mklib_mimetype_add(mklib_ctx ctx, char *name, const char *type)
{
    if (!ctx || !name || !type) return MKLIB_FALSE;

    /* Is it added already? */
    if (mk_mimetype_lookup(name)) return MKLIB_FALSE;

    mk_mimetype_add(name, type);

    return MKLIB_TRUE;
}

int mklib_get_request_header(const mklib_session *ms,
      const char *key,
      char **value)
{
   const struct session_request *sr = ms;
   int i, n;
   size_t len, key_len = strlen(key), value_len;
   const struct header_toc_row *row;

   if (!sr) {
      mk_err("mklib_get_request_header: mklib_session is not set.");
      return -1;
   }

   n = sr->headers_toc.length;
   row = sr->headers_toc.rows;

   for (i = 0; i < n; i++) {
      if (!row[i].end || !row[i].init) {
         continue;
      }
      if (row[i].end <= row[i].init) {
         continue;
      }
      len = row[i].end - row[i].init;
      // Expect at least 2 extra chars after key. "<key>: "
      if (len < key_len + 2 || !!strncasecmp(key, row[i].init, key_len)) {
         continue;
      }

      if (row[i].init[key_len] != ':') {
         // Partial match, skip
         continue;
      }

      value_len = len - (key_len + 2);
      if (value_len == 0) {
         // Value set to ""
         *value = NULL;
         return 0;
      }

      *value = mk_mem_malloc_z(value_len + 1);
      if (*value == NULL) {
         mk_err("mklib_get_request_header: Malloc failed.");
         return -1;
      }
      memcpy(*value, row[i].init + key_len + 2, value_len);
      return 0;
   }

   *value = NULL;
   return 0;
}

#endif
