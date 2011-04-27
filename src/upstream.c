/* tinyproxy - A fast light-weight HTTP proxy
 * Copyright (C) 1998 Steven Young <sdyoung@miranda.org>
 * Copyright (C) 1999-2005 Robert James Kaes <rjkaes@users.sourceforge.net>
 * Copyright (C) 2000 Chris Lightfoot <chris@ex-parrot.com>
 * Copyright (C) 2002 Petr Lampa <lampa@fit.vutbr.cz>
 * Copyright (C) 2009 Michael Adam <obnox@samba.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/*
 * Routines for handling the list of upstream proxies.
 */

#include "upstream.h"
#include "heap.h"
#include "log.h"

#ifdef UPSTREAM_SUPPORT
/**
 * Construct an upstream struct from input data.
 */
static struct upstream *upstream_build (const char *user, const char *pwd, const char *host, int port, const char *domain)
{
        char *ptr;
        struct upstream *up;

        up = (struct upstream *) safemalloc (sizeof (struct upstream));
        if (!up) {
                log_message (LOG_ERR,
                             "Unable to allocate memory in upstream_build()");
                return NULL;
        }

        up->host = up->domain = NULL;
        up->ip = up->mask = 0;
        up->user = up->pwd = "";
        if(user != NULL) up->user = safestrdup(user);
        if(pwd != NULL) up->pwd = safestrdup(pwd);

        if (domain == NULL) {
                if (!host || host[0] == '\0' || port < 1) {
                        log_message (LOG_WARNING,
                                     "Nonsense upstream rule: invalid host or port");
                        goto fail;
                }

                up->host = safestrdup (host);
                up->port = port;

                log_message (LOG_INFO, "Added upstream %s:%d for [default]",
                             host, port);
        } else if (host == NULL) {
                if (!domain || domain[0] == '\0') {
                        log_message (LOG_WARNING,
                                     "Nonsense no-upstream rule: empty domain");
                        goto fail;
                }

                ptr = strchr (domain, '/');
                if (ptr) {
                        struct in_addr addrstruct;

                        *ptr = '\0';
                        if (inet_aton (domain, &addrstruct) != 0) {
                                up->ip = ntohl (addrstruct.s_addr);
                                *ptr++ = '/';

                                if (strchr (ptr, '.')) {
                                        if (inet_aton (ptr, &addrstruct) != 0)
                                                up->mask =
                                                    ntohl (addrstruct.s_addr);
                                } else {
                                        up->mask =
                                            ~((1 << (32 - atoi (ptr))) - 1);
                                }
                        }
                } else {
                        up->domain = safestrdup (domain);
                }

                log_message (LOG_INFO, "Added no-upstream for %s", domain);
        } else {
                if (!host || host[0] == '\0' || port < 1 || !domain
                    || domain == '\0') {
                        log_message (LOG_WARNING,
                                     "Nonsense upstream rule: invalid parameters");
                        goto fail;
                }

                up->host = safestrdup (host);
                up->port = port;
                up->domain = safestrdup (domain);

                log_message (LOG_INFO, "Added upstream %s:<pwd>@%s:%d for %s",
                             user, host, port, domain);
        }

        return up;

fail:
        safefree (up->host);
        safefree (up->domain);
        safefree (up);

        return NULL;
}

/*
 * Add an entry to the upstream list
 */
void upstream_add (const char *user, const char *pwd, const char *host, int port, const char *domain,
                   struct upstream **upstream_list)
{
        struct upstream *up;

        up = upstream_build (user, pwd, host, port, domain);
        if (up == NULL) {
                return;
        }

        if (!up->domain && !up->ip) {   /* always add default to end */
                struct upstream *tmp = *upstream_list;

                while (tmp) {
                        if (!tmp->domain && !tmp->ip) {
                                log_message (LOG_WARNING,
                                             "Duplicate default upstream");
                                goto upstream_cleanup;
                        }

                        if (!tmp->next) {
                                up->next = NULL;
                                tmp->next = up;
                                return;
                        }

                        tmp = tmp->next;
                }
        }

        up->next = *upstream_list;
        *upstream_list = up;

        return;

upstream_cleanup:
        safefree (up->host);
        safefree (up->domain);
        safefree (up);

        return;
}

/*
 * Check if a host is in the upstream list
 */
struct upstream *upstream_get (char *host, struct upstream *up)
{
        in_addr_t my_ip = INADDR_NONE;

        while (up) {
                if (up->domain) {
                        if (strcasecmp (host, up->domain) == 0)
                                break;  /* exact match */

                        if (up->domain[0] == '.') {
                                char *dot = strchr (host, '.');

                                if (!dot && !up->domain[1])
                                        break;  /* local host matches "." */

                                while (dot && strcasecmp (dot, up->domain))
                                        dot = strchr (dot + 1, '.');

                                if (dot)
                                        break;  /* subdomain match */
                        }
                } else if (up->ip) {
                        if (my_ip == INADDR_NONE)
                                my_ip = ntohl (inet_addr (host));

                        if ((my_ip & up->mask) == up->ip)
                                break;
                } else {
                        break;  /* No domain or IP, default upstream */
                }

                up = up->next;
        }

        if (up && (!up->host || !up->port))
                up = NULL;

        if (up)
                log_message (LOG_INFO, "Found upstream proxy %s:%d for %s",
                             up->host, up->port, host);
        else
                log_message (LOG_INFO, "No upstream proxy for %s", host);

        return up;
}

void free_upstream_list (struct upstream *up)
{
        while (up) {
                struct upstream *tmp = up;
                up = up->next;
                safefree (tmp->domain);
                safefree (tmp->host);
                safefree (tmp);
        }
}

#endif
