/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 78 -*-
 *
 * distcc -- A simple distributed compiler system
 *
 * Copyright 2005-2008 Apple Computer, Inc.
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

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fnmatch.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>

#include "daemon.h"
#include "distcc.h"
#include "dopt.h"
#include "trace.h"
#include "util.h"
#include "xci_utils.h"
#include "xci_versinfo.h"

typedef struct _dcc_xci_compiler_info {
    char *raw_path;
    char *absolute_path;
    char *version;
    struct _dcc_xci_compiler_info *next;
} dcc_xci_compiler_info;

/**
 * Return the system version.  Three things can happen here.  If a system
 * version string override is set, it will be returned.  If the sw_vers
 * program is found, as it should be on Macs, this will run sw_vers to obtain
 * the product and build versions.  In other cases, the operating system name
 * and version will be obtained from uname().  This information, along with
 * the CPU type also obtained from uname(), will be returned.
 *
 * Example return values:
 *   "10.5.6 (9G55, i386)"
 *   "Linux 2.6.28-11-generic (x86_64)"
 *
 * This function returns a string from static storage.  Callers must not free
 * the returned string.
 *
 * Returns NULL on failure.
 **/
static const char *dcc_xci_get_system_version(void) {
    static const char product_line_match[] = "ProductVersion:";
    static const char build_line_match[] = "BuildVersion:";
    struct utsname info;
    struct stat statbuf;
    char *sw_vers = NULL;
    char *product_line, *build_line, *product, *build, *end;
    int len;
    static int has_system_version = 0;
    static char *system_version = NULL;

    /* Use a command-line override if set. */
    if (arg_system_version)
        return arg_system_version;

    if (!has_system_version) {
        has_system_version = 1;

        if (uname(&info)) {
            rs_log_error("uname() failed: %s", strerror(errno));
            goto out_error;
        }

#ifdef __APPLE__
        /* PowerPC Mac OS X reports "Power Macintosh" for uname -m, but
         * historically Apple distcc uses "ppc" as the architecture name. */
        if (!strncmp(info.machine, "Power Macintosh", sizeof(info.machine))) {
            strncpy(info.machine, "ppc", sizeof(info.machine));
            info.machine[sizeof(info.machine) - 1] = '\0';
        }
#endif /* __APPLE__ */

        if (stat("/usr/bin/sw_vers", &statbuf) == 0) {
            /* sw_vers is available.  This is either a real Mac or something
             * doing a reasonable impersonation.  Use the numbers reported
             * by sw_vers. */
            if (!(sw_vers = dcc_xci_run_command("/usr/bin/sw_vers")))
                goto out_error;

            /* Find the product and build versions. */
            product_line = strstr(sw_vers, product_line_match);
            build_line = strstr(sw_vers, build_line_match);

            /* Make sure that the product and build versions are present and
             * that they're at the beginnings of their lines. */
            if (!product_line || !build_line ||
                !(product_line == sw_vers || *(product_line - 1) == '\n') ||
                !(build_line == sw_vers || *(build_line - 1) == '\n')) {
                rs_log_error("malformed output from sw_vers");
                goto out_error;
            }

            /* Skip spaces to find the beginning of the product and build
             * version strings. */
            product = product_line + sizeof(product_line_match);
            while (*product && *product != '\n' && isspace(*product))
                ++product;

	    build = build_line + sizeof(build_line_match);
            while (*build && *build != '\n' && isspace(*build))
                ++build;

            if (!*product || *product == '\n' || !*build || *build == '\n') {
                rs_log_error("malformed output from sw_vers");
                goto out_error;
            }

            /* NUL-terminate the product and build strings. */
            end = strchr(product, '\n');
            if (!end) {
                rs_log_error("malformed output from sw_vers");
                goto out_error;
            }
            *end = '\0';

            end = strchr(build, '\n');
            if (!end) {
                rs_log_error("malformed output from sw_vers");
                goto out_error;
            }
            *end = '\0';

            /* Format as "product (build, info.machine)".  Leave room for
             * two spaces, two parentheses, a comma, and a NUL terminator. */
            len = strlen(product) + strlen(build) + strlen(info.machine) + 6;

            if (!(system_version = malloc(len))) {
                rs_log_error("malloc() failed: %s", strerror(errno));
                goto out_error;
            }

            snprintf(system_version, len, "%s (%s, %s)",
                     product, build, info.machine);

            free(sw_vers);
            sw_vers = NULL;
        } else {
            /* sw_vers is not available.  This must not be a Mac.  Report the
             * version using uname -srm.  Format as "sysname release (machine)".
             * Leave room for the two spaces, two parentheses, and a NUL
             * terminator. */
            len = strlen(info.sysname) + strlen(info.release) +
                  strlen(info.machine) + 5;

            if (!(system_version = malloc(len))) {
                rs_log_error("malloc() failed: %s", strerror(errno));
                goto out_error;
            }

            snprintf(system_version, len, "%s %s (%s)",
                     info.sysname, info.release, info.machine);
        }
    }

    return system_version;

  out_error:
    if (sw_vers)
        free(sw_vers);
    return NULL;
}

