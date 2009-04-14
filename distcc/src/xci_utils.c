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
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "xci_utils.h"

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
char *dcc_xci_read_whole_file(FILE *file, size_t *len) {
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
char *dcc_xci_run_command(const char *command_line) {
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
char *dcc_xci_xcodeselect_path(void) {
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

/* dcc_xci_mask_developer_dir() & dcc_xci_unmask_developer_dir()
 *
 * We replace the system's Xcode developer dir with a token that could work as
 * a path, the receiving side then looks for the token and puts that system's
 * Xcode developer dir back in, and the build can handle different install
 * locations for Xcode developer tools.
 */

/* We use a token for the below that is valid path characters in case it ever
 * gets sent to a build without Xcode integration support. */
static const char xci_dev_dir_token[] = "/_^_XCODE_DEV_DIRECTORY_^_";

char *dcc_xci_mask_developer_dir(const char *path) {
    const char *xci_dev_dir = dcc_xci_xcodeselect_path();
    /* dcc_replace_substring will return NULL if it gets any NULL arg */
    char *result = dcc_replace_substring(path, xci_dev_dir, xci_dev_dir_token);
    if (!result) {
      rs_log_error("failed to create new string for developer dir processing "
                   "\"%s\".", path);
    }
    return result;
}

char *dcc_xci_unmask_developer_dir(const char *path) {
    const char *xci_dev_dir = dcc_xci_xcodeselect_path();
    /* dcc_replace_substring will return NULL if it gets any NULL arg */
    char *result = dcc_replace_substring(path, xci_dev_dir_token, xci_dev_dir);
    if (!result) {
        rs_log_error("failed to create new string for developer dir processing "
                     "\"%s\".", path);
    }
    return result;
}

#endif /* XCODE_INTEGRATION */
