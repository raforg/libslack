/*
* libslack - http://libslack.org/
*
* Copyright (C) 2001 raf <raf@raf.org>
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
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
* or visit http://www.gnu.org/copyleft/gpl.html
*
* 20020916 raf <raf@raf.org>
*/

#ifndef LIBSLACK_SNPRINTF_H
#define LIBSLACK_SNPRINTF_H

#include <stdlib.h>
#include <stdarg.h>

#include <slack/hdr.h>

_begin_decls
int snprintf _args ((char *str, size_t size, const char *format, ...));
int vsnprintf _args ((char *str, size_t size, const char *format, va_list args));
_end_decls

#endif

/* vi:set ts=4 sw=4: */