/**
 * Returns the appropriate prefix directory within the Xcode developer
 * directory.  The Xcode developer dir is collected from xcode-select.  The
 * prefix directory is what was chosen as --prefix at configure time.  The
 * default is "/usr/local".  For distcc configured with
 * --prefix=/usr and Xcode developer tools installed in the default
 * location of "/Developer", this will return "/Developer/usr".
 *
 * The returned value must be disposed of with free().
 *
 * On failure, returns NULL.
 **/
static char *dcc_xci_selected_prefix(void) {
    const char *xcodeselect_path;
    char *selected_prefix = NULL;
    int size;

    xcodeselect_path = dcc_xci_xcodeselect_path();
    if (!xcodeselect_path) {
        rs_log_error("failed to get xcode-select path");
        return NULL;
    }

    size = strlen(xcodeselect_path) + sizeof(PREFIXDIR);
    selected_prefix = malloc(size);
    if (!selected_prefix) {
        rs_log_error("malloc() failed: %s", strerror(errno));
        return NULL;
    }

    snprintf(selected_prefix, size, "%s%s", xcodeselect_path, PREFIXDIR);

    return selected_prefix;
}

/**
 * Returns an absolute path corresponding to |path|.  If |path| is already
 * absolute, a new copy of |path| is returned without modification.  If |path|
 * is a relative path, it is appended to the prefix obtained by calling
 * dcc_xci_selected_prefix.
 *
 * The returned value must be disposed of with free().
 *
 * On failure, returns NULL.
 **/
static char *dcc_xci_path_in_prefix(const char *path) {
    char *prefix = NULL, *prefix_path = NULL;
    size_t prefix_len, path_len;

    /* If the path is absolute, return a copy as-is. */
    if (path[0] == '/') {
        char *rv = strdup(path);
        if (!rv)
            rs_log_error("strdup(\"%s\") failed: %s", path, strerror(errno));
        return rv;
    }

    /* |path| is relative.  Treat it as relative to dcc_xci_selected_prefix
     * and return the correct absolute path. */
    prefix = dcc_xci_selected_prefix();
    if (!prefix)
        goto out_error;

    /* Allocate enough space for the / separator and NUL terminator. */
    prefix_len = strlen(prefix);
    path_len = strlen(path);
    prefix_path = realloc(prefix, prefix_len + path_len + 2);
    if (!prefix_path)
        goto out_error;
    prefix = NULL;

    /* Put in the / separator and then append |path| and a NUL terminator. */
    prefix_path[prefix_len] = '/';
    strncpy(prefix_path + prefix_len + 1, path, path_len + 1);

    return prefix_path;

  out_error:
    if (prefix)
        free(prefix);
    return NULL;
}

/**
 * Returns a linked list of dcc_xci_compiler_info structs describing the
 * various compilers available according to the distcc_compilers file.
 * distcc_compilers is located in the directory identified by
 * dcc_xci_selected_prefix, subdirectory "share".  Each line in that file
 * identifies a compiler by absolute or relative path.  Relative paths are
 * taken as relative to dcc_xci_selected_prefix.  This function populates
 * the |raw_path|, |absolute_path|, |version|, and |next| members of the
 * dcc_xci_compiler_info structs.
 *
 * The data returned is obtained only once during the duration of execution.
 * It is accessed through static storage on subsequent calls.  Callers must
 * not free anything obtained through this interface.
 *
 * On failure, returns NULL.
 **/
