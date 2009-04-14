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

#ifndef DISTCC_XCI_UTILS_H_
#define DISTCC_XCI_UTILS_H_

#ifdef XCODE_INTEGRATION

/* xci_utils.c */
char *dcc_xci_read_whole_file(FILE *file, size_t *len);
char *dcc_xci_run_command(const char *command_line);
char *dcc_xci_xcodeselect_path(void);
char *dcc_xci_mask_developer_dir(const char *path);
char *dcc_xci_unmask_developer_dir(const char *path);

#endif /* XCODE_INTEGRATION */

#endif /* DISTCC_XCI_UTILS_H_ */
