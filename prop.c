/*
* libslack - http://libslack.org/
*
* Copyright (C) 1999-2001 raf <raf@raf.org>
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
* 20010215 raf <raf@raf.org>
*/

/*

=head1 NAME

I<libslack(prop)> - program properties file module

=head1 SYNOPSIS

    #include <slack/prop.h>

    const char *prop_get(const char *name);
    const char *prop_get_or(const char *name, const char *default_value);
    const char *prop_set(const char *name, const char *value);
    int prop_get_int(const char *name);
    int prop_get_int_or(const char *name, int default_value);
    int prop_set_int(const char *name, int value);
    double prop_get_double(const char *name);
    double prop_get_double_or(const char *name, double default_value);
    double prop_set_double(const char *name, double value);
    int prop_get_bool(const char *name);
    int prop_get_bool_or(const char *name, int default_value);
    int prop_set_bool(const char *name, int value);
    int prop_unset(const char *name);
    int prop_save(void);


=head1 DESCRIPTION

This module provides support for system-wide and user-specific (generic and
program-specific) properties in "well-known" locations:

    /etc/properties.app             - system-wide, generic properties
    $HOME/.properties/app           - user-defined, generic properties
    /etc/properties/app.progname    - system-wide, program-specific properties
    $HOME/.properties/app.progname  - user-defined, program-specific properties

When the client first requests, sets or unsets a property, all properties
relevant to the current program are loaded from these files in the order
given above. This order ensures that program-specific properties override
generic properties and user-defined properties override system-wide
properties. The client can change properties at runtime and save the current
properties back to disk (to the user-defined, program-specific properties
file).

Program names (as returned by I<prog_name()> are converted into file name
suffixes by replacing every occurrance of the file path separator (``/'')
with a ``-''. Properties files consist of one property per line. Each
property is specified by its name, followed by C<``=''> followed by its
value. The name must not have a C<``=''> in it unless it is quoted with a
C<``\''>. The properties files may also contain blank lines and comments
(C<``#''> until the end of the line).

=over 4

=cut

*/

#include "std.h"

#include <pwd.h>
#include <sys/stat.h>

#include "prog.h"
#include "daemon.h"
#include "map.h"
#include "err.h"
#include "lim.h"
#include "mem.h"
#include "prop.h"
#include "str.h"
#include "thread.h"

#ifdef NEEDS_SNPRINTF
#include "snprintf.h"
#endif

#ifndef isodigit
#define isodigit(c) (isdigit(c) && (c) < '8')
#endif

typedef struct Prop Prop;

struct Prop
{
	Map *map;
	Prop *defaults;
};

static struct
{
	int init;
	Prop *prop;
	char *home;
	int dirty;
	Locker *locker;
}
g = { 0, NULL, NULL, 0, NULL };

/*

C<Prop *prop_create(Map *map, Prop *defaults)>

Creates and returns a I<Prop> containing C<map> and C<defaults>. On error,
returns C<NULL>. The new I<Prop> will destroy C<map> and C<defaults> when it
is destroyed.

*/

static Prop *prop_create(Map *map, Prop *defaults)
{
	Prop *prop;

	if (!(prop = mem_new(Prop)))
		return NULL;

	prop->map = map;
	prop->defaults = defaults;

	return prop;
}

/*

C<void prop_release(Prop *prop)>

Releases (deallocates) C<prop>.

*/

static void prop_release(Prop *prop)
{
	if (!prop)
		return;

	map_release(prop->map);
	prop_release(prop->defaults);
	mem_release(prop);
}

/*

C<int key_cmp(const char **a, const char **b)>

Compares the two strings pointed to by C<a> and C<b>.

*/

static int key_cmp(const char **a, const char **b)
{
	return strcmp(*a, *b);
}

static const char *special_code = "abfnrtv";
static const char *special_char = "\a\b\f\n\r\t\v";
static const char *eq = "=";

/*

C<String *quote_special(const char *src)>

Replaces every occurrance in C<src> of special characters (represented in
I<C> by C<"\a">, C<"\b">, C<"\f">, C<"\n">, C<"\r">, C<"\t">, C<"\v">) with
their corresponding I<C> representation. Other non-printable characters are
replaced with their ASCII codes in hexadecimal (i.e. "\xhh"). The caller
must deallocate the memory returned.

*/