static const dcc_xci_compiler_info *dcc_xci_parse_distcc_compilers(void) {
    static int parsed_compilers = 0;
    static dcc_xci_compiler_info *compilers = NULL;
    char *compilers_path = NULL;
    FILE *compilers_file = NULL;
    char *compilers_data = NULL;
    char *line, *newline;
    size_t len, pos, line_len;
    dcc_xci_compiler_info *ci = NULL, *last_ci = NULL;
    struct stat statbuf;
    char *cmd = NULL, *version_output = NULL;

    if (parsed_compilers)
        return compilers;

    parsed_compilers = 1;

    compilers_path = dcc_xci_path_in_prefix("share/distcc_compilers");
    if (!compilers_path)
        goto out_error;

    if (!(compilers_file = fopen(compilers_path, "r"))) {
        rs_log_error("fopen(\"%s\", \"r\") failed: %s",
                     compilers_path, strerror(errno));
        goto out_error;
    }

    if (!(compilers_data = dcc_xci_read_whole_file(compilers_file, &len))) {
        rs_log_error("dcc_xci_read_whole_file failed for \"%s\"",
                     compilers_path);
        goto out_error;
    }

    for (pos = 0; pos < len; pos += line_len + 1) {
        line = compilers_data + pos;

        /* Get rid of the line terminator. */
        newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
            line_len = newline - line;
        } else {
            /* The file had an embedded NUL in this line, or the file ended
             * abruptly without a newline.  Treat what's available as its own
             * line. */
            line_len = strlen(line);
        }

        /* Skip blank lines and comments. */
        if (!line_len || line[0] == '#')
            continue;

        /* Allocate and populate a dcc_xci_compiler_info record for this
         * compiler. */
        ci = calloc(1, sizeof(dcc_xci_compiler_info));
        if (!ci) {
            rs_log_error("calloc() failed: %s", strerror(errno));
            goto out_error;
        }

        /* raw_path is the path as listed in the compilers file. */
        ci->raw_path = strdup(line);
        if (!ci->raw_path) {
            rs_log_error("strdup(\"%s\") failed: %s", line, strerror(errno));
            goto out_error;
        }

        /* Determine an absolute path to the compiler. */
        if (!(ci->absolute_path = dcc_xci_path_in_prefix(ci->raw_path)))
            goto out_error;

        if (stat(ci->absolute_path, &statbuf) == 0) {
            /* The compiler exists.  Get it to report its version. */
            static const char version_args[] = " -v 2>&1";
            size_t cmd_len = strlen(ci->absolute_path) + sizeof(version_args);
            if (!(cmd = malloc(cmd_len))) {
                rs_log_error("malloc() failed: %s\n", strerror(errno));
                goto out_error;
            }
            snprintf(cmd, cmd_len, "%s%s", ci->absolute_path, version_args);
            if (!(version_output = dcc_xci_run_command(cmd)))
                goto out_error;

            /* The version is on a line by itself beginning with
             * "gcc version ".  It's never on the first line of output, so
             * it's safe to match with a leading '\n'. */
            static const char version_pattern[] = "\ngcc version ";
            char *version = strstr(version_output, version_pattern);
            if (!version) {
                rs_log_error("could not determine version for \"%s\"",
                             ci->absolute_path);
                goto out_error;
            }

            /* The entire line is the version string.  Move past the newline
             * at the beginning of version_pattern and take everything up
             * to the next newline. */
            ++version;
            char *version_end = strchr(version, '\n');
            if (!version_end) {
                rs_log_error("could not determine end of version for \"%s\"",
                             ci->absolute_path);
                goto out_error;
            }
            *version_end = '\0';

            if (!(ci->version = strdup(version))) {
                rs_log_error("strdup(\"%s\") failed: %s",
                             version, strerror(errno));
                goto out_error;
            }

            /* Insert the new compiler into the list. */
            if (!compilers)
                compilers = ci;
            else
                last_ci->next = ci;
            last_ci = ci;
            ci = NULL;

            free(cmd);
            cmd = NULL;
            free(version_output);
            version_output = NULL;
        } else {
            /* It's not an error if the compiler isn't present, just don't
             * add it to the list of compilers. */
            free(ci->absolute_path);
            free(ci->raw_path);
            free(ci);
            ci = NULL;
        }
    }

    free(compilers_data);
    compilers_data = NULL;

    if (fclose(compilers_file)) {
        rs_log_error("fclose(\"%s\") failed: %s",
                     compilers_path, strerror(errno));
        /* Don't fail, just live with it. */
    }
    compilers_file = NULL;

    free(compilers_path);
    compilers_path = NULL;

    /* Leak |compilers| to a static to avoid having to do the same file input
     * each time through. */
    return compilers;

  out_error:
    if (version_output)
        free(version_output);
    if (cmd)
        free(cmd);

    /* Free the compiler that was being worked on. */
    if (ci) {
        if (ci->version)
            free (ci->version);
        if (ci->absolute_path)
            free (ci->absolute_path);
        if (ci->raw_path)
            free (ci->raw_path);
        free(ci);
    }

    /* Free added compilers. */
    ci = compilers;
    while (ci) {
        if (ci->version)
            free (ci->version);
        if (ci->absolute_path)
            free (ci->absolute_path);
        if (ci->raw_path)
            free (ci->raw_path);
        last_ci = ci;
        ci = ci->next;
        free(last_ci);
    }
    compilers = NULL;

    if (compilers_data)
        free(compilers_data);
    if (compilers_file)
        fclose(compilers_file);
    if (compilers_path)
        free(compilers_path);

    return NULL;
}

