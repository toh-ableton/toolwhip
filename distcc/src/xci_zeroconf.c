/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright 2005-2008 Apple Computer, Inc.
 * Copyright 2007 Lennart Poettering
 * Copyright 2009 Google Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/* Author: Mark Mentovai */

#include <config.h>

#ifdef XCODE_INTEGRATION

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dopt.h"
#include "trace.h"
#include "xci_zeroconf.h"

#if defined(HAVE_AVAHI) || defined(HAVE_DNSSD)

static const char *dcc_xci_zeroconf_service_name(void) {
    const char *service = getenv("DISTCCD_SERVICE_NAME");

    if (!service)
        service = "_xcodedistcc._tcp";

    return service;
}

#endif

#if defined(HAVE_AVAHI)

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/defs.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/thread-watch.h>

typedef struct {
    char *name;
    AvahiThreadedPoll *threaded_poll;
    AvahiClient *client;
    AvahiEntryGroup *entry_group;
} context;

static void dcc_xci_zeroconf_entry_callback(AvahiEntryGroup *,
                                            AvahiEntryGroupState, void *);

static void dcc_xci_zeroconf_do_register(context *ctx) {
    const char *service;

    if (!ctx->entry_group) {
        if (!(ctx->entry_group =
            avahi_entry_group_new(ctx->client,
                                  dcc_xci_zeroconf_entry_callback, ctx))) {
            rs_log_error("avahi_entry_group_new() failed: %s",
                         avahi_strerror(avahi_client_errno(ctx->client)));
            goto out_error;
        }
    }

    if (avahi_entry_group_is_empty(ctx->entry_group)) {
        service = dcc_xci_zeroconf_service_name();

        if (avahi_entry_group_add_service(ctx->entry_group,
                                          AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC,
                                          0, ctx->name, service,
                                          NULL, NULL, arg_port, NULL) < 0) {
            rs_log_error("avahi_entry_group_add_service failed: %s",
                         avahi_strerror(avahi_client_errno(ctx->client)));
            goto out_error;
        }

        if (avahi_entry_group_commit(ctx->entry_group) < 0) {
            rs_log_error("avahi_entry_group_commit failed: %s",
                         avahi_strerror(avahi_client_errno(ctx->client)));
            goto out_error;
        }
    }

    return;

  out_error:
    avahi_threaded_poll_quit(ctx->threaded_poll);
}

static void dcc_xci_zeroconf_entry_callback(AvahiEntryGroup *entry_group,
                                            AvahiEntryGroupState state,
                                            void *userdata) {
    context *ctx = userdata;
    char *name;

    /* Avoid unused parameter warning. */
    (void)entry_group;

    switch (state) {
        case AVAHI_ENTRY_GROUP_COLLISION:
            if (!(name = avahi_alternative_service_name(ctx->name))) {
                rs_log_error("avahi_alternative_service_name failed");
                goto out_error;
            }

            avahi_free(ctx->name);
            ctx->name = name;

            dcc_xci_zeroconf_do_register(ctx);

            break;

        case AVAHI_ENTRY_GROUP_FAILURE:
            rs_log_error("entry group failure: %s",
                         avahi_strerror(avahi_client_errno(ctx->client)));
            goto out_error;
            break;

        case AVAHI_ENTRY_GROUP_ESTABLISHED:
            rs_log_info("registered as \"%s.%s.%s\"",
                        avahi_client_get_host_name(ctx->client),
                        dcc_xci_zeroconf_service_name(),
                        avahi_client_get_domain_name(ctx->client));
            break;

        case AVAHI_ENTRY_GROUP_UNCOMMITED:
        case AVAHI_ENTRY_GROUP_REGISTERING:
            break;
    }

    return;

  out_error:
    avahi_threaded_poll_quit(ctx->threaded_poll);
}

static void dcc_xci_zeroconf_client_callback(AvahiClient *client,
                                             AvahiClientState state,
                                             void *userdata) {
    context *ctx = userdata;
    const AvahiPoll *threaded_poll;
    int error;

    ctx->client = client;

    switch (state) {
        case AVAHI_CLIENT_S_RUNNING:
            dcc_xci_zeroconf_do_register(ctx);
            break;

        case AVAHI_CLIENT_S_COLLISION:
        case AVAHI_CLIENT_S_REGISTERING:
            if (ctx->entry_group)
                avahi_entry_group_reset(ctx->entry_group);
            break;

        case AVAHI_CLIENT_FAILURE:
            if (avahi_client_errno(client) == AVAHI_ERR_DISCONNECTED) {
                avahi_client_free(ctx->client);
                ctx->client = NULL;
                ctx->entry_group = NULL;

                threaded_poll = avahi_threaded_poll_get(ctx->threaded_poll);

                if (!(ctx->client =
                    avahi_client_new(threaded_poll, AVAHI_CLIENT_NO_FAIL,
                                     dcc_xci_zeroconf_client_callback,
                                     ctx, &error))) {
                    rs_log_error("avahi_client_new() failed: %s",
                                 avahi_strerror(error));
                    goto out_error;
                }
            } else {
                rs_log_error("client failure: %s",
                             avahi_strerror(avahi_client_errno(client)));
                goto out_error;
            }

            break;

        case AVAHI_CLIENT_CONNECTING:
            break;
    }

    return;

  out_error:
    avahi_threaded_poll_quit(ctx->threaded_poll);
}