static String *quote_special(const char *src)
{
	return encode(src, special_char, special_code, '\\', 1);
}

/*

C<String *unquote_special(const char *src)>

Replaces every occurrance in C<src> of C<"\a">, C<"\b">, C<"\f">, C<"\n">,
C<"\r">, C<"\t"> or C<"\v"> with the corresponding special characters (as
interpreted by I<C>). Ascii codes in octal or hexadecimal (i.e. "\ooo" or
"\xhh") are replaced with their corresponding ASCII characters. The caller
must deallocate the memory returned.

*/

static String *unquote_special(const char *src)
{
	return decode(src, special_char, special_code, '\\', 1);
}

/*

C<String *quote_equals(const char *src)>

Replace every occurrance in C<src> of C<"="> with C<"\=">. The caller must
deallocate the memory returned.

*/

static String *quote_equals(const char *src)
{
	return encode(src, eq, eq, '\\', 0);
}

/*

C<String *quote_equals(const char *src)>

Replace every occurrance in C<src> of C<"\="> with C<"=">. The caller must
deallocate the memory returned.

*/

static String *unquote_equals(const char *src)
{
	return decode(src, eq, eq, '\\', 0);
}

/*

C<char *user_home(void)>

Returns the user's home directory (obtained from C</etc/passwd>). The return
value is cached so any subsequent calls are faster.

*/

static char *user_home(void)
{
	struct passwd *pwent;
	char *home = NULL;

	if (g.home)
		return g.home;

	if ((pwent = getpwuid(getuid())))
		home = pwent->pw_dir;

	return g.home = (home && strlen(home)) ? mem_strdup(home) : NULL;
}

/*

C<void prop_parse(Map *map, const char *path, char *line, size_t lineno)>

Parses a line from a properties file. C<path> if the path of the properties
file. C<line> is the text to parse. C<lineno> is the current line number.
The property parsed, if any, is added to C<map>. Emits error messages when
syntax errors occur.

*/

static void prop_parse(Map *map, const char *path, char *line, size_t lineno)
{
	String *prop, *name;
	char *p, *eq, *value, *val, *key;

	/* Unquote any special characters in the line */

	if (!(prop = unquote_special(line)))
	{
		error("prop: Out of memory");
		return;
	}

	/* Find first unquoted '=' */

	for (p = cstr(prop), eq = strchr(p, '='); eq; eq = strchr(eq + 1, '='))
		if (eq != p && eq[-1] != '\\')
			break;

	if (!eq)
	{
		error("prop: %s line %d: Expected '='\n%s", path, lineno, line);
		str_release(prop);
		return;
	}

	/* Identify and separate the name and value */

	value = eq + 1;
	while (eq > p && isspace((int)(unsigned char)eq[-1]))
		--eq;
	*eq = nul;

	/* Unquote any quoted '=' in the name */

	if (!(name = unquote_equals(p)))
	{
		error("prop: Out of memory");
		str_release(prop);
		return;
	}

	/* Add this property to the map */

	if (str_length(name) == 0)
	{
		error("prop: %s line %d: Empty name\n%s", path, lineno, line);
		str_release(prop);
		str_release(name);
		return;
	}

	key = cstr(name);

	if (!(val = mem_strdup(value)))
	{
		error("prop: Out of memory");
		str_release(prop);
		str_release(name);
		return;
	}

	if (map_add(map, key, val) == -1)
		error("prop: %s line %d: Property %s already defined\n%s", path, lineno, name, line);

	str_release(prop);
	str_release(name);
}

/*

C<Prop *prop_load(const char *path, Prop *defaults)>

Creates and returns a new I<Prop> containing C<defaults> and the properties
obtained from the properties file named C<path>. On error, returns C<NULL>.

*/