/**
 * Returns a list of all compiler versions available.  This is done by
 * scanning the versions in the list of compilers returned by
 * dcc_xci_parse_distcc_compilers.  Duplicate version strings are removed.
 * (Duplicates occur when, for example, /usr/bin/gcc and /usr/bin/g++ both
 * appear in the compilers file and are both the same version, as they
 * typically are.)
 *
 * The returned value is a vector.  The vector itself must be disposed of
 * with free().  The individual elements of the vector must not be freed.
 * The final element of the vector will be NULL.
 *
 * On failure, returns NULL.  This does not necessarily indicate a hard error, 
 * it is possible for NULL to be returned when there are no compilers
 * available.
 **/
static char **dcc_xci_get_all_compiler_versions(void) {
    const dcc_xci_compiler_info *compilers, *ci;
    int count = 0, i, j;
    char **result = NULL;

    compilers = dcc_xci_parse_distcc_compilers();
    if (!compilers)
        return NULL;

    /* Count the maximum number of compiler versions possible. */
    for (ci = compilers; ci; ci = ci->next)
        ++count;

    result = malloc((count + 1) * sizeof(char *));
    if (!result) {
        rs_log_error("malloc() failed: %s\n", strerror(errno));
        return NULL;
    }

    for (i = 0, ci = compilers; ci; ci = ci->next) {
        for (j = 0; j < i; j++) {
            if (!strcmp(result[j], ci->version))
                break;
        }
        /* If the version wasn't found yet, add it. */
        if (j == i)
            result[i++] = ci->version;
    }

    result[i++] = NULL;

    return result;
}

/**
 * Returns an list of items in the  directory @p dir.  The matches will have
 * the prefix directory added to them.  If @p pattern is not NULL, the entries
 * must match that pattern (fnmatch).  If match_type is not zero, the d_type
 * must be of the same value.
 *
 * NOTE: The returned value from this function is compatible with the argv
 * functions, specifically, dcc_argv_len() and dcc_free_argv() since it is an
 * array of strings.
 **/
static char **dcc_xci_directory_list(const char *dir, const char *pattern,
                                     int match_type) {
    DIR *dir_scanner = NULL;
    struct dirent *entry;
    char **result = NULL;
    char **new_result;
    int count = 0;
    int dir_len, name_len;

    if (!dir) goto out_error;
    dir_len = strlen(dir);

    if (!(dir_scanner = opendir(dir))) {
        rs_log_error("opendir(\"%s\") failed: %s", dir, strerror(errno));
        goto out_error;
    }

    while ((entry = readdir(dir_scanner))) {
        if (match_type && (match_type != entry->d_type))
            continue;
        if (pattern && fnmatch(pattern, entry->d_name, 0))
            continue;

        /* If reading directories becomes important, this probably wants a
         * better allocation system, but for now, this is good. */
        new_result = realloc(result, (count + 1) * sizeof(char*));
        if (!new_result) {
            rs_log_error("realloc() failed: %s", strerror(errno));
            goto out_error;
        }
        result = new_result;
        name_len = dir_len + 2 + entry->d_namlen;
        result[count] = malloc(name_len);
        if (!result[count]) {
            rs_log_error("malloc() failed: %s", strerror(errno));
            goto out_error;
        }
        snprintf(result[count++], name_len, "%s/%s", dir, entry->d_name);
    }

    /* If it has anything, we need need to put the ending NULL */
    if (count)
        result[count] = NULL;

    if (closedir(dir_scanner)) {
        rs_log_error("closedir(\"%s\") failed: %s", dir, strerror(errno));
        /* Keep going since we got the data we needed */
    }
  
    return result;

  out_error:
    if (dir_scanner)
        closedir(dir_scanner);
    if (result) {
        while (count--) {
            if (result[count])
                free(result[count]);
        }
        free(result);
    }
    return NULL;
}

