/* -*- c-file-style: "java"; indent-tabs-mode: nil; fill-column: 78; -*-
 * 
 * distcc -- A simple distributed compiler system
 *
 * Copyright (C) 2002, 2003 by Martin Pool <mbp@samba.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


                /* Power is nothing without control
                 *      -- Pirelli tyre advertisment. */


/**
 * @file
 *
 * @brief Manage lockfiles.
 *
 * distcc uses a simple disk-based lockfile system to keep track of how many
 * jobs are queued on various machines.  These locks might be used for
 * something else in the future.
 *
 * We use locks rather than e.g. a database or a central daemon because we
 * want to make sure that the lock will be removed if the client terminates
 * unexpectedly.  
 *
 * The files themselves (as opposed to the lock on them) are never cleaned up;
 * since locking & creation is nonatomic I can't think of a clean way to do
 * it.  There shouldn't be many of them, and dead ones will be caught by the
 * tmpreaper.  In any case they're zero bytes.
 *
 * Sys V semaphores might work well here, but the interface is a bit ugly and
 * they are probably not portable to Cygwin.  In particular they can leak if
 * the process is abruptly terminated, which is likely to happen to distcc.
 */


#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/file.h>

#include "distcc.h"
#include "trace.h"
#include "util.h"
#include "hosts.h"
#include "lock.h"
#include "exitcode.h"
#include "snprintf.h"


struct dcc_hostdef _dcc_local = {
    DCC_MODE_LOCAL,
    NULL,
    (char *) "localhost",
    0,
    NULL,
    4,                          /* number of tasks */
    (char *)"localhost",        /* verbatim string */
    NULL,
    NULL,
    DCC_VER_1,                  /* protocol (ignored) */
    DCC_COMPRESS_NONE,          /* compression (ignored) */
    NULL    
};

struct dcc_hostdef *dcc_hostdef_local = &_dcc_local;

static int cpp_lock;
static char cpp_lock_filename[MAXPATHLEN];

/**
 * Returns a newly allocated buffer.
 **/