static Prop *prop_load(const char *path, Prop *defaults)
{
	Prop *prop;
	Map *map;

	if (!(map = map_create((map_release_t *)free)))
		return NULL;

	if (!daemon_parse_config(path, map, (daemon_config_parser_t *)prop_parse))
	{
		map_release(map);
		return NULL;
	}

	if (!(prop = prop_create(map, defaults)))
	{
		map_release(map);
		return NULL;
	}

	return prop;
}

/*

C<int prop_init(void)>

Initialises the I<prop> module. Loads properties from the following locations:

    /etc/properties.app             - system-wide, generic properties
    $HOME/.properties/app           - user-defined, generic properties
    /etc/properties/app.progname    - system-wide, program-specific properties
    $HOME/.properties/app.progname  - user-defined, program-specific properties

Properties from the first three files become (nesting) defaults. Properties
from the last file becomes the top level I<Prop>. If the last file doesn't
exist, an empty I<Prop> becomes the top level I<Prop>.

Called at the start of the first get, set or unset function called.

*/

static int prop_init(void)
{
	char *path;
	Prop *prop = NULL;
	Prop *prop_next;
	char *home;
	int writable = 0;
	size_t path_len;

	path_len = limit_path();

	if (!(path = mem_create(path_len, char)))
		return -1;

	/* System wide generic properties: /etc/properties/app */

	snprintf(path, path_len, "%s%cproperties%capp", ETC_DIR, PATH_SEP, PATH_SEP);
	prop_next = prop_load(path, prop);
	if (prop_next)
		prop = prop_next;

	/* User defined generic properties: $HOME/.properties/app */

	home = user_home();
	if (home)
	{
		snprintf(path, path_len, "%s%c.properties%capp", home, PATH_SEP, PATH_SEP);
		prop_next = prop_load(path, prop);
		if (prop_next)
			prop = prop_next;
	}

	if (prog_name())
	{
		char *progname, *sep;

		if (!(progname = mem_strdup(prog_name())))
		{
			mem_release(path);
			prop_release(prop);
			return -1;
		}

		for (sep = strchr(progname, PATH_SEP); sep; sep = strchr(sep, PATH_SEP))
			*sep++ = '-';

		/* System wide program specific properties: /etc/properties/app.progname */

		snprintf(path, path_len, "%s%cproperties%capp.%s", ETC_DIR, PATH_SEP, PATH_SEP, progname);
		prop_next = prop_load(path, prop);
		if (prop_next)
			prop = prop_next;

		/* User defined program specific properties: $HOME/.properties/app.progname */

		if (home)
		{
			snprintf(path, path_len, "%s%c.properties%capp.%s", home, PATH_SEP, PATH_SEP, progname);
			prop_next = prop_load(path, prop);
			if (prop_next)
			{
				prop = prop_next;
				writable = 1;
			}
		}

		free(progname);
	}

	/* Guarantee a user defined program specific property map for prop_set() */

	if (!writable)
	{
		Map *map;

		if (!(map = map_create((map_release_t *)free)))
		{
			mem_release(path);
			prop_release(prop);
			return -1;
		}

		if (!(prop_next = prop_create(map, prop)))
		{
			mem_release(path);
			prop_release(prop);
			map_release(map);
			return -1;
		}

		prop = prop_next;
	}

	mem_release(path);
	g.prop = prop;
	g.init = 1;

	return 0;
}

/*

=item C<const char *prop_get(const char *name)>

Returns the value of the property named C<name>. Returns C<NULL> if there is
no such property.

=cut

*/

const char *prop_get(const char *name)
{
	Prop *p;
	const char *value = NULL;

	if (locker_wrlock(g.locker) == -1)
		return NULL;

	if (!g.init && prop_init() == -1)
	{
		locker_unlock(g.locker);
		return NULL;
	}

	for (p = g.prop; p; p = p->defaults)
		if ((value = map_get(p->map, name)))
			break;

	if (locker_unlock(g.locker) == -1)
		return NULL;

	return value;
}

/*

=item C<const char *prop_get_or(const char *name, const char *default_value)>

Returns the value of the property named C<name>. Returns C<default_value> if
there is no such property.

=cut

*/

const char *prop_get_or(const char *name, const char *default_value)
{
	const char *prop = prop_get(name);

	return prop ? prop : default_value;
}

