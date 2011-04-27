/* tinyproxy - A fast light-weight HTTP proxy
 * Copyright (C) 2004 Robert James Kaes <rjkaes@users.sourceforge.net>
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

/* Parses the configuration file and sets up the config_s structure for
 * use by the application.  This file replaces the old grammar.y and
 * scanner.l files.  It takes up less space and _I_ think is easier to
 * add new directives to.  Who knows if I'm right though.
 */

#include "conf.h"

#include "acl.h"
#include "anonymous.h"
#include "child.h"
#include "filter.h"
#include "heap.h"
#include "html-error.h"
#include "log.h"
#include "reqs.h"
#include "reverse-proxy.h"
#include "upstream.h"
#include "connect-ports.h"

/*
 * The configuration directives are defined in the structure below.  Each
 * directive requires a regular expression to match against, and a
 * function to call when the regex is matched.
 *
 * Below are defined certain constant regular expression strings that
 * can (and likely should) be used when building the regex for the
 * given directive.
 */
#define WS "[[:space:]]+"
#define STR "\"([^\"]+)\""
#define BOOL "(yes|on|no|off)"
#define INT "((0x)?[[:digit:]]+)"
#define ALNUM "([-a-z0-9._]+)"
#define IP "((([0-9]{1,3})\\.){3}[0-9]{1,3})"
#define IPMASK "(" IP "(/[[:digit:]]+)?)"
#define IPV6 "(" \
        "(([0-9a-f]{1,4}:){1,1}(:[0-9a-f]{1,4}){1,6})|" \
        "(([0-9a-f]{1,4}:){1,2}(:[0-9a-f]{1,4}){1,5})|" \
        "(([0-9a-f]{1,4}:){1,3}(:[0-9a-f]{1,4}){1,4})|" \
        "(([0-9a-f]{1,4}:){1,4}(:[0-9a-f]{1,4}){1,3})|" \
        "(([0-9a-f]{1,4}:){1,5}(:[0-9a-f]{1,4}){1,2})|" \
        "(([0-9a-f]{1,4}:){1,6}(:[0-9a-f]{1,4}){1,1})|" \
        "((([0-9a-f]{1,4}:){1,7}|:):)|" \
        "(:(:[0-9a-f]{1,4}){1,7})|" \
        "([0-9a-f]{1,4}(:[0-9a-f]{1,4}){1,7})|" \
        "(((([0-9a-f]{1,4}:){6})(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3}))|" \
        "((([0-9a-f]{1,4}:){5}[0-9a-f]{1,4}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3}))|" \
        "(([0-9a-f]{1,4}:){5}:[0-9a-f]{1,4}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "(([0-9a-f]{1,4}:){1,1}(:[0-9a-f]{1,4}){1,4}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "(([0-9a-f]{1,4}:){1,2}(:[0-9a-f]{1,4}){1,3}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "(([0-9a-f]{1,4}:){1,3}(:[0-9a-f]{1,4}){1,2}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "(([0-9a-f]{1,4}:){1,4}(:[0-9a-f]{1,4}){1,1}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "((([0-9a-f]{1,4}:){1,5}|:):(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})|" \
        "(:(:[0-9a-f]{1,4}){1,5}:(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)(\\.(25[0-5]|2[0-4]\\d|[0-1]?\\d?\\d)){3})" \
        ")"

#define IPV6MASK "(" IPV6 "(/[[:digit:]]+)?)"
#define BEGIN "^[[:space:]]*"
#define END "[[:space:]]*$"

/*
 * Limit the maximum number of substring matches to a reasonably high
 * number.  Given the usual structure of the configuration file, sixteen
 * substring matches should be plenty.
 */
#define RE_MAX_MATCHES 16

/*
 * All configuration handling functions are REQUIRED to be defined
 * with the same function template as below.
 */
typedef int (*CONFFILE_HANDLER) (struct config_s *, const char *, regmatch_t[]);

/*
 * Define the pattern used by any directive handling function.  The
 * following arguments are defined:
 *
 *   struct config_s* conf   pointer to the current configuration structure
 *   const char* line          full line matched by the regular expression
 *   regmatch_t match[]        offsets to the substrings matched
 *
 * The handling function must return 0 if the directive was processed
 * properly.  Any errors are reported by returning a non-zero value.
 */
#define HANDLE_FUNC(func) \
  int func(struct config_s* conf, const char* line, \
           regmatch_t match[])

/*
 * List all the handling functions.  These are defined later, but they need
 * to be in-scope before the big structure below.
 */
static HANDLE_FUNC (handle_nop)
{
        return 0;
}                               /* do nothing function */

static HANDLE_FUNC (handle_allow);
static HANDLE_FUNC (handle_anonymous);
static HANDLE_FUNC (handle_bind);
static HANDLE_FUNC (handle_bindsame);
static HANDLE_FUNC (handle_connectport);
static HANDLE_FUNC (handle_defaulterrorfile);
static HANDLE_FUNC (handle_deny);
static HANDLE_FUNC (handle_errorfile);
static HANDLE_FUNC (handle_addheader);
#ifdef FILTER_ENABLE
static HANDLE_FUNC (handle_filter);
static HANDLE_FUNC (handle_filtercasesensitive);
static HANDLE_FUNC (handle_filterdefaultdeny);
static HANDLE_FUNC (handle_filterextended);
static HANDLE_FUNC (handle_filterurls);
#endif
static HANDLE_FUNC (handle_group);
static HANDLE_FUNC (handle_listen);
static HANDLE_FUNC (handle_logfile);
static HANDLE_FUNC (handle_loglevel);
static HANDLE_FUNC (handle_maxclients);
static HANDLE_FUNC (handle_maxrequestsperchild);
static HANDLE_FUNC (handle_maxspareservers);
static HANDLE_FUNC (handle_minspareservers);
static HANDLE_FUNC (handle_pidfile);
static HANDLE_FUNC (handle_port);
#ifdef REVERSE_SUPPORT
static HANDLE_FUNC (handle_reversebaseurl);
static HANDLE_FUNC (handle_reversemagic);
static HANDLE_FUNC (handle_reverseonly);
static HANDLE_FUNC (handle_reversepath);
#endif
static HANDLE_FUNC (handle_startservers);
static HANDLE_FUNC (handle_statfile);
static HANDLE_FUNC (handle_stathost);
static HANDLE_FUNC (handle_syslog);
static HANDLE_FUNC (handle_timeout);

static HANDLE_FUNC (handle_user);
static HANDLE_FUNC (handle_viaproxyname);
static HANDLE_FUNC (handle_disableviaheader);
static HANDLE_FUNC (handle_xtinyproxy);

#ifdef UPSTREAM_SUPPORT
static HANDLE_FUNC (handle_upstream);
static HANDLE_FUNC (handle_upstream_no);
#endif

static void config_free_regex (void);

/*
 * This macro can be used to make standard directives in the form:
 *   directive arguments [arguments ...]
 *
 * The directive itself will be the first matched substring.
 *
 * Note that this macro is not required.  As you can see below, the
 * comment and blank line elements are defined explicitly since they
 * do not follow the pattern above.  This macro is for convenience
 * only.
 */
#define STDCONF(d, re, func) { BEGIN "(" d ")" WS re END, func, NULL }

/*
 * Holds the regular expression used to match the configuration directive,
 * the function pointer to the routine to handle the directive, and
 * for internal use, a pointer to the compiled regex so it only needs
 * to be compiled one.
 */
struct {
        const char *re;
        CONFFILE_HANDLER handler;
        regex_t *cre;
} directives[] = {
        /* comments */
        {
                BEGIN "#", handle_nop, NULL
        },
        /* blank lines */
        {
                "^[[:space:]]+$", handle_nop, NULL
        },
        /* string arguments */
        STDCONF ("logfile", STR, handle_logfile),
        STDCONF ("pidfile", STR, handle_pidfile),
        STDCONF ("anonymous", STR, handle_anonymous),
        STDCONF ("viaproxyname", STR, handle_viaproxyname),
        STDCONF ("defaulterrorfile", STR, handle_defaulterrorfile),
        STDCONF ("statfile", STR, handle_statfile),
        STDCONF ("stathost", STR, handle_stathost),
        STDCONF ("xtinyproxy",  BOOL, handle_xtinyproxy),
        /* boolean arguments */
        STDCONF ("syslog", BOOL, handle_syslog),
        STDCONF ("bindsame", BOOL, handle_bindsame),
        STDCONF ("disableviaheader", BOOL, handle_disableviaheader),
        /* integer arguments */
        STDCONF ("port", INT, handle_port),
        STDCONF ("maxclients", INT, handle_maxclients),
        STDCONF ("maxspareservers", INT, handle_maxspareservers),
        STDCONF ("minspareservers", INT, handle_minspareservers),
        STDCONF ("startservers", INT, handle_startservers),
        STDCONF ("maxrequestsperchild", INT, handle_maxrequestsperchild),
        STDCONF ("timeout", INT, handle_timeout),
        STDCONF ("connectport", INT, handle_connectport),
        /* alphanumeric arguments */
        STDCONF ("user", ALNUM, handle_user),
        STDCONF ("group", ALNUM, handle_group),
        /* ip arguments */
        STDCONF ("listen", "(" IP "|" IPV6 ")", handle_listen),
        STDCONF ("allow", "(" "(" IPMASK "|" IPV6MASK ")" "|" ALNUM ")",
                 handle_allow),
        STDCONF ("deny", "(" "(" IPMASK "|" IPV6MASK ")" "|" ALNUM ")",
                 handle_deny),
        STDCONF ("bind", "(" IP "|" IPV6 ")", handle_bind),
        /* other */
        STDCONF ("errorfile", INT WS STR, handle_errorfile),
        STDCONF ("addheader",  STR WS STR, handle_addheader),

#ifdef FILTER_ENABLE
        /* filtering */
        STDCONF ("filter", STR, handle_filter),
        STDCONF ("filterurls", BOOL, handle_filterurls),
        STDCONF ("filterextended", BOOL, handle_filterextended),
        STDCONF ("filterdefaultdeny", BOOL, handle_filterdefaultdeny),
        STDCONF ("filtercasesensitive", BOOL, handle_filtercasesensitive),
#endif
#ifdef REVERSE_SUPPORT
        /* Reverse proxy arguments */
        STDCONF ("reversebaseurl", STR, handle_reversebaseurl),
        STDCONF ("reverseonly", BOOL, handle_reverseonly),
        STDCONF ("reversemagic", BOOL, handle_reversemagic),
        STDCONF ("reversepath", STR "(" WS STR ")?", handle_reversepath),
#endif
#ifdef UPSTREAM_SUPPORT
        /* upstream is rather complicated */
        {
                BEGIN "(no" WS "upstream)" WS STR END, handle_upstream_no, NULL
        },
        {
                BEGIN "(upstream)" WS "(" IP "|" ALNUM ")" ":" INT "(" WS STR
                      ")?" END, handle_upstream, NULL
        },
#endif
        /* loglevel */
        STDCONF ("loglevel", "(critical|error|warning|notice|connect|info)",
                 handle_loglevel)
};

const unsigned int ndirectives = sizeof (directives) / sizeof (directives[0]);

static void
free_added_headers (vector_t add_headers)
{
        ssize_t i;

        for (i = 0; i < vector_length (add_headers); i++) {
                http_header_t *header = (http_header_t *)
                        vector_getentry (add_headers, i, NULL);

                safefree (header->name);
                safefree (header->value);
        }

        vector_delete (add_headers);
}

static void free_config (struct config_s *conf)
{
        safefree (conf->config_file);
        safefree (conf->logf_name);
        safefree (conf->stathost);
        safefree (conf->user);
        safefree (conf->group);
        safefree (conf->ipAddr);
#ifdef FILTER_ENABLE
        safefree (conf->filter);
#endif                          /* FILTER_ENABLE */
#ifdef REVERSE_SUPPORT
        free_reversepath_list(conf->reversepath_list);
        safefree (conf->reversebaseurl);
#endif
#ifdef UPSTREAM_SUPPORT
        free_upstream_list (conf->upstream_list);
#endif                          /* UPSTREAM_SUPPORT */
        safefree (conf->pidpath);
        safefree (conf->bind_address);
        safefree (conf->via_proxy_name);
        hashmap_delete (conf->errorpages);
        free_added_headers (conf->add_headers);
        safefree (conf->errorpage_undef);
        safefree (conf->statpage);
        flush_access_list (conf->access_list);
        free_connect_ports_list (conf->connect_ports);
        hashmap_delete (conf->anonymous_map);

        memset (conf, 0, sizeof(*conf));
}

/*
 * Compiles the regular expressions used by the configuration file.  This
 * routine MUST be called before trying to parse the configuration file.
 *
 * Returns 0 on success; negative upon failure.
 */
int
config_compile_regex (void)
{
        unsigned int i, r;

        for (i = 0; i != ndirectives; ++i) {
                assert (directives[i].handler);
                assert (!directives[i].cre);

                directives[i].cre = (regex_t *) safemalloc (sizeof (regex_t));
                if (!directives[i].cre)
                        return -1;

                r = regcomp (directives[i].cre,
                             directives[i].re,
                             REG_EXTENDED | REG_ICASE | REG_NEWLINE);
                if (r)
                        return r;
        }

        atexit (config_free_regex);

        return 0;
}

/*
 * Frees pre-compiled regular expressions used by the configuration
 * file. This function is registered to be automatically called at exit.
 */
static void
config_free_regex (void)
{
        unsigned int i;

        for (i = 0; i < ndirectives; i++) {
                if (directives[i].cre) {
                        regfree (directives[i].cre);
                        safefree (directives[i].cre);
                        directives[i].cre = NULL;
                }
        }
}

/*
 * Attempt to match the supplied line with any of the configuration
 * regexes defined above.  If a match is found, call the handler
 * function to process the directive.
 *
 * Returns 0 if a match was found and successfully processed; otherwise,
 * a negative number is returned.
 */
static int check_match (struct config_s *conf, const char *line)
{
        regmatch_t match[RE_MAX_MATCHES];
        unsigned int i;

        assert (ndirectives > 0);

        for (i = 0; i != ndirectives; ++i) {
                assert (directives[i].cre);
                if (!regexec
                    (directives[i].cre, line, RE_MAX_MATCHES, match, 0))
                        return (*directives[i].handler) (conf, line, match);
        }

        return -1;
}

/*
 * Parse the previously opened configuration stream.
 */
static int config_parse (struct config_s *conf, FILE * f)
{
        char buffer[1024];      /* 1KB lines should be plenty */
        unsigned long lineno = 1;

        while (fgets (buffer, sizeof (buffer), f)) {
                if (check_match (conf, buffer)) {
                        printf ("Syntax error on line %ld\n", lineno);
                        return 1;
                }
                ++lineno;
        }
        return 0;
}

/**
 * Read the settings from a config file.
 */
static int load_config_file (const char *config_fname, struct config_s *conf)
{
        FILE *config_file;
        int ret = -1;

        config_file = fopen (config_fname, "r");
        if (!config_file) {
                fprintf (stderr,
                         "%s: Could not open config file \"%s\".\n",
                         PACKAGE, config_fname);
                goto done;
        }

        if (config_parse (conf, config_file)) {
                fprintf (stderr, "Unable to parse config file. "
                         "Not starting.\n");
                goto done;
        }

        ret = 0;

done:
        if (config_file)
                fclose (config_file);

        return ret;
}

static void initialize_with_defaults (struct config_s *conf,
                                      struct config_s *defaults)
{
        if (defaults->logf_name) {
                conf->logf_name = safestrdup (defaults->logf_name);
        }

        if (defaults->config_file) {
                conf->config_file = safestrdup (defaults->config_file);
        }

        conf->syslog = defaults->syslog;
        conf->port = defaults->port;

        if (defaults->stathost) {
                conf->stathost = safestrdup (defaults->stathost);
        }

        conf->godaemon = defaults->godaemon;
        conf->quit = defaults->quit;

        if (defaults->user) {
                conf->user = safestrdup (defaults->user);
        }

        if (defaults->group) {
                conf->group = safestrdup (defaults->group);
        }

        if (defaults->ipAddr) {
                conf->ipAddr = safestrdup (defaults->ipAddr);
        }

#ifdef FILTER_ENABLE
        if (defaults->filter) {
                conf->filter = safestrdup (defaults->filter);
        }

        conf->filter_url = defaults->filter_url;
        conf->filter_extended = defaults->filter_extended;
        conf->filter_casesensitive = defaults->filter_casesensitive;
#endif                          /* FILTER_ENABLE */

#ifdef XTINYPROXY_ENABLE
        conf->add_xtinyproxy = defaults->add_xtinyproxy;
#endif

#ifdef REVERSE_SUPPORT
        /* struct reversepath *reversepath_list; */
        conf->reverseonly = defaults->reverseonly;
        conf->reversemagic = defaults->reversemagic;

        if (defaults->reversebaseurl) {
                conf->reversebaseurl = safestrdup (defaults->reversebaseurl);
        }
#endif

#ifdef UPSTREAM_SUPPORT
        /* struct upstream *upstream_list; */
#endif                          /* UPSTREAM_SUPPORT */

        if (defaults->pidpath) {
                conf->pidpath = safestrdup (defaults->pidpath);
        }

        conf->idletimeout = defaults->idletimeout;

        if (defaults->bind_address) {
                conf->bind_address = safestrdup (defaults->bind_address);
        }

        conf->bindsame = defaults->bindsame;

        if (defaults->via_proxy_name) {
                conf->via_proxy_name = safestrdup (defaults->via_proxy_name);
        }

        conf->disable_viaheader = defaults->disable_viaheader;

        if (defaults->errorpage_undef) {
                conf->errorpage_undef = safestrdup (defaults->errorpage_undef);
        }

        if (defaults->statpage) {
                conf->statpage = safestrdup (defaults->statpage);
        }

        /* vector_t access_list; */
        /* vector_t connect_ports; */
        /* hashmap_t anonymous_map; */
}

/**
 * Load the configuration.
 */
int reload_config_file (const char *config_fname, struct config_s *conf,
                        struct config_s *defaults)
{
        int ret;

        log_message (LOG_INFO, "Reloading config file");

        free_config (conf);

        initialize_with_defaults (conf, defaults);

        ret = load_config_file (config_fname, conf);
        if (ret != 0) {
                goto done;
        }

        /* Set the default values if they were not set in the config file. */
        if (conf->port == 0) {
                /*
                 * Don't log here in error path:
                 * logging might not be set up yet!
                 */
                fprintf (stderr, PACKAGE ": You MUST set a Port in the "
                         "config file.\n");
                ret = -1;
                goto done;
        }

        if (!conf->user) {
                log_message (LOG_WARNING, "You SHOULD set a UserName in the "
                             "config file. Using current user instead.");
        }

        if (conf->idletimeout == 0) {
                log_message (LOG_WARNING, "Invalid idle time setting. "
                             "Only values greater than zero are allowed. "
                             "Therefore setting idle timeout to %u seconds.",
                             MAX_IDLE_TIME);
                conf->idletimeout = MAX_IDLE_TIME;
        }

done:
        return ret;
}

/***********************************************************************
 *
 * The following are basic data extraction building blocks that can
 * be used to simplify the parsing of a directive.
 *
 ***********************************************************************/

static char *get_string_arg (const char *line, regmatch_t * match)
{
        char *p;
        const unsigned int len = match->rm_eo - match->rm_so;

        assert (line);
        assert (len > 0);

        p = (char *) safemalloc (len + 1);
        if (!p)
                return NULL;

        memcpy (p, line + match->rm_so, len);
        p[len] = '\0';
        return p;
}

static int set_string_arg (char **var, const char *line, regmatch_t * match)
{
        char *arg = get_string_arg (line, match);

        if (!arg)
                return -1;

        if (*var != NULL) {
                safefree (*var);
        }

        *var = arg;

        return 0;
}

static int get_bool_arg (const char *line, regmatch_t * match)
{
        const char *p = line + match->rm_so;

        assert (line);
        assert (match && match->rm_so != -1);

        /* "y"es or o"n" map as true, otherwise it's false. */
        if (tolower (p[0]) == 'y' || tolower (p[1]) == 'n')
                return 1;
        else
                return 0;
}

static int
set_bool_arg (unsigned int *var, const char *line, regmatch_t * match)
{
        assert (var);
        assert (line);
        assert (match && match->rm_so != -1);

        *var = get_bool_arg (line, match);
        return 0;
}

static unsigned long
get_long_arg (const char *line, regmatch_t * match)
{
        assert (line);
        assert (match && match->rm_so != -1);

        return strtoul (line + match->rm_so, NULL, 0);
}

static int
set_int_arg (unsigned int *var, const char *line, regmatch_t * match)
{
        assert (var);
        assert (line);
        assert (match);

        *var = (unsigned int) get_long_arg (line, match);
        return 0;
}

/***********************************************************************
 *
 * Below are all the directive handling functions.  You will notice
 * that most of the directives delegate to one of the basic data
 * extraction routines.  This is deliberate.  To add a new directive
 * to tinyproxy only requires you to define the regular expression
 * above and then figure out what data extract routine to use.
 *
 * However, you will also notice that more complicated directives are
 * possible.  You can make your directive as complicated as you require
 * to express a solution to the problem you're tackling.
 *
 * See the definition/comment about the HANDLE_FUNC() macro to learn
 * what arguments are supplied to the handler, and to determine what
 * values to return.
 *
 ***********************************************************************/

static HANDLE_FUNC (handle_logfile)
{
        return set_string_arg (&conf->logf_name, line, &match[2]);
}

static HANDLE_FUNC (handle_pidfile)
{
        return set_string_arg (&conf->pidpath, line, &match[2]);
}

static HANDLE_FUNC (handle_anonymous)
{
        char *arg = get_string_arg (line, &match[2]);

        if (!arg)
                return -1;

        anonymous_insert (arg);
        safefree (arg);
        return 0;
}

static HANDLE_FUNC (handle_viaproxyname)
{
        int r = set_string_arg (&conf->via_proxy_name, line, &match[2]);

        if (r)
                return r;
        log_message (LOG_INFO,
                     "Setting \"Via\" header to '%s'",
                     conf->via_proxy_name);
        return 0;
}

static HANDLE_FUNC (handle_disableviaheader)
{
        int r = set_bool_arg (&conf->disable_viaheader, line, &match[2]);

        if (r) {
                return r;
        }

        log_message (LOG_INFO,
                     "Disabling transmission of the \"Via\" header.");
        return 0;
}

static HANDLE_FUNC (handle_defaulterrorfile)
{
        return set_string_arg (&conf->errorpage_undef, line, &match[2]);
}

static HANDLE_FUNC (handle_statfile)
{
        return set_string_arg (&conf->statpage, line, &match[2]);
}

static HANDLE_FUNC (handle_stathost)
{
        int r = set_string_arg (&conf->stathost, line, &match[2]);

        if (r)
                return r;
        log_message (LOG_INFO, "Stathost set to \"%s\"", conf->stathost);
        return 0;
}

static HANDLE_FUNC (handle_xtinyproxy)
{
#ifdef XTINYPROXY_ENABLE
        return set_bool_arg (&conf->add_xtinyproxy, line, &match[2]);
#else
        fprintf (stderr,
                 "XTinyproxy NOT Enabled! Recompile with --enable-xtinyproxy\n");
        return 1;
#endif
}

static HANDLE_FUNC (handle_syslog)
{
#ifdef HAVE_SYSLOG_H
        return set_bool_arg (&conf->syslog, line, &match[2]);
#else
        fprintf (stderr, "Syslog support not compiled in executable.\n");
        return 1;
#endif
}

static HANDLE_FUNC (handle_bindsame)
{
        int r = set_bool_arg (&conf->bindsame, line, &match[2]);

        if (r)
                return r;
        log_message (LOG_INFO, "Binding outgoing connection to incoming IP");
        return 0;
}

static HANDLE_FUNC (handle_port)
{
        set_int_arg (&conf->port, line, &match[2]);

        if (conf->port > 65535) {
                fprintf (stderr, "Bad port number (%d) supplied for Port.\n",
                         conf->port);
                return 1;
        }

        return 0;
}

static HANDLE_FUNC (handle_maxclients)
{
        child_configure (CHILD_MAXCLIENTS, get_long_arg (line, &match[2]));
        return 0;
}

static HANDLE_FUNC (handle_maxspareservers)
{
        child_configure (CHILD_MAXSPARESERVERS,
                         get_long_arg (line, &match[2]));
        return 0;
}

static HANDLE_FUNC (handle_minspareservers)
{
        child_configure (CHILD_MINSPARESERVERS,
                         get_long_arg (line, &match[2]));
        return 0;
}

static HANDLE_FUNC (handle_startservers)
{
        child_configure (CHILD_STARTSERVERS, get_long_arg (line, &match[2]));
        return 0;
}

static HANDLE_FUNC (handle_maxrequestsperchild)
{
        child_configure (CHILD_MAXREQUESTSPERCHILD,
                         get_long_arg (line, &match[2]));
        return 0;
}

static HANDLE_FUNC (handle_timeout)
{
        return set_int_arg (&conf->idletimeout, line, &match[2]);
}

static HANDLE_FUNC (handle_connectport)
{
        add_connect_port_allowed (get_long_arg (line, &match[2]),
                                  &conf->connect_ports);
        return 0;
}

static HANDLE_FUNC (handle_user)
{
        return set_string_arg (&conf->user, line, &match[2]);
}

static HANDLE_FUNC (handle_group)
{
        return set_string_arg (&conf->group, line, &match[2]);
}

static HANDLE_FUNC (handle_allow)
{
        char *arg = get_string_arg (line, &match[2]);

        insert_acl (arg, ACL_ALLOW, &conf->access_list);
        safefree (arg);
        return 0;
}

static HANDLE_FUNC (handle_deny)
{
        char *arg = get_string_arg (line, &match[2]);

        insert_acl (arg, ACL_DENY, &conf->access_list);
        safefree (arg);
        return 0;
}

static HANDLE_FUNC (handle_bind)
{
#ifndef TRANSPARENT_PROXY
        int r = set_string_arg (&conf->bind_address, line, &match[2]);

        if (r)
                return r;
        log_message (LOG_INFO,
                     "Outgoing connections bound to IP %s", conf->bind_address);
        return 0;
#else
        fprintf (stderr,
                 "\"Bind\" cannot be used with transparent support enabled.\n");
        return 1;
#endif
}

static HANDLE_FUNC (handle_listen)
{
        int r = set_string_arg (&conf->ipAddr, line, &match[2]);

        if (r)
                return r;
        log_message (LOG_INFO, "Listening on IP %s", conf->ipAddr);
        return 0;
}

static HANDLE_FUNC (handle_errorfile)
{
        /*
         * Because an integer is defined as ((0x)?[[:digit:]]+) _two_
         * match places are used.  match[2] matches the full digit
         * string, while match[3] matches only the "0x" part if
         * present.  This is why the "string" is located at
         * match[4] (rather than the more intuitive match[3].
         */
        unsigned long int err = get_long_arg (line, &match[2]);
        char *page = get_string_arg (line, &match[4]);

        add_new_errorpage (page, err);
        safefree (page);
        return 0;
}

static HANDLE_FUNC (handle_addheader)
{
        char *name = get_string_arg (line, &match[2]);
        char *value = get_string_arg (line, &match[3]);
        http_header_t *header;

        if (!conf->add_headers) {
                conf->add_headers = vector_create ();
        }

        header = (http_header_t *) safemalloc (sizeof (http_header_t));
        header->name = name;
        header->value = value;

        vector_prepend (conf->add_headers, header, sizeof *header);

        safefree (header);

        /* Don't free name or value here, as they are referenced in the
         * struct inserted into the vector. */

        return 0;
}

/*
 * Log level's strings.
 */
struct log_levels_s {
        const char *string;
        int level;
};
static struct log_levels_s log_levels[] = {
        {"critical", LOG_CRIT},
        {"error", LOG_ERR},
        {"warning", LOG_WARNING},
        {"notice", LOG_NOTICE},
        {"connect", LOG_CONN},
        {"info", LOG_INFO}
};

static HANDLE_FUNC (handle_loglevel)
{
        static const unsigned int nlevels =
            sizeof (log_levels) / sizeof (log_levels[0]);
        unsigned int i;

        char *arg = get_string_arg (line, &match[2]);

        for (i = 0; i != nlevels; ++i) {
                if (!strcasecmp (arg, log_levels[i].string)) {
                        set_log_level (log_levels[i].level);
                        safefree (arg);
                        return 0;
                }
        }

        safefree (arg);
        return -1;
}

#ifdef FILTER_ENABLE
static HANDLE_FUNC (handle_filter)
{
        return set_string_arg (&conf->filter, line, &match[2]);
}

static HANDLE_FUNC (handle_filterurls)
{
        return set_bool_arg (&conf->filter_url, line, &match[2]);
}

static HANDLE_FUNC (handle_filterextended)
{
        return set_bool_arg (&conf->filter_extended, line, &match[2]);
}

static HANDLE_FUNC (handle_filterdefaultdeny)
{
        assert (match[2].rm_so != -1);

        if (get_bool_arg (line, &match[2]))
                filter_set_default_policy (FILTER_DEFAULT_DENY);
        return 0;
}

static HANDLE_FUNC (handle_filtercasesensitive)
{
        return set_bool_arg (&conf->filter_casesensitive, line, &match[2]);
}
#endif

#ifdef REVERSE_SUPPORT
static HANDLE_FUNC (handle_reverseonly)
{
        return set_bool_arg (&conf->reverseonly, line, &match[2]);
}

static HANDLE_FUNC (handle_reversemagic)
{
        return set_bool_arg (&conf->reversemagic, line, &match[2]);
}

static HANDLE_FUNC (handle_reversebaseurl)
{
        return set_string_arg (&conf->reversebaseurl, line, &match[2]);
}

static HANDLE_FUNC (handle_reversepath)
{
        /*
         * The second string argument is optional.
         */
        char *arg1, *arg2;

        arg1 = get_string_arg (line, &match[2]);
        if (!arg1)
                return -1;

        if (match[4].rm_so != -1) {
                arg2 = get_string_arg (line, &match[4]);
                if (!arg2) {
                        safefree (arg1);
                        return -1;
                }
                reversepath_add (arg1, arg2, &conf->reversepath_list);
                safefree (arg1);
                safefree (arg2);
        } else {
                reversepath_add (NULL, arg1, &conf->reversepath_list);
                safefree (arg1);
        }
        return 0;
}
#endif

#ifdef UPSTREAM_SUPPORT
static HANDLE_FUNC (handle_upstream)
{
        char *ip;
        int port;
        char *domain;

        ip = get_string_arg (line, &match[2]);
        if (!ip)
                return -1;
        port = (int) get_long_arg (line, &match[7]);

        if (match[10].rm_so != -1) {
                domain = get_string_arg (line, &match[10]);
                if (domain) {
                        upstream_add (ip, port, domain, &conf->upstream_list);
                        safefree (domain);
                }
        } else {
                upstream_add (ip, port, NULL, &conf->upstream_list);
        }

        safefree (ip);

        return 0;
}

static HANDLE_FUNC (handle_upstream_no)
{
        char *domain;

        domain = get_string_arg (line, &match[2]);
        if (!domain)
                return -1;

        upstream_add (NULL, 0, domain, &conf->upstream_list);
        safefree (domain);

        return 0;
}
#endif