void *dcc_xci_zeroconf_register(void) {
    char service[] = "xcodedistcc@";
    char hostname[_POSIX_HOST_NAME_MAX + 1];
    context *ctx = NULL;
    const AvahiPoll *threaded_poll;
    int len, error;

    if (!(ctx = calloc(1, sizeof(context)))) {
        rs_log_error("calloc() failed: %s", strerror(errno));
        goto out_error;
    }

    if (gethostname(hostname, sizeof(hostname))) {
        rs_log_error("gethostname() failed: %s", strerror(errno));
        goto out_error;
    }

    len = sizeof(service) + strlen(hostname);
    if (!(ctx->name = avahi_malloc(len))) {
        rs_log_error("avahi_malloc() failed");
        goto out_error;
    }

    snprintf(ctx->name, len, "%s%s", service, hostname);

    if (!(ctx->threaded_poll = avahi_threaded_poll_new())) {
        rs_log_error("avahi_threaded_poll_new() failed");
        goto out_error;
    }

    threaded_poll = avahi_threaded_poll_get(ctx->threaded_poll);

    if (!(ctx->client= avahi_client_new(threaded_poll, AVAHI_CLIENT_NO_FAIL,
                                        dcc_xci_zeroconf_client_callback,
                                        ctx, &error))) {
        rs_log_error("avahi_client_new() failed: %s", avahi_strerror(error));
        goto out_error;
    }

    if (avahi_threaded_poll_start(ctx->threaded_poll) < 0) {
        rs_log_error("avahi_threaded_poll_start() failed");
        goto out_error;
    }

    return ctx;

  out_error:
    if (ctx) {
        if (ctx->threaded_poll)
            avahi_threaded_poll_stop(ctx->threaded_poll);
        if (ctx->client)
            avahi_client_free(ctx->client);
        if (ctx->threaded_poll)
            avahi_threaded_poll_free(ctx->threaded_poll);
        if (ctx->name)
            avahi_free(ctx->name);

        free(ctx);
    }

    return NULL;
}

#elif defined(HAVE_DNSSD)

#include <dns_sd.h>
#include <pthread.h>

typedef struct {
    pthread_t thread;
    int thread_live;
    DNSServiceRef zc;
} context;

static void dcc_xci_zeroconf_reply(const DNSServiceRef zeroconf,
                                   const DNSServiceFlags flags,
                                   const DNSServiceErrorType error,
                                   const char *name,
                                   const char *reg_type,
                                   const char *domain,
                                   void *user_data) {
    context *ctx = user_data;

    /* Avoid unused parameter warnings. */
    (void)zeroconf;
    (void)flags;

    if (error != kDNSServiceErr_NoError) {
        rs_log_error("received error %d", error);
        ctx->thread_live = 0;
        pthread_exit(NULL);
    } else {
        rs_log_info("registered as \"%s.%s%s\"", name, reg_type, domain);
    }
}

static void *dcc_xci_zeroconf_main(void *param) {
    context *ctx = param;
    const char *service;
    DNSServiceErrorType rv;

    service = dcc_xci_zeroconf_service_name();

    if ((rv = DNSServiceRegister(&ctx->zc, 0, 0, NULL, service,
                                 NULL, NULL, htons(arg_port), 0, NULL,
                                 dcc_xci_zeroconf_reply, ctx)) !=
        kDNSServiceErr_NoError) {
        rs_log_error("DNSServiceRegister() failed: error %d", rv);
        return NULL;
    }

    while ((rv = DNSServiceProcessResult(ctx->zc) == kDNSServiceErr_NoError)) {
    }

    rs_log_error("DNSServiceProcessResult() failed: error %d", rv);

    ctx->thread_live = 0;
    return NULL;
}

void *dcc_xci_zeroconf_register(void) {
    int rv;
    context *ctx = NULL;

    if (!(ctx = calloc(1, sizeof(context)))) {
        rs_log_error("calloc() failed: %s", strerror(errno));
        goto out_error;
    }

    if ((rv = pthread_create(&ctx->thread, NULL,
                             dcc_xci_zeroconf_main, ctx))) {
        rs_log_error("pthread_create() failed: %s", strerror(rv));
    }

    ctx->thread_live = 1;

    return ctx;

  out_error:
    if (ctx) {
        free(ctx);
    }

    return NULL;
}

#endif /* HAVE_DNSSD */

#endif /* XCODE_INTEGRATION */