/*

=item C<const char *prop_set(const char *name, const char *value)>

Sets the property named C<name> to a copy of C<value>. On success, returns
the copy of C<value>. On error, returns C<NULL>. If I<prop_save()> is called
after a call to this function, the new property will be saved to disk and
will be available the next time this program is executed.

=cut

*/

const char *prop_set(const char *name, const char *value)
{
	char *val;

	if (locker_wrlock(g.locker) == -1)
		return NULL;

	if (!g.init && prop_init() == -1)
	{
		locker_unlock(g.locker);
		return NULL;
	}

	if (!(val = mem_strdup(value)))
	{
		locker_unlock(g.locker);
		return NULL;
	}

	if (map_put(g.prop->map, name, val) == -1)
	{
		mem_release(val);
		locker_unlock(g.locker);
		return NULL;
	}

	g.dirty = 1;

	if (locker_unlock(g.locker) == -1)
		return NULL;

	return val;
}

/*

=item C<int prop_get_int(const char *name)>

Returns the value of the property named C<name> as an integer. Returns C<0>
if there is no such property or if it is not interpretable as a decimal
integer.

=cut

*/

int prop_get_int(const char *name)
{
	return prop_get_int_or(name, 0);
}

/*

=item C<int prop_get_int_or(const char *name, int default_value)>

Returns the value of the property named C<name> as an integer. Returns
C<default_value> if there is no such property or of it is not interpretable
as a decimal integer.

=cut

*/

int prop_get_int_or(const char *name, int default_value)
{
	const char *prop = prop_get(name);
	int val;

	return (prop && sscanf(prop, " %d ", &val)) ? val : default_value;
}

/*

=item C<int prop_set_int(const char *name, int value)>

Sets the property named C<name> to C<value>. On success, returns C<value>.
On error, returns 0. If I<prop_save()> is called after a call to this
function, the new property will be saved to disk and will be available the
next time this program is executed.

=cut

*/

int prop_set_int(const char *name, int value)
{
	char buf[128];
	snprintf(buf, 128, "%d", value);
	return prop_set(name, buf) ? value : 0;
}

/*

=item C<double prop_get_double(const char *name)>

Returns the value of the property named C<name> as a double. Returns 0.0
if there is no such property or if it is not interpretable as a floating
point number.

=cut

*/

double prop_get_double(const char *name)
{
	return prop_get_double_or(name, 0.0);
}

/*

=item C<double prop_get_double_or(const char *name, double default_value)>

Returns the value of the property named C<name> as a double. Returns
C<default_value> if there is no such property or of it is not interpretable
as a floating point number.

=cut

*/

double prop_get_double_or(const char *name, double default_value)
{
	const char *prop = prop_get(name);
	double val;

	return (prop && sscanf(prop, "%lg", &val)) ? val : default_value;
}

/*

=item C<double prop_set_double(const char *name, double value)>

Sets the property named C<name> to C<value>. On success, returns C<value>.
On error, returns 0.0. If I<prop_save()> is called after a call to this
function, the new property will be saved to disk and will be available the
next time this program is executed.

=cut

*/

double prop_set_double(const char *name, double value)
{
	char buf[128];
	snprintf(buf, 128, "%g", value);
	return prop_set(name, buf) ? value : -1;
}

/*

=item C<int prop_get_bool(const char *name)>

Returns the value of the property named C<name> as a boolean value. Returns
C<0> if there is no such property or if it is not interpretable as a boolean
value. The values: C<"true">, C<"yes">, C<"on"> and C<"1"> are all
interpreted as C<true>. All other values are C<false>.

=cut

*/

int prop_get_bool(const char *name)
{
	return prop_get_bool_or(name, 0);
}

/*

=item C<int prop_get_bool_or(const char *name, int default_value)>

Returns the value of the property named C<name> as a boolean value. Returns
C<default_value> if there is no such property or of it is not interpretable
as a boolean value. The values: C<"true">, C<"yes">, C<"on"> and C<"1"> are
all interpreted as C<true>. The values: C<"false">, C<"no">, C<"off"> and
C<"0"> are all interpreted as C<false>. All other values are disregarded and
will cause C<default_value> to be returned.

=cut

*/