int dcc_make_lock_filename(const char *lockname,
                           const struct dcc_hostdef *host,
                           int iter,
                           char **filename_ret)
{
    char * buf;
    int ret;
    char *lockdir;

    if ((ret = dcc_get_lock_dir(&lockdir)))
        return ret;

    if (host->mode == DCC_MODE_LOCAL) {
        if (asprintf(&buf, "%s/%s_localhost_%d", lockdir, lockname, 
                     iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else if (host->mode == DCC_MODE_TCP) {
        if (asprintf(&buf, "%s/%s_tcp_%s_%d_%d", lockdir, lockname,
                     host->hostname,
                     host->port, iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else if (host->mode == DCC_MODE_SSH) {
        if (asprintf(&buf, "%s/%s_ssh_%s_%d", lockdir, lockname,
                     host->hostname, iter) == -1)
            return EXIT_OUT_OF_MEMORY;
    } else {
        rs_log_crit("oops");
        return EXIT_PROTOCOL_ERROR;
    }

    *filename_ret = buf;
    return 0;
}


/**
 * Get an exclusive, non-blocking lock on a file using whatever method
 * is available on this system.
 *
 * @retval 0 if we got the lock
 * @retval -1 with errno set if the file is already locked.
 **/
int sys_lock(int fd, int block)
{
#if defined(HAVE_FLOCK)
    return flock(fd, LOCK_EX | (block ? 0 : LOCK_NB));
#elif defined(F_SETLK)
    struct flock lockparam;

    lockparam.l_type = F_WRLCK;
    lockparam.l_whence = SEEK_SET;
    lockparam.l_start = 0;
    lockparam.l_len = 0;        /* whole file */
    
    return fcntl(fd, block ? F_SETLKW : F_SETLK, &lockparam);
#elif defined(HAVE_LOCKF)
    return lockf(fd, block ? F_LOCK : F_TLOCK, 0);
#else
#  error "No supported lock method.  Please port this code."
#endif
}



int dcc_unlock(int lock_fd)
{
    rs_trace("release lock fd%d", lock_fd);
    /* All our current locks can just be closed */
    if (close(lock_fd)) {
        rs_log_error("close failed: %s", strerror(errno));
        return EXIT_IO_ERROR;
    }
    return 0;
}


/**
 * Open a lockfile, creating if it does not exist.
 **/
int dcc_open_lockfile(const char *fname, int *plockfd)
{
    /* Create if it doesn't exist.  We don't actually do anything with
     * the file except lock it.
     *
     * The file is created with the loosest permissions allowed by the user's
     * umask, to give the best chance of avoiding problems if they should
     * happen to use a shared lock dir. */
    /* FIXME: If we fail to open with EPERM or something similar, try deleting
     * the file and try again.  That might fix problems with root-owned files
     * in user home directories. */
    *plockfd = open(fname, O_WRONLY|O_CREAT, 0666);
    if (*plockfd == -1 && errno != EEXIST) {
        rs_log_error("failed to creat %s: %s", fname, strerror(errno));
        return EXIT_IO_ERROR;
    }
    dcc_set_owner(fname);
    
    return 0;
}


/**
 * Lock a server slot, in either blocking or nonblocking mode.
 *
 * In blocking mode, this function will not return until either the lock has
 * been acquired, or an error occured.  In nonblocking mode, it will instead
 * return EXIT_BUSY if some other process has this slot locked.
 *
 * @param slot 0-based index of available slots on this host.
 * @param block True for blocking mode.
 *
 * @param lock_fd On return, contains the lock file descriptor to allow
 * it to be closed.
 **/
int dcc_lock_host(const char *lockname,
                  const struct dcc_hostdef *host,
                  int slot, int block,
                  int *lock_fd)
{
    char *fname;
    int ret;

    if ((ret = dcc_make_lock_filename(lockname, host, slot, &fname)))
        return ret;

    if ((ret = dcc_open_lockfile(fname, lock_fd)) != 0) {
        free(fname);
        return ret;
    }        

    if (sys_lock(*lock_fd, block) == 0) {
        rs_trace("got %s lock on %s slot %d as fd%d", lockname,
                 host->hostdef_string, slot, *lock_fd);
        free(fname);
        return 0;
    } else {
        switch (errno) {
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EAGAIN:
        case EACCES: /* HP-UX and Cygwin give this for exclusion */
            rs_trace("%s is busy", fname);
            ret = EXIT_BUSY;
            break;
        default:
            rs_log_error("lock %s failed: %s", fname, strerror(errno));
            ret = EXIT_IO_ERROR;
            break;
        }

        dcc_close(*lock_fd);
        free(fname);
        return ret;
    }
}

int dcc_get_cpp_lock()
{
    int lock_fd;
    char *lockdir;
    int i, ncpus, sleepTime = 10000;
    if (dcc_get_lock_dir(&lockdir))
        return -1;
    if (dcc_ncpus(&ncpus))
        ncpus = 1;
    ncpus++;
    for (i=0; i<ncpus; i++) {
        sprintf(cpp_lock_filename, "%s/%s_%d", lockdir, "cpp_lock", i);
        if (dcc_open_lockfile(cpp_lock_filename, &lock_fd))
            lock_fd = -1;
        else {
            if (sys_lock(lock_fd, 0) != 0) {
                rs_trace("someone already has cpp lock: %s (%s)", cpp_lock_filename, strerror(errno));
                close(lock_fd);
                lock_fd = -1;
            } else {
                break;
            }
        }
    }
    if (lock_fd == -1) {
        srandom(getpid());
        sprintf(cpp_lock_filename, "%s/%s_%d", lockdir, "cpp_lock", random()%(ncpus));
        rs_trace("blocking for cpp lock: %s (%s)", cpp_lock_filename, strerror(errno));
        if (dcc_open_lockfile(cpp_lock_filename, &lock_fd))
            lock_fd = -1;
        else {
            if (sys_lock(lock_fd, 1) != 0) {
                rs_log_warning("failed to get cpp lock: %s (%s)", cpp_lock_filename, strerror(errno));
                close(lock_fd);
                lock_fd = -1;
            }
        }
    }
    if (lock_fd != -1)
        rs_trace("got cpp lock: %s", cpp_lock_filename);
    cpp_lock = lock_fd;
    return lock_fd;
}

void dcc_unlock_cpp_lock()
{
    if (cpp_lock != -1) {
        close(cpp_lock);
        rs_trace("gave up cpp lock: %s", cpp_lock_filename);
    }
}