/**
 * Returns a list of all Mac OS X and iPhone SDKs available.  This is done by
 * scanning the directories under the Xcode developer directory as there
 * does not seem to be a listing of them.
 *
 * The returned value is a vector.  The vector itself and the individual
 * elements of the vector must not be freed.  The final element of the vector
 * will be NULL.
 *
 * On failure, returns NULL.  This does not necessarily indicate a hard error, 
 * it is possible for NULL to be returned when there are no SDKs available.
 **/
static const char **dcc_xci_get_all_developer_sdks(void) {
    static int scanned_sdks = 0;
    static const char **sdks = NULL;
    char **sdk_paths = NULL;
    char **new_sdk_paths;
    char **platforms = NULL;
    char **platform_sdks = NULL;
    char buf[PATH_MAX + 1];
    const char *dev_dir;
    int i, count, sdk_paths_size = 0;
    int dev_dir_len;

    if (scanned_sdks)
        return sdks;

    scanned_sdks = 1;
  
    dev_dir = dcc_xci_xcodeselect_path();
    if (!dev_dir) goto bail_out;
  
    /* As of Xcode 3.1.x, we need to look in:
     *   [XCODE_DEVELOPER]/Developer/SDKs
     *   [XCODE_DEVELOPER]/Platforms/[ANYTHING].platform/Developer/SDKs
     * for directories that end in '.sdk'. */

    snprintf(buf, sizeof(buf), "%s/SDKs", dev_dir);
    sdk_paths = dcc_xci_directory_list(buf, "*.sdk", DT_DIR);
    if (sdk_paths)
        sdk_paths_size = dcc_argv_len(sdk_paths) + 1;

    snprintf(buf, sizeof(buf), "%s/Platforms", dev_dir);
    platforms = dcc_xci_directory_list("/Developer/Platforms", "*.platform",
                                       DT_DIR);
    if (platforms) {
        for (i = 0 ; platforms[i] ; ++i) {
            snprintf(buf, sizeof(buf), "%s/Developer/SDKs", platforms[i]);
            platform_sdks = dcc_xci_directory_list(buf, "*.sdk", DT_DIR);
            if (!platform_sdks)
                continue; /* No sdks, on to the next */
            count = dcc_argv_len(platform_sdks);
            new_sdk_paths = realloc(sdk_paths,
                                    (sdk_paths_size + count) *
                                      sizeof(char*));
            if (!new_sdk_paths) {
                rs_log_error("realloc() failed: %s", strerror(errno));
                goto bail_out;
            }
            sdk_paths = new_sdk_paths;
            memcpy(sdk_paths + sdk_paths_size - 1, platform_sdks,
                   (count + 1) * sizeof(char*));
            /* We've moved the strings, just free the list itself. */
            free(platform_sdks);
            platform_sdks = NULL;
        }
    }

    /* Now take the Xcode developer dir off the front of them to get their
     * names (for comparing between machines). */
    if (sdk_paths_size) {
        sdks = malloc(sdk_paths_size * sizeof(char*));
        if (!sdks) {
            rs_log_error("malloc() failed: %s", strerror(errno));
            goto bail_out;
        }
        dev_dir_len = strlen(dev_dir) + 1; /* Remove the trailing / also */
        for (i = 0 ; sdk_paths[i] ; i++) {
            sdks[i] = strdup(sdk_paths[i] + dev_dir_len);
            if (!sdks[i]) {
                rs_log_error("strdup() failed: %s", strerror(errno));
                dcc_free_argv((char**)sdks);
                sdks = NULL;
                goto bail_out;
            }
        }
        sdks[i] = NULL;
    }

  bail_out:
    /* Normal or error cleanup */
    if (sdk_paths)
        dcc_free_argv(sdk_paths);
    if (platforms)
        dcc_free_argv(platforms);
    if (platform_sdks)
        dcc_free_argv(platform_sdks);
    return sdks;
}


/**
 * Return a string containing host information, including hardware information,
 * the OS version, and compiler versions.  Information that can't be collected
 * due to errors is omitted from the report.
 *
 * Errors that occur for reasons other than failure to collect data are
 * treated as failures and result in this function returning NULL.
 *
 * The returned string is cached in static storage and must not be freed by
 * callers.
 **/