int prop_get_bool_or(const char *name, int default_value)
{
	const char *prop = prop_get(name);
	char buf[128];
	int val;

	if (!prop)
		return default_value;

	if (sscanf(prop, " %d ", &val))
		return val;

	if (sscanf(prop, " %s ", buf))
	{
		if ((buf[0] == 't' || buf[0] == 'T') &&
			(buf[1] == 'r' || buf[1] == 'R') &&
			(buf[2] == 'u' || buf[2] == 'U') &&
			(buf[3] == 'e' || buf[3] == 'E') &&
			(buf[4] == nul))
			return 1;

		if ((buf[0] == 'f' || buf[0] == 'F') &&
			(buf[1] == 'a' || buf[1] == 'A') &&
			(buf[2] == 'l' || buf[2] == 'L') &&
			(buf[3] == 's' || buf[3] == 'S') &&
			(buf[4] == 'e' || buf[4] == 'E') &&
			(buf[5] == nul))
			return 0;

		if ((buf[0] == 'y' || buf[0] == 'Y') &&
			(buf[1] == 'e' || buf[1] == 'E') &&
			(buf[2] == 's' || buf[2] == 'S') &&
			(buf[3] == nul))
			return 1;

		if ((buf[0] == 'n' || buf[0] == 'N') &&
			(buf[1] == 'o' || buf[1] == 'O') &&
			(buf[2] == nul))
			return 0;

		if ((buf[0] == 'o' || buf[0] == 'O') &&
			(buf[1] == 'n' || buf[1] == 'N') &&
			(buf[2] == nul))
			return 1;

		if ((buf[0] == 'o' || buf[0] == 'O') &&
			(buf[1] == 'f' || buf[1] == 'F') &&
			(buf[2] == 'f' || buf[2] == 'F') &&
			(buf[3] == nul))
			return 0;
	}

	return default_value;
}

/*

=item C<int prop_set_bool(const char *name, int value)>

Sets the property named C<name> to C<value>. On success, returns C<value>.
On error, returns 0. If I<prop_save()> is called after a call to this
function, the new property will be saved to disk and will be available the
next time this program is executed.

=cut

*/

int prop_set_bool(const char *name, int value)
{
	return prop_set_int(name, value);
}

/*

=item C<int prop_unset(const char *name)>

Removes the property named C<name>. Property removal is only saved to disk
when I<prop_save()> is called, if the property existed only in the
user-defined, program-specific properties file, or was created by the
program at runtime.

=cut

*/

int prop_unset(const char *name)
{
	Prop *p;

	if (locker_wrlock(g.locker) == -1)
		return -1;

	if (!g.init && prop_init() == -1)
	{
		locker_unlock(g.locker);
		return -1;
	}

	for (p = g.prop; p; p = p->defaults)
		map_remove(p->map, name);

	g.dirty = 1;

	if (locker_unlock(g.locker) == -1)
		return -1;

	return 0;
}

/*

=item C<int prop_save(void)>

Saves the program's properties to disk. If the C<"save"> property is set to
C<"false">, C<"no">, C<"off"> or C<"0">, nothing is written to disk. If no
properties were added, removed or changed, nothing is written to disk. Only
the user-defined, program-specific properties are saved. Generic and
system-wide properties files must be edited by hand. Each program can only
save its own properties. They are saved in the following file:

    $HOME/.properties/app.progname

The F<.properties> directory is created if necessary. On success, returns 0.
On error, returns -1.

=cut

*/

