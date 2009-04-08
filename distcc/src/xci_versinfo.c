/* -*- c-file-style: "java"; indent-tabs-mode: nil; tab-width: 4; fill-column: 7
8 -*-
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

#ifdef XCODE_INTEGRATION

#include <config.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "xci_versinfo.h"

typedef struct _dcc_xci_compiler_info {
    char *raw_path;
    char *absolute_path;
    char *version;
    struct _dcc_xci_compiler_info *next;
} dcc_xci_compiler_info;

/**
 * Reads the entire contents of |file| to end-of-file and returns it in a
 * properly-sized buffer allocated by malloc().  It is the caller's
 * responsibility to free this buffer with free().  The returned string will
 * be NUL-terminated.  Note that if |file| contains embedded NUL characters,
 * the returned string will contain those embedded characters in addition
 * to the final NUL terminator.
 *
 * If |len| is non-NULL, it will be set to the length of the file read,
 * excluding the terminating NUL character.
 *
 * On failure, returns NULL.
 **/
static char *dcc_xci_read_whole_file(FILE *file, size_t *len) {
    char *output = NULL, *new_output = NULL;
    const int max_buffer_chunk = 10240;
    int buffer_size = 128, pos = 0;

    output = malloc(buffer_size);
    if (!output) {
        rs_log_error("malloc(%d) failed: %s", buffer_size, strerror(errno));
        goto out_error;
    }

    while (!feof(file)) {
        if (pos == buffer_size - 1) {
            /* Double the buffer up to a point, but never increase it by more
             * than max_buffer_chunk. */
            if (buffer_size < max_buffer_chunk)
                buffer_size *= 2;
            else
                buffer_size += max_buffer_chunk;

            new_output = realloc(output, buffer_size);
            if (!new_output) {
                rs_log_error("realloc(%d) failed: %s",
                             buffer_size, strerror(errno));
                goto out_error;
            }
            output = new_output;
        }

        pos += fread(&output[pos], 1, buffer_size - pos - 1, file);
    }

    output[pos] = '\0';

    /* Trim the buffer down to size. */
    if (pos + 1 < buffer_size) {
        buffer_size = pos + 1;
        new_output = realloc(output, buffer_size);
        if (!new_output) {
            rs_log_error("realloc(%d) failed: %s",
                         buffer_size, strerror(errno));
            goto out_error;
        }
        output = new_output;
    }

    if (len)
        *len = pos;

    return output;

  out_error:
    if (output)
        free(output);
    return NULL;
}

/**
 * Execute |command_line| via popen.  On success, returns output from the
 * command's stdout, which must be freed with free().  On failure, returns
 * NULL.
 **/
static char *dcc_xci_run_command(const char *command_line) {
    FILE *p = NULL;
    char *output = NULL;
    int rv;

    p = popen(command_line, "r");
    if (!p) {
        rs_log_error("popen(\"%s\", \"r\") failed", command_line);
        goto out_error;
    }

    output = dcc_xci_read_whole_file(p, NULL);
    if (!output) {
        rs_log_error("dcc_xci_read_whole_file failed for \"%s\"", command_line);
        goto out_error;
    }

    if ((rv = pclose(p))) {
        rs_log_error("pclose returned %d for \"%s\"", rv, command_line);
        p = NULL;
        goto out_error;
    }

    return output;

  out_error:
    if (output)
        free(output);
    if (p)
        pclose(p);
    return NULL;
}

/**
 * Returns the active Xcode Tools installation location as chosen and reported
 * by xcode-select.  This runs "/usr/bin/xcode-select -print-path" to collect
 * data.  On systems where xcode-select is not available, such as non-Macs
 * and Macs running Xcode installations prior to 2.5, this just returns
 * "/Developer".
 *
 * The returned string must be disposed of with free().
 *
 * On failure, returns NULL.
 **/
static char *dcc_xci_xcodeselect_path(void) {
    static const char default_path[] = "/Developer";
    static int has_xcodeselect_path = 0;
    static char *xcodeselect_path = NULL;
    char *output = NULL;
    struct stat statbuf;

    if (!has_xcodeselect_path) {
        has_xcodeselect_path = 1;
        if (stat("/usr/bin/xcode-select", &statbuf) != 0) {
            rs_log_info("no /usr/bin/xcode-select, using \"%s\"",
                        default_path);
            output = strdup(default_path);
            if (!output) {
                rs_log_error("strdup(\"%s\") failed: %s",
                             default_path, strerror(errno));
                goto out_error;
            }
        } else {
            output = dcc_xci_run_command("/usr/bin/xcode-select -print-path");
            if (!output)
                goto out_error;

            /* There must be some output. */
            if (output[0] == '\0' || output[0] == '\n') {
                rs_log_error("no output from xcode-select");
                goto out_error;
            }

            /* The output must be a single line. */
            char *newline = strchr(output + 1, '\n');
            if (!newline || newline[1] != '\0') {
                rs_log_error("malformed output from xcode-select");
                goto out_error;
            }

            /* Remove the newline. */
            *newline = '\0';
        }

        /* Leak the allocated string to a static to avoid having to run
         * xcode-select each time through. */
        xcodeselect_path = output;
    }

    /* !xcodeselect_path can be true here when the initial attempt failed. */
    if (!xcodeselect_path)
        goto out_error;

    /* Make another copy for caller-owns semantics. */
    output = strdup(xcodeselect_path);
    if (!output) {
        rs_log_error("strdup(\"%s\") failed: %s",
                     xcodeselect_path, strerror(errno));
    }
    return output;

  out_error:
    if (output)
        free(output);
    return NULL;
}

/**
 * Returns the appropriate prefix directory, which is the directory that
 * was chosen as --prefix at configure time.  The default is "/usr/local".
 * If the USE_XCODE_SELECT_PATH environment variable  is set to 1, the prefix
 * directory is within the xcode-select path.  For distcc configured with
 * --prefix=/usr and USE_XCODE_SELECT_PATH set, this might return
 * "/Developer/usr".
 *
 * The returned value must be disposed of with free().
 *
 * On failure, returns NULL.
 **/
static char *dcc_xci_selected_prefix(void) {
    char *xcodeselect_path = NULL;
    char *selected_prefix = NULL;
    int len = 0;

    if (dcc_getenv_bool("USE_XCODE_SELECT_PATH", 0)) {
        xcodeselect_path = dcc_xci_xcodeselect_path();
        if (!xcodeselect_path)
            goto out_error;
        len = strlen(xcodeselect_path);
    }

    selected_prefix = realloc(xcodeselect_path, len + sizeof(PREFIXDIR));
    if (!selected_prefix) {
        rs_log_error("realloc() failed: %s", strerror(errno));
        goto out_error;
    }
    /* xcodeselect_path is invalid now.  selected_prefix is now the pointer
     * to that reallocated buffer. */
    xcodeselect_path = NULL;

    strncpy(selected_prefix + len, PREFIXDIR, sizeof(PREFIXDIR));

    return selected_prefix;

  out_error:
    if (xcodeselect_path)
        free(xcodeselect_path);
    return NULL;
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
static dcc_xci_compiler_info *dcc_xci_parse_distcc_compilers(void) {
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

            /* The rest of the line up to the newline is the version string. */
            version += sizeof(version_pattern) - 1;
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
char **dcc_xci_get_all_compiler_versions(void) {
    dcc_xci_compiler_info *compilers;
    dcc_xci_compiler_info *ci;
    int count, i, j;
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

#endif /* XCODE_INTEGRATION */