const char *dcc_xci_host_info_string() {
    static const char sys_key[] = "SYSTEM=";
    static const char distcc_key_and_value[] = "DISTCC=" PACKAGE_VERSION;
    static const char compiler_key[] = "COMPILER=";
    static const char cpus_key[] = "CPUS=";
    static const char cpuspeed_key[] = "CPUSPEED=";
    static const char jobs_key[] = "JOBS=";
    static const char priority_key[] = "PRIORITY=";
    static const char sdk_key[] = "DEVELOPER_SDK=";
    static int has_host_info = 0;
    static char *host_info = NULL;
    int len = 0, pos = 0, ncpus;
    unsigned long long cpuspeed;
    char *info = NULL;
    const char *sys;
    char **compilers = NULL, **compiler;
    const char **sdks = NULL, **sdk;

    if (has_host_info)
        return host_info;

    has_host_info = 1;

    /* Allocate 20 bytes for each integer value, so that even if they're
     * 64-bit integers, there will be enough room to store them in decimal.
     * Using sizeof on each key includes the key's terminating NUL, so this
     * doesn't need to account separately for each line's terminating newline.
     */
    static const int int_decimal_len = 20;

    sys = dcc_xci_get_system_version();
    if (sys)
        len += sizeof(sys_key) + strlen(sys);

    len += sizeof(distcc_key_and_value);

    compilers = dcc_xci_get_all_compiler_versions();
    if (compilers) {
        for (compiler = compilers; *compiler; ++compiler)
            len += sizeof(compiler_key) + strlen(*compiler);
    }

    len += sizeof(cpus_key) + int_decimal_len;
    len += sizeof(cpuspeed_key) + int_decimal_len;

    if (dcc_max_kids)
        len += sizeof(jobs_key) + int_decimal_len;

    len += sizeof(priority_key) + int_decimal_len;

    sdks = dcc_xci_get_all_developer_sdks();
    if (sdks) {
        for (sdk = sdks; *sdk; ++sdk)
            len += sizeof(sdk_key) + strlen(*sdk);
    }
  
    /* Leave room for a NUL terminator at the end of the entire string. */
    ++len;

    info = malloc(len);
    if (!info) {
        rs_log_error("malloc(%d) failed: %s", len, strerror(errno));
        goto out_error;
    }
    info[pos] = '\0';

    if (sys) {
        pos += snprintf(info + pos, len - pos, "%s%s\n", sys_key, sys);
        if (pos >= len)
            goto out_error_info_size;
    }

    pos += snprintf(info + pos, len - pos, "%s\n", distcc_key_and_value);
    if (pos >= len)
        goto out_error_info_size;

    if (compilers) {
        for (compiler = compilers; *compiler; ++compiler) {
            pos += snprintf(info + pos, len - pos, "%s%s\n",
                            compiler_key, *compiler);
            if (pos >= len)
                goto out_error_info_size;
        }
        free(compilers);
        compilers = NULL;
    }
 
    if (dcc_ncpus(&ncpus) == 0) {
        pos += snprintf(info + pos, len - pos, "%s%d\n", cpus_key, ncpus);
        if (pos >= len)
            goto out_error_info_size;
    }

    if (dcc_cpuspeed(&cpuspeed) == 0) {
        pos += snprintf(info + pos, len - pos, "%s%llu\n", cpuspeed_key,
                        cpuspeed);
        if (pos >= len)
            goto out_error_info_size;
    }

    if (dcc_max_kids) {
        pos += snprintf(info + pos, len - pos, "%s%d\n",
                        jobs_key, dcc_max_kids);
        if (pos >= len)
            goto out_error_info_size;
    }

    pos += snprintf(info + pos, len - pos, "%s%d\n", priority_key,
                    arg_priority);
    if (pos >= len)
        goto out_error_info_size;

    if (sdks) {
        for (sdk = sdks; *sdk; ++sdk) {
            pos += snprintf(info + pos, len - pos, "%s%s\n", sdk_key, *sdk);
            if (pos >= len)
                goto out_error_info_size;
        }
    }

    /* Trim the buffer to the size actually used. */
    if (pos + 1 < len) {
        if (!(host_info = realloc(info, pos + 1))) {
            rs_log_error("realloc() failed: %s", strerror(errno));
            goto out_error;
        }
    } else {
        host_info = info;
    }
    info = NULL;

    return host_info;

  out_error_info_size:
    rs_log_error("info buffer of size %d is too small", len);

  out_error:
    if (info)
        free(info);
    if (compilers)
        free(compilers);

    return NULL;
}

#endif /* XCODE_INTEGRATION */