int prop_save(void)
{
	char *path;
	size_t path_len;
	size_t len;
	char *home;
	char *progname, *sep;
	struct stat status[1];
	List *keys;
	Lister *k;
	FILE *file;

	if (!prop_get_bool_or("save", 1))
		return 0;

	if (locker_wrlock(g.locker) == -1)
		return -1;

	if (!g.dirty)
	{
		if (locker_unlock(g.locker) == -1)
			return -1;

		return 0;
	}

	if (!prog_name())
	{
		locker_unlock(g.locker);
		return -1;
	}

	if (!(home = user_home()))
	{
		locker_unlock(g.locker);
		return -1;
	}

	path_len = limit_path();

	if (!(path = mem_create(path_len, char)))
	{
		locker_unlock(g.locker);
		return -1;
	}

	snprintf(path, path_len, "%s%c.properties", home, PATH_SEP);

	if (stat(path, status) == -1 && mkdir(path, S_IRWXU) == -1)
	{
		mem_release(path);
		locker_unlock(g.locker);
		return -1;
	}

	if (stat(path, status) == -1 || S_ISDIR(status->st_mode) == 0)
	{
		mem_release(path);
		locker_unlock(g.locker);
		return -1;
	}

	if (!(progname = mem_strdup(prog_name())))
	{
		mem_release(path);
		locker_unlock(g.locker);
		return -1;
	}

	for (sep = strchr(progname, PATH_SEP); sep; sep = strchr(sep, PATH_SEP))
		*sep++ = '-';

	len = strlen(path);
	snprintf(path + len, path_len - len, "%capp.%s", PATH_SEP, progname);
	mem_release(progname);

	file = fopen(path, "w");
	mem_release(path);

	if (!file)
	{
		locker_unlock(g.locker);
		return -1;
	}

	if (!(keys = map_keys(g.prop->map)))
	{
		locker_unlock(g.locker);
		return -1;
	}

	if (!list_sort(keys, (list_cmp_t *)key_cmp))
	{
		list_release(keys);
		locker_unlock(g.locker);
		return -1;
	}

	if (!(k = lister_create(keys)))
	{
		list_release(keys);
		locker_unlock(g.locker);
		return -1;
	}

	while (lister_has_next(k))
	{
		const char *key = (const char *)lister_next(k);
		const char *value = map_get(g.prop->map, key);
		String *lhs, *rhs, *lhs2;

		/* Quote any '=' in the key */

		if (!(lhs = quote_equals(key)))
		{
			fclose(file);
			lister_release(k);
			list_release(keys);
			locker_unlock(g.locker);
			return -1;
		}

		/* Quote any special chars in the key */

		if (!(lhs2 = quote_special(cstr(lhs))))
		{
			fclose(file);
			lister_release(k);
			list_release(keys);
			str_release(lhs);
			locker_unlock(g.locker);
			return -1;
		}

		str_release(lhs);
		lhs = lhs2;

		/* Quote any special chars in the value */

		if (!(rhs = quote_special(value)))
		{
			fclose(file);
			lister_release(k);
			list_release(keys);
			str_release(lhs);
			locker_unlock(g.locker);
			return -1;
		}

		fprintf(file, "%s=%s\n", cstr(lhs), cstr(rhs));
		str_release(lhs);
		str_release(rhs);
	}

	fclose(file);
	lister_release(k);
	list_release(keys);
	g.dirty = 0;

	if (locker_unlock(g.locker) == -1)
		return -1;

	return 0;
}

/*

=item C<int prop_locker(Locker *locker)>

Sets the locking strategy for the prop module to C<locker>. This is only
needed in multi threaded programs. It must only be called once, from the
main thread. On success, returns C<0>. On error, returns C<-1>.

=cut

*/

int prop_locker(Locker *locker)
{
	if (g.locker)
		return set_errno(EINVAL);

	g.locker = locker;

	return 0;
}

/*

=back

=head1 MT-Level

MT-Disciplined

=head1 FILES

    /etc/properties/app
    $HOME/.properties/app
    /etc/properties/app.*
    $HOME/.properties/app.*

=head1 EXAMPLE

    #include <slack/prog.h>
    #include <slack/prop.h>
    #include <slack/err.h>

    int main(int ac, char **av)
    {
        const char *s; int i; double d; int b;

        prog_init();
        prog_set_name(*av);

        s = prop_get("string");
        i = prop_get_int_or("int", 1);
        d = prop_get_double_or("double", 1.0);
        b = prop_get_bool("boolean");

        msg("s = '%s'\ni = %d\nd = %g\nb = %s\n", s, i, d, (b) ? "true" : "false");

        prop_set("string", "strung");
        prop_set_int("int", i += 4);
        prop_set_double("double", d *= 1.75);
        prop_set_bool("boolean", !b);
        prop_save();
        return 0;
    }

=head1 BUGS

This only provides coarse grained persistence. If multiple instances
of the same program are setting properties, the last to exit wins.

=head1 SEE ALSO

L<libslack(3)|libslack(3)>,
L<prog(3)|prog(3)>

=head1 AUTHOR

20010215 raf <raf@raf.org>

=cut

*/

#ifdef TEST

static int props_exist(void)
{
	struct stat status[1];
	const char *home;
	char *path;
	size_t path_len;

	home = user_home();
	if (!home)
		return 0;

	path_len = limit_path();

	if (!(path = mem_create(path_len, char)))
		return -1;

	snprintf(path, path_len, "%s%c.properties", home, PATH_SEP);

	if (stat(path, status) == 0 && S_ISDIR(status->st_mode) == 1)
	{
		mem_release(path);
		return 1;
	}

	mem_release(path);
	return 0;
}

static void clean(int has_props)
{
	const char *home;
	char *path;
	char *progname;
	char *sep;
	size_t path_len;
	size_t len;

	home = user_home();
	if (!home)
		return;

	path_len = limit_path();

	if (!(path = mem_create(path_len, char)))
		return;

	if (!(progname = mem_strdup(prog_name())))
	{
		mem_release(path);
		return;
	}

	for (sep = strchr(progname, PATH_SEP); sep; sep = strchr(sep, PATH_SEP))
		*sep++ = '-';

	snprintf(path, path_len, "%s%c.properties", home, PATH_SEP);
	len = strlen(path);
	snprintf(path + len, path_len - len, "%capp.%s", PATH_SEP, progname);
	unlink(path);
	path[len] = nul;

	if (!has_props)
		rmdir(path);

	mem_release(path);
	mem_release(progname);
}

int main(int ac, char **av)
{
	struct
	{
		const char *key;
		const char *value;
	}
	data[] =
	{
		{ "key", "value" },
		{ "key with spaces", "value with spaces" },
		{ "key with = sign", " value with leading space" },
		{ "key with newline\n and = two = signs", "value with newline\n!" },
		{ "key with newline,\n = two = signs and an Escape\033!", "value with newline\n and two non printables\001!\002!" },
		{ NULL, NULL }
	};

	const char *key = "key";
	const char *value = "value";
	const char *not_key = "not key";
	int has_props = props_exist();
	const char *val;
	int ival, i;
	int int_val;
	double double_val;
	int bool_val;
	int errors = 0;

	printf("Testing: prop\n");
	prog_init();

	val = prop_set(key, value);
	if (strcmp(val, value))
		++errors, printf("Test1: prop_set(key, value) failed (%s not %s)\n", val, value);
	val = prop_get(key);
	if (strcmp(val, value))
		++errors, printf("Test2: prop_get(key) failed (%s not %s)\n", val, value);
	val = prop_get_or(key, NULL);
	if (strcmp(val, value))
		++errors, printf("Test3: prop_get_or(key, NULL) failed (%s not %s)\n", val, value);
	val = prop_get_or(not_key, value);
	if (strcmp(val, value))
		++errors, printf("Test4: prop_get_or(not_key, value) failed (%s not %s)\n", val, value);
	ival = prop_unset(key);
	if (ival != 0)
		++errors, printf("Test5: prop_unset() failed (%d not %d)\n", ival, 0);
	val = prop_get(key);
	if (val != NULL)
		++errors, printf("Test6: prop_get(key) (after unset) failed (%s not NULL)\n", val);
	val = prop_get_or(key, value);
	if (strcmp(val, value))
		++errors, printf("Test7: prop_get_or(key, value) (after unset) failed (%s not %s)\n", val, value);
	ival = prop_save();
	if (ival != -1)
		++errors, printf("Test8: prop_save() (without progname) failed (%d not -1)\n", ival);

	prog_set_name("prop.test");
	prop_release(g.prop);
	g.prop = NULL;
	g.init = 0;
	g.dirty = 0;

	val = prop_get(not_key);
	if (val != NULL)
		++errors, printf("Test9: prop_get(not_key) failed (%s not NULL)\n", val);
	val = prop_get_or(not_key, value);
	if (val != value)
		++errors, printf("Test10: prop_get_or(not_key, value) failed (%s not %s)\n", val, value);
	val = prop_set(key, value);
	if (strcmp(val, value))
		++errors, printf("Test11: prop_set(key, value) failed (%s not %s)\n", val, value);
	ival = prop_save();
	if (ival != 0)
		++errors, printf("Test12: prop_save() (with progname) failed (%d not 0)\n", ival);

	clean(has_props);
	prop_release(g.prop);
	g.prop = NULL;
	g.init = 0;
	g.dirty = 0;

	for (i = 0; data[i].key; ++i)
	{
		val = prop_set(data[i].key, data[i].value);
		if (strcmp(val, data[i].value))
			++errors, printf("Test%d: prop_set('%s', '%s') failed ('%s' not '%s')\n", 13 + 2 * i, data[i].key, data[i].value, val, value);
		val = prop_get(data[i].key);
		if (strcmp(val, data[i].value))
			++errors, printf("Test%d: prop_get('%s') failed ('%s' not '%s')\n", 14 + 2 * i, data[i].key, val, value);
	}

	ival = prop_save();
	if (ival != 0)
		++errors, printf("Test21: prop_save() (with progname) failed (%d not 0)\n", ival);

	prop_release(g.prop);
	g.prop = NULL;
	g.init = 0;
	g.dirty = 0;

	for (i = 0; data[i].key; ++i)
	{
		val = prop_get(data[i].key);
		if (strcmp(val, data[i].value))
			++errors, printf("Test%d: prop_get('%s') failed ('%s' not '%s')\n", 22 + i, data[i].key, val, data[i].value);
	}

	clean(has_props);
	prop_release(g.prop);
	g.prop = NULL;
	g.init = 0;
	g.dirty = 0;

	if ((int_val = prop_set_int("i", 37)) != 37)
		++errors, printf("Test26: prop_set_int() failed (%d not 37)\n", int_val);

	if ((int_val = prop_get_int("i")) != 37)
		++errors, printf("Test27: prop_get_int() failed (%d not 37)\n", int_val);

	if ((int_val = prop_get_int_or("i", 13)) != 37)
		++errors, printf("Test28: prop_get_int_or() failed (%d not 37)\n", int_val);

	if ((int_val = prop_get_int_or("j", 13)) != 13)
		++errors, printf("Test29: prop_get_int_or() failed (%d not 13)\n", int_val);

	if ((double_val = prop_set_double("d", 37.0)) != 37.0)
		++errors, printf("Test30: prop_set_double() failed (%g not 37.0)\n", double_val);

	if ((double_val = prop_get_double("d")) != 37)
		++errors, printf("Test31: prop_get_double() failed (%g not 37.0)\n", double_val);

	if ((double_val = prop_get_double_or("d", 13.0)) != 37)
		++errors, printf("Test32: prop_get_double_or() failed (%g not 37.0)\n", double_val);

	if ((double_val = prop_get_double_or("e", 13.0)) != 13.0)
		++errors, printf("Test33: prop_get_double_or() failed (%g not 13.0)\n", double_val);

	if ((bool_val = prop_set_bool("b", 1)) != 1)
		++errors, printf("Test34: prop_set_bool() failed (%d not 1)\n", bool_val);

	if ((bool_val = prop_get_bool("b")) != 1)
		++errors, printf("Test35: prop_get_bool() failed (%d not 1)\n", bool_val);

	if ((bool_val = prop_get_bool_or("b", 0)) != 1)
		++errors, printf("Test36: prop_get_bool_or() failed (%d not 1)\n", bool_val);

	if ((bool_val = prop_get_bool_or("c", 1)) != 1)
		++errors, printf("Test37: prop_get_bool_or() failed (%d not 1)\n", bool_val);

	if (errors)
		printf("%d/37 tests failed\n", errors);
	else
		printf("All tests passed\n");

	return 0;
}

#endif

/* vi:set ts=4 sw=4: */
