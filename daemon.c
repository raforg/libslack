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

I<libslack(daemon)> - daemon module

=head1 SYNOPSIS

    #include <slack/daemon.h>

    int daemon_started_by_init(void);
    int daemon_started_by_inetd(void);
    int daemon_prevent_core(void);
    int daemon_revoke_privileges(void);
    char *daemon_absolute_path(const char *path);
    int daemon_path_is_safe(const char *path);
    void *daemon_parse_config(const char *path, void *obj, daemon_config_parser_t *parser);
    int daemon_init(const char *name);
    int daemon_close(void);

=head1 DESCRIPTION

This module provides functions for writing daemons. There are many tasks
that need to be performed to correctly set up a daemon process. This can be
tedious. These functions perform these tasks for you.

=over 4

=cut

*/

#ifndef _BSD_SOURCE
#define _BSD_SOURCE /* For setgroups(2) and S_ISLNK(2) on Linux */
#endif

#include "std.h"

#include <signal.h>
#include <fcntl.h>
#include <pwd.h>
#include <grp.h>

#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/resource.h>

#include "daemon.h"
#include "mem.h"
#include "err.h"
#include "lim.h"
#include "fio.h"

#ifdef NEEDS_SNPRINTF
#include "snprintf.h"
#endif

static struct
{
	pthread_mutex_t lock; /* Mutex lock for structure */
	char *pidfile;        /* Name of the locked pid file */
}
g =
{
	PTHREAD_MUTEX_INITIALIZER,
	NULL
};

#define ptry(action) { int err = (action); if (err) return set_errno(err); }

/*

=item C<int daemon_started_by_init(void)>

If this process was started by I<init(8)>, returns 1. If not, returns 0. If
it was, we might be getting respawned so I<fork(2)> and I<exit(2)> would be
a big mistake (and unnecessary anyway since there is no controlling
terminal). The return value is cached so any subsequent calls are faster.

=cut

*/

int daemon_started_by_init(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static int rc = -1;

	if (rc == -1)
	{
		ptry(pthread_mutex_lock(&lock))

		if (rc == -1)
			rc = (getppid() == 1);

		ptry(pthread_mutex_unlock(&lock))
	}

	return rc;
}

/*

=item C<int daemon_started_by_inetd(void)>

If this process was started by I<inetd(8)>, returns 1. If not, returns 0. If
it was, C<stdin>, C<stdout> and C<stderr> would be opened to a socket.
Closing them would be a big mistake. We also would not need to I<fork(2)>
and I<exit(2)> because there is no controlling terminal. The return value is
cached so any subsequent calls are faster.

=cut

*/

int daemon_started_by_inetd(void)
{
	static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
	static int rc = -1;

	if (rc == -1)
	{
		size_t optlen = sizeof(int);
		int optval;

		ptry(pthread_mutex_lock(&lock))

		if (rc == -1)
			rc = (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &optval, &optlen) == 0);

		ptry(pthread_mutex_unlock(&lock))
	}

	return rc;
}

/*

=item C<int daemon_prevent_core(void)>

Prevents core files from being generated. This is used to prevent security
holes in daemons run by root. On success, returns 0. On error, returns -1
with C<errno> set appropriately.

=cut

*/

int daemon_prevent_core(void)
{
	struct rlimit limit[1] = {{ 0, 0 }};

	if (getrlimit(RLIMIT_CORE, limit) == -1)
		return -1;

	limit->rlim_cur = 0;

	return setrlimit(RLIMIT_CORE, limit);
}

/*

=item C<int daemon_revoke_privileges(void)>

Revokes suid and sgid privileges. Useful when your program does not require
any special privileges and may become unsafe if incorrectly installed with
special privileges. Also useful when your program only requires special
privileges upon startup (e.g. binding to a privileged socket). Performs the
following: Clears all supplementary groups. Sets the effective gid to the
real gid if they differ. Sets the effective uid to the real uid if they
differ. Checks that they no longer differ. Also closes /etc/passwd and
/etc/group in case they were opened by root and give access to user and
group passwords. On success, returns 0. On error, returns -1.

=cut

*/

int daemon_revoke_privileges(void)
{
	uid_t uid = getuid();
	gid_t gid = getgid();
	uid_t euid = geteuid();
	gid_t egid = getegid();

	if (euid == 0 && euid != uid && (setgroups(0, NULL) == -1 || getgroups(0, NULL) != 0))
		return -1;

	if (egid != gid && (setgid(gid) == -1 || getegid() != getgid()))
		return -1;

	if (euid != uid && (setuid(uid) == -1 || geteuid() != getuid()))
		return -1;

	endpwent();
	endgrent();

	return 0;
}

/*

=item C<char *daemon_absolute_path(const char *path)>

Returns C<path> converted into an absolute path. Cleans up any C<.> and
C<..> and C<//> and trailing C</> found in the returned path. Note that the
returned path looks canonical but isn't because symbolic links are not
followed and expanded. It is the caller's responsibility to deallocate the
path returned with C<mem_release(3)> or C<free(3)>. On success, returns the
absolute path. On error, returns C<-1> with C<errno> set appropriately.

=cut

*/

char *daemon_absolute_path(const char *path)
{
	size_t path_len;
	char *abs_path;
	char *p;

	if (!path)
	{
		set_errno(EINVAL);
		return NULL;
	}

	/* Make path absolute and mostly canonical (don't follow symbolic links) */

	if (*path != PATH_SEP)
	{
		long lim = limit_path();
		char *cwd = mem_create(lim, char);
		size_t cwd_len;
		int rc;

		if (!cwd)
			return NULL;

		if (!getcwd(cwd, lim))
		{
			mem_release(cwd);
			return NULL;
		}

		cwd_len = strlen(cwd);

		if (cwd_len + 1 + strlen(path) >= lim)
		{
			mem_release(cwd);
			set_errno(ENAMETOOLONG);
			return NULL;
		}

		rc = snprintf(cwd + cwd_len, lim - cwd_len, "%c%s", PATH_SEP, path);

		if (rc == -1 || rc >= lim - cwd_len)
		{
			mem_release(cwd);
			set_errno(ENAMETOOLONG);
			return NULL;
		}

		abs_path = cwd;
	}
	else
	{
		abs_path = mem_strdup(path);
		if (!abs_path)
			return NULL;
	}

	/* Clean up any // and . and .. in the absolute path */

	path_len = strlen(abs_path);

	for (p = abs_path; *p; ++p)
	{
		if (p[0] == PATH_SEP)
		{
			if (p[1] == PATH_SEP)
			{
				memmove(p, p + 1, path_len + 1 - (p + 1 - abs_path));
				--path_len;
				--p;
			}
			else if (p[1] == '.')
			{
				if (p[2] == PATH_SEP || p[2] == nul)
				{
					int keep_sep = (p == abs_path && p[2] == nul);
					memmove(p + keep_sep, p + 2, path_len + 1 - (p + 2 - abs_path));
					path_len -= 2 - keep_sep;
					--p;
				}
				else if (p[2] == '.' && (p[3] == PATH_SEP || p[3] == nul))
				{
					char *scan, *parent;
					int keep_sep;

					for (scan = parent = p; scan > abs_path; )
					{
						if (*--scan == PATH_SEP)
						{
							parent = scan;
							break;
						}
					}

					keep_sep = (parent == abs_path && p[3] == nul);
					memmove(parent + keep_sep, p + 3, path_len + 1 - (p + 3 - abs_path));
					path_len -= p + 3 - parent;
					p = parent - 1;
				}
			}
		}
	}

	/* Strip off any trailing / */

	while (path_len > 1 && abs_path[path_len - 1] == PATH_SEP)
		abs_path[--path_len] = nul;

	return abs_path;
}

/*

=item C<int daemon_path_is_safe(const char *path)>

Checks that the file referred to by C<path> is not group or world writable.
Also checks that the containing directories are not group or world writable,
following symbolic links. Useful when you need to know whether or not you
can trust a user supplied configuration/command file before reading and
acting upon its contents. On success, returns 1 if C<path> is safe or 0 if
it is not. On error, returns C<-1> with C<errno> set appropriately.

=cut

*/

static int daemon_check_path(char *path, int level)
{
	struct stat status[1];
	char *sep;
	int rc;

	if (level > 16)
		return set_errno(ELOOP);

	for (sep = path + strlen(path); sep; sep = strrchr(path, PATH_SEP))
	{
		sep[sep == path] = nul;

		if (lstat(path, status) == -1)
			return -1;

		if (S_ISLNK(status->st_mode))
		{
			size_t lim;
			char *sym_linked;
			char *tmp;

			lim = limit_path();
			sym_linked = mem_create(lim, char);
			if (!sym_linked)
				return -1;

			memset(sym_linked, nul, lim);

			if (readlink(path, sym_linked, lim) == -1)
			{
				mem_release(sym_linked);
				return -1;
			}

			if (*sym_linked != PATH_SEP)
			{
				tmp = mem_create(lim, char);
				if (!tmp)
				{
					mem_release(sym_linked);
					return -1;
				}

				rc = snprintf(tmp, lim, "%s%c..%c%s", path, PATH_SEP, PATH_SEP, sym_linked);
				if (rc == -1 || rc >= lim)
				{
					mem_release(sym_linked);
					mem_release(tmp);
					return set_errno(ENAMETOOLONG);
				}

				rc = snprintf(sym_linked, lim, "%s", tmp);
				mem_release(tmp);
				if (rc == -1 || rc >= lim)
				{
					mem_release(sym_linked);
					return set_errno(ENAMETOOLONG);
				}
			}

			tmp = daemon_absolute_path(sym_linked);
			mem_release(sym_linked);
			sym_linked = tmp;
			if (!sym_linked)
				return -1;

			rc = daemon_check_path(sym_linked, level + 1);
			mem_release(sym_linked);

			switch (rc)
			{
				case -1: return -1;
				case  0: return 0;
				case  1: break;
			}
		}
		else if (status->st_mode & (S_IWGRP | S_IWOTH))
		{
			return 0;
		}

		if (sep == path)
			break;
	}

	return 1;
}

int daemon_path_is_safe(const char *path)
{
	char *abs_path;
	int rc;

	if (!path)
		return set_errno(EINVAL);

	abs_path = daemon_absolute_path(path);
	if (!abs_path)
		return -1;

	rc = daemon_check_path(abs_path, 0);
	mem_release(abs_path);

	return rc;
}

/*

=item C<void *daemon_parse_config(const char *path, void *obj, daemon_config_parser_t *parser)>

Parses the text configuration file named C<path>. Blank lines are ignored.
Comments (C<`#'> to end of line) are ignored. Lines that end with C<`\'> are
joined with the following line. There may be whitespace and even a comment
after the C<`\'> character but nothing else. The C<parser> function is
called with the client supplied C<obj>, the file name, the line and the line
number as arguments. On success, returns C<obj>. On errors, returns C<NULL>
(i.e. if the configuration file could not be read). Note: Don't parse config
files unless they are "safe" as determined by I<daemon_path_is_safe()>.

=cut

*/

void *daemon_parse_config(const char *path, void *obj, daemon_config_parser_t *parser)
{
	FILE *conf;
	char line[BUFSIZ];
	char buf[BUFSIZ];
	int lineno;
	int rc;

	if (!(conf = fopen(path, "r")))
		return NULL;

	line[0] = nul;

	for (lineno = 1; fgets(buf, BUFSIZ, conf); ++lineno)
	{
		char *start = buf;
		char *end;
		size_t length;
		int cont_line;

		/* Strip trailing comments */

		end = strchr(start, '#');
		if (end)
			*end = nul;
		else
			end = start + strlen(start);

		/* Skip trailing spaces (allows comments after line continuation) */

		while (end > start && isspace((int)(unsigned char)end[-1]))
			--end;

		/* Skip empty lines */

		if (*start == nul || start == end)
			continue;

		/* Perform line continuation */

		cont_line = (end[-1] == '\\');
		if (cont_line)
			--end;

		length = strlen(line);
		rc = snprintf(line + length, BUFSIZ - length, "%*.*s", end - start, end - start, start);
		if (rc == -1 || rc >= BUFSIZ - length)
			return NULL;

		if (cont_line)
			continue;

		/* Parse the resulting line */

		parser(obj, path, line, lineno);
		line[0] = nul;
	}

	fclose(conf);

	return obj;
}

/*

C<int daemon_pidfile(const char *name)>

Creates a pid file for a daemon and locks it. The file has one line
containing the process id of the daemon. The well-known locations for the
file is defined in C<ROOT_PID_DIR> for I<root> (by default, C<"/var/run"> on
Linux and C<"/etc"> on Solaris) and C<USER_PID_DIR> for all other users
(C<"/tmp"> by default). The name of the file is the name of the daemon
(given by the name argument) followed by C<".pid">. The presence of this
file will prevent two daemons with the same name from running at the same
time. On success, returns 0. On error, returns -1 with C<errno> set
appropriately.

*/

static int daemon_pidfile(const char *name)
{
	mode_t mode;
	char pid[32];
	int pid_fd;
	long path_len;
	const char *pid_dir;
	char *suffix = ".pid";
	int rc;

	path_len = limit_path();
	pid_dir = (getuid()) ? USER_PID_DIR : ROOT_PID_DIR;

	if (sizeof(pid_dir) + 1 + strlen(name) + strlen(suffix) + 1 > path_len)
		return set_errno(ENAMETOOLONG);

	if (!g.pidfile && !(g.pidfile = mem_create(path_len, char)))
		return set_errno(ENOMEM);

	rc = snprintf(g.pidfile, path_len, "%s%c%s%s", pid_dir, PATH_SEP, name, suffix);
	if (rc == -1 || rc >= path_len)
	{
		mem_destroy(&g.pidfile);
		return set_errno(ENOSPC);
	}

	/* This is broken over NFS (Linux). So pidfiles must reside locally. */

	mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;

	if ((pid_fd = open(g.pidfile, O_RDWR | O_CREAT | O_EXCL, mode)) == -1)
	{
		if (errno != EEXIST)
		{
			mem_destroy(&g.pidfile);
			return -1;
		}

		/*
		** The pidfile already exists. Is it locked?
		** If so, another invocation is still alive.
		** If not, the invocation that created it has died.
		** Open the pidfile to attempt a lock.
		*/

		if ((pid_fd = open(g.pidfile, O_RDWR)) == -1)
		{
			mem_destroy(&g.pidfile);
			return -1;
		}
	}

	if (fcntl_lock(pid_fd, F_SETLK, F_WRLCK, SEEK_SET, 0, 0) == -1)
	{
		mem_destroy(&g.pidfile);
		return -1;
	}

	/* It wasn't locked. Now we have it locked, store our pid. */

	rc = snprintf(pid, 32, "%d\n", getpid());
	if (rc == -1 || rc >= 32)
	{
		mem_destroy(&g.pidfile);
		return set_errno(ENOSPC);
	}

	if (write(pid_fd, pid, strlen(pid)) != strlen(pid))
	{
		daemon_close();
		return -1;
	}

	/*
	** Flaw: If someone unceremoniously unlinks the pidfile,
	** we won't know about it and nothing will stop another
	** invocation from starting up.
	*/

	return 0;
}

/*

=item C<int daemon_init(const char *name)>

Initialises a daemon by performing the following tasks:

=over 4

=item *

If the process was not invoked by I<init(8)> or I<inetd(8)>:

=over 4

=item *

Background the process to lose process group leadership.

=item *

Start a new process session.

=item *

Under I<SVR4>, background the process again to lose process session
leadership. This prevents the process from ever gaining a controlling
terminal. This only happens when C<SVR4> is defined and
C<NO_EXTRA_SVR4_FORK> is not defined when I<libslack> is compiled.

=back

=item *

Change to the root directory so as not to hamper umounts.

=item *

Clear the umask to enable explicit file creation modes.

=item *

Close all open file descriptors. If the process was invoked by I<inetd(8)>,
C<stdin>, C<stdout> and C<stderr> are left open since they are open to a
socket.

=item *

Open C<stdin>, C<stdout> and C<stderr> to C</dev/null> in case something
requires them to be open. Of course, this is not done if the process was
invoked by I<inetd(8)>.

=item *

If C<name> is non-NULL, create and lock a file containing the process id of
the process. The presence of this locked file prevents two instances of a
daemon with the same name from running at the same time.

=back

On success, returns 0. On error, returns -1 with C<errno> set appropriately.

=cut

*/

int daemon_init(const char *name)
{
	pid_t pid;
	long nopen;
	int fd;

	/*
	** Don't setup a daemon-friendly process context
	** if started by init(8) or inetd(8).
	*/

	if (!(daemon_started_by_init() || daemon_started_by_inetd()))
	{
		/*
		** Background the process.
		** Lose process group leadership.
		*/

		if ((pid = fork()) == -1)
			return -1;

		if (pid)
			exit(0);

		/* Become a process session leader. */

		setsid();

#ifndef NO_EXTRA_SVR4_FORK
#ifdef SVR4
		/*
		** Lose process session leadership
		** to prevent gaining a controlling
		** terminal in SVR4.
		*/

		if ((pid = fork()) == -1)
			return -1;

		if (pid)
			exit(0);
#endif
#endif
	}

	/* Enter the root directory to prevent hampering umounts. */

	if (chdir(ROOT_DIR) == -1)
		return -1;

	/* Clear umask to enable explicit file modes. */

	umask(0);

	/*
	** We need to close all open file descriptors. Check how
	** many file descriptors we have (If indefinite, a usable
	** number (1024) will be returned).
	*/

	if ((nopen = limit_open()) == -1)
		return -1;

	/*
	** Close all open file descriptors. If started by inetd,
	** we don't close stdin, stdout and stderr.
	** Don't forget to open any future tty devices with O_NOCTTY
	** so as to prevent gaining a controlling terminal
	** (not necessary with SVR4).
	*/

	if (daemon_started_by_inetd())
	{
		for (fd = 0; fd < nopen; ++fd)
		{
			switch (fd)
			{
				case STDIN_FILENO:
				case STDOUT_FILENO:
				case STDERR_FILENO:
					break;
				default:
					close(fd);
			}
		}
	}
	else
	{
		for (fd = 0; fd < nopen; ++fd)
			close(fd);

		/*
		** Open stdin, stdout and stderr to /dev/null just in case some
		** code buried in a library somewhere expects them to be open.
		*/

		if ((fd = open("/dev/null", O_RDWR)) == -1)
			return -1;

		/*
		** This is only needed for very strange (hypothetical)
		** POSIX implementations where STDIN_FILENO != 0 or
		** STDOUT_FILE != 1 or STERR_FILENO != 2 (yeah, right).
		*/

		if (fd != STDIN_FILENO)
		{
			if (dup2(fd, STDIN_FILENO) == -1)
				return -1;

			close(fd);
		}

		if (dup2(STDIN_FILENO, STDOUT_FILENO) == -1)
			return -1;

		if (dup2(STDIN_FILENO, STDERR_FILENO) == -1)
			return -1;
	}

	/* Place our process id in the file system and lock it. */

	if (name)
	{
		int rc;

		ptry(pthread_mutex_lock(&g.lock))
		rc = daemon_pidfile(name);
		ptry(pthread_mutex_unlock(&g.lock))

		return rc;
	}

	return 0;
}

/*

=item C<int daemon_close(void)>

Unlinks the locked pid file, if any. Returns 0.

=cut

*/

int daemon_close(void)
{
	ptry(pthread_mutex_lock(&g.lock))

	if (g.pidfile)
	{
		unlink(g.pidfile);
		mem_destroy(&g.pidfile);
	}

	ptry(pthread_mutex_unlock(&g.lock))

	return 0;
}

/*

=back

=head1 ERRORS

Additional errors may be generated and returned from the underlying system
calls. See their manual pages.

=over 4

=item ENAMETOOLONG

The C<name> passed to I<daemon_init()> resulted in a path name that is
too long for the intended filesystem.

=item ENOMEM

I<daemon_init()> failed to allocate memory for the the pid file's path.

=back

=head1 EXAMPLE

    #include <stdio.h>

    #include <unistd.h>
    #include <syslog.h>
    #include <signal.h>

    #include <slack/daemon.h>
    #include <slack/prog.h>
    #include <slack/sig.h>

    void hup(int signo)
    {
        // reload config file...
    }

    void term(int signo)
    {
        daemon_close();
        exit(0);
    }

    void do_stuff()
    {
        // do stuff...
        kill(getpid(), SIGTERM);
        signal_handle_all();
    }

    void fstab_parser(void *obj, const char *path, char *line, size_t lineno)
    {
        char device[64], mount[64], fstype[64], opts[64];
        int freq, passno;

        if (sscanf(line, "%s %s %s %s %d %d", device, mount, fstype, opts, &freq, &passno) != 6)
            fprintf(stderr, "Syntax error in %s (line %d): %s\n", path, lineno, line);
        else
            printf("%s %s %s %s %d %d\n", device, mount, fstype, opts, freq, passno);
    }

    int main(int ac, char **av)
    {
        const char * const config = "/etc/fstab";

        if (daemon_revoke_privileges() == -1 ||
            daemon_prevent_core() == -1 ||
            daemon_parse_config(config, NULL, fstab_parser) == NULL ||
            daemon_init(prog_basename(*av)) == -1 ||
            signal_set_handler(SIGHUP, 0, hup) == -1 ||
            signal_set_handler(SIGTERM, 0, term) == -1 ||
            daemon_path_is_safe(config) != 1)
            return 1;

        do_stuff();
        return 0; // unreached
    }

=head1 MT-Level

MT-Safe

=head1 BUGS

It is possible to obtain a controlling terminal under I<BSD> (and even under
I<SVR4> if C<SVR4> was not defined or C<NO_EXTRA_SVR4_FORK> was defined when
I<libslack> is compiled). If anything calls I<open(2)> on a terminal device
without the C<O_NOCTTY> flag, the process doing so will obtain a controlling
terminal.

Because I<root>'s pidfiles are created in a different directory (C</var/run>
on Linux, C</etc> on Solaris) to those of ordinary users (C</tmp>), it is
possible for I<root> and another user to use the same name for a daemon
client. This shouldn't be a problem but if it is, recompile I<libslack> and
relink I<daemon> so that all pidfiles are created in C</tmp> by defining
C<ROOT_PID_DIR> and C<USER_PID_DIR> to both be C</tmp>.

The exclusive creation and locking of the pidfile doesn't work correctly
over NFS on Linux so pidfiles must reside locally.

I<daemon_path_is_safe()> ignores ACLs (so does I<sendmail>). It should
probably treat a path as unsafe if there are any ACLs (allowing extra
access) along the path.

The functions C<daemon_prevent_core()>, C<daemon_revoke_privileges()>,
C<daemon_absolute_path()>, C<daemon_path_is_safe()> and
C<daemon_parse_config()> should probably all have the C<daemon_> prefix
removed from their names. Their use is more general than just in daemons.

=head1 SEE ALSO

L<libslack(3)|libslack(3)>,
L<daemon(1)|daemon(1)>,
L<init(8)|init(8)>,
L<inetd(8)|inetd(8)>

=head1 AUTHOR

20010215 raf <raf@raf.org>

=cut

*/

#ifdef TEST

#include <syslog.h>
#include <wait.h>

#include "msg.h"
#include "prog.h"
#include "sig.h"

typedef struct Pair1 Pair1;
typedef struct Data1 Data1;
typedef struct Data2 Data2;

struct Data1
{
	int test;
	int i;
	Pair1 *pair;
};

struct Pair1
{
	const char *service;
	const char *port;
};

static Pair1 pairs[] =
{
	{ "echo", "7/tcp" },
	{ "echo", "7/udp" },
	{ "ftp", "21/tcp" },
	{ "ssh", "22/tcp" },
	{ "smtp", "25/tcp" },
	{ NULL, NULL }
};

static const int final_pair = 5;

static Data1 data1[1] = {{ 0, 0, pairs }};

struct Data2
{
	int test;
	int i;
	int j;
	const char *text;
	const char *results[3][8];
};

static Data2 data2[1] =
{
	{
		0,
		0,
		0,
		"\n"
		"# This is a comment\n"
		"\n"
		"line1 = word1 word2\n"
		"line2 = word3 \\\n"
		"\tword4 word5 \\ # a comment in a funny place\n"
		"\tword6 word7\n"
		"\n"
		"line3 = \\\n"
		"\tword8\n"
		"\n",
		{
			{ "line1", "=", "word1", "word2", NULL, NULL, NULL, NULL },
			{ "line2", "=", "word3", "word4", "word5", "word6", "word7", NULL },
			{ "line3", "=", "word8", NULL, NULL, NULL, NULL, NULL }
		}
	}
};

static const int final_line = 3;
static const int final_word = 3;

static int errors = 0;

static int config_test1(int test, const char *name)
{
	FILE *out = fopen(name, "w");
	int i;

	if (!out)
	{
		++errors, printf("Test%d: failed to create file: '%s'\n", test, name);
		return 0;
	}

	for (i = 0; data1->pair[i].service; ++i)
		fprintf(out, "%s %s\n", data1->pair[i].service, data1->pair[i].port);

	fclose(out);
	return 1;
}

static void parse_test1(void *obj, const char *path, char *line, size_t lineno)
{
	Data1 *data1 = (Data1 *)obj;
	char service[BUFSIZ];
	char port[BUFSIZ];

	if (sscanf(line, "%s %s", service, port) != 2)
		++errors, printf("Test%d: syntax error: '%s' (file %s line %d)\n", data1->test, line, path, lineno);
	else if (strcmp(service, data1->pair[data1->i].service))
		++errors, printf("Test%d: expected service '%s', received '%s' (file %s line %d)\n", data1->test, data1->pair[data1->i].service, service, path, lineno);
	else if (strcmp(port, data1->pair[data1->i].port))
		++errors, printf("Test%d: expected port '%s', received '%s' (file %s line %d)\n", data1->test, data1->pair[data1->i].port, port, path, lineno);
	++data1->i;
}

static int config_test2(int test, const char *name)
{
	FILE *out = fopen(name, "w");

	if (!out)
	{
		++errors, printf("Test%d: failed to create file: '%s'\n", test, name);
		return 0;
	}

	fprintf(out, "%s", data2->text);
	fclose(out);
	return 1;
}

static void parse_test2(void *obj, const char *path, char *line, size_t lineno)
{
	Data2 *data2 = (Data2 *)obj;
	char word[8][BUFSIZ];
	int words;
	
	words = sscanf(line, "%s %s %s %s %s %s %s %s", word[0], word[1], word[2], word[3], word[4], word[5], word[6], word[7]);

	for (data2->j = 0; data2->j < words; ++data2->j)
	{
		if (!data2->results[data2->i][data2->j])
		{
			++errors, printf("Test%d: too many words: '%s' (file %s line %d)\n", data2->test, line, path, lineno);
			break;
		}

		if (strcmp(word[data2->j], data2->results[data2->i][data2->j]))
		{
			++errors;
			printf("Test%d: expected '%s', received '%s' (file %s line %d)\n", data2->test, data2->results[data2->i][data2->j], word[data2->j - 1], path, lineno);
			break;
		}
	}

	++data2->i;
}

void term(int signo)
{
	daemon_close();
	exit(0);
}

int main(int ac, char **av)
{
	const char *config_name;
	char *cwd;
	int facility = LOG_DAEMON | LOG_ERR;
	pid_t pid;
	int rc;
	uid_t uid, euid;
	gid_t gid, egid;
	int no_privileges = 0;
	int not_safe = 0;

	printf("Testing: daemon\n");

	/* Test (a bit) daemon_started_by_init() and daemon_started_by_inetd() */

	if ((rc = daemon_started_by_init()) != 0)
		++errors, printf("Test1: daemon_started_by_init() failed (ret %d, not %d)\n", rc, 0);

	if ((rc = daemon_started_by_inetd()) != 0)
		++errors, printf("Test2: daemon_started_by_inetd() failed (ret %d, not %d)\n", rc, 0);

	/* Test daemon_prevent_core() */

	unlink("core");

	switch (pid = fork())
	{
		case -1:
		{
			++errors, printf("Test3: Failed to run test: fork: %s\n", strerror(errno));
			break;
		}

		case 0:
		{
			if (daemon_prevent_core() == -1)
			{
				printf("Test3: daemon_prevent_core() failed: %s\n", strerror(errno));
				return 1;
			}

			dump("");
		}

		default:
		{
			struct stat statbuf[1];
			int status;

			if (waitpid(pid, &status, 0) == -1)
			{
				printf("Test3: Failed to evaluate test: waitpid: %s\n", strerror(errno));
				break;
			}

			if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
				++errors;
			else if (WCOREDUMP(status) || stat("core", statbuf) == 0)
				++errors, printf("Test3: child dumped core\n");
			unlink("core");
		}
	}

	/* Test daemon_revoke_privileges() if possible */

	uid = getuid();
	gid = getgid();
	euid = geteuid();
	egid = getegid();

	if (euid == uid && egid == gid)
		no_privileges = 1;
	else if (daemon_revoke_privileges() == -1 || geteuid() != getuid() || getegid() != getgid())
		++errors, printf("Test4: daemon_revoke_privileges() failed: %s\n", strerror(errno));

	/* Test daemon_absolute_path() */

#define TEST_ABSOLUTE_PATH(i, path, abs_path) \
	{ \
		char *result = daemon_absolute_path(path); \
		if (!result) \
			++errors, printf("Test%d: absolute_path(%s) failed (%s)\n", (i), (path), strerror(errno)); \
		else if (strcmp(result, (abs_path))) \
		{ \
			++errors, printf("Test%d: absolute_path(%s) failed (was %s, not %s)\n", (i), (path), result, (abs_path)); \
			free(result); \
		} \
	}

	/* We must be in a safe, writable directory to test relative paths */

	if (!(cwd = mem_create(limit_path(), char)))
		++errors, printf("Test5: Failed to run test: mem_create: %s\n", strerror(errno));
	else if (!getcwd(cwd, limit_path()))
		++errors, printf("Test5: Failed to run test: getcwd: %s\n", strerror(errno));
	else if (chdir("/etc") == -1)
		++errors, printf("Test5: Failed to run test: chdir: %s\n", strerror(errno));
	else
	{
		TEST_ABSOLUTE_PATH(5, ".", "/etc")
		TEST_ABSOLUTE_PATH(6, "..", "/")
		TEST_ABSOLUTE_PATH(7, "/", "/")
		TEST_ABSOLUTE_PATH(8, "/etc/passwd", "/etc/passwd")
		TEST_ABSOLUTE_PATH(9, "/.", "/")
		TEST_ABSOLUTE_PATH(10, "/..", "/")
		TEST_ABSOLUTE_PATH(11, "/./etc", "/etc")
		TEST_ABSOLUTE_PATH(12, "/../etc", "/etc")
		TEST_ABSOLUTE_PATH(13, "/etc/.././.././../usr", "/usr")
		TEST_ABSOLUTE_PATH(14, "../../../../../etc/././../etc/./.././etc", "/etc")
		TEST_ABSOLUTE_PATH(15, "././../../../../../etc/././.", "/etc")
		TEST_ABSOLUTE_PATH(16, "/etc/./sysconfig/./network-scripts/../blog/..", "/etc/sysconfig")
		TEST_ABSOLUTE_PATH(17, "/etc/./sysconfig/./network-scripts/../blog/../..", "/etc")
		TEST_ABSOLUTE_PATH(18, "passwd", "/etc/passwd")
		TEST_ABSOLUTE_PATH(19, "passwd/", "/etc/passwd")
		TEST_ABSOLUTE_PATH(20, "passwd////", "/etc/passwd")
		TEST_ABSOLUTE_PATH(21, "///////////////", "/")
		TEST_ABSOLUTE_PATH(22, "///////etc////////", "/etc")
		TEST_ABSOLUTE_PATH(23, "//////./.././..////..//", "/")
		chdir(cwd);
	}

	/* Test daemon_path_is_safe() */

#define TEST_PATH_IS_SAFE(i, path, safe, err) \
	{ \
		int rc; \
		errno = 0; \
		if ((rc = daemon_path_is_safe(path)) != (safe)) \
			++errors, printf("Test%d: daemon_path_is_safe(%s) failed (ret %d, not %d) %s\n", (i), (path), rc, (safe), errno ? strerror(errno) : ""); \
		else if (rc == -1 && errno != (err)) \
			++errors, printf("Test%d: daemon_path_is_safe(%s) failed (errno was %d, not %d)\n", (i), (path), errno, (err)); \
	}

	TEST_PATH_IS_SAFE(24, "/etc/passwd", 1, 0)
	TEST_PATH_IS_SAFE(25, "/tmp", 0, 0)
	TEST_PATH_IS_SAFE(26, "/nonexistent-path", -1, ENOENT)

	if (daemon_path_is_safe(".") != 1)
		not_safe = 1;
	else
	{
		const char *sym_link = "daemon_path_is_safe.test";
		const char *sym_linked = "/tmp/daemon_path_is_safe.test";
		mode_t mask;
		int fd;

		/* Test absolute link from safe directory to unsafe directory */

		if ((fd = open(sym_linked, O_CREAT | O_EXCL, S_IRUSR | S_IWUSR)) == -1)
			++errors, printf("Test27: Failed to run test: open(%s) failed %s\n", sym_linked, strerror(errno));
		else
		{
			close(fd);
			TEST_PATH_IS_SAFE(27, sym_linked, 0, 0)

			if (symlink(sym_linked, sym_link) == -1)
				++errors, printf("Test27: Failed to run test: symlink(%s, %s) failed %s\n", sym_linked, sym_link, strerror(errno));
			else
			{
				TEST_PATH_IS_SAFE(27, sym_link, 0, 0)

				if (unlink(sym_link) == -1)
					++errors, printf("Test27: Failed to unlink(%s): %s\n", sym_link, strerror(errno));
			}

			if (unlink(sym_linked) == -1)
				++errors, printf("Test27: Failed to unlink(%s): %s\n", sym_linked, strerror(errno));
		}

		/* Test relative symbolic link from safe directory to safe directory */

		if (mkdir("safedir", S_IRUSR | S_IWUSR | S_IXUSR) == -1)
			++errors, printf("Test28: Failed to run test: mkdir(%s) failed: %s\n", "safedir", strerror(errno));
		else
		{
			if (symlink("..", "safedir/safelink") == -1)
				++errors, printf("Test28: symlink(.., safedir/safelink) failed: %s\n", strerror(errno));
			else
			{
				TEST_PATH_IS_SAFE(28, "safedir/safelink", 1, 0)

				if (unlink("safedir/safelink") == -1)
					++errors, printf("Test28: Failed to unlink(safedir/safelink): %s\n", strerror(errno));
			}

			if (rmdir("safedir") == -1)
				++errors, printf("Test28: Failed to rmdir(safedir): %s\n", strerror(errno));
		}

		/* Test relative symbolic link from safe directory to unsafe directory */

		if (mkdir("safedir", S_IRUSR | S_IWUSR | S_IXUSR) == -1)
			++errors, printf("Test29: Failed to run test: mkdir(safedir) failed: %s\n", strerror(errno));
		else
		{
			mask = umask((mode_t)0);

			if (mkdir("unsafedir", S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP) == -1)
				++errors, printf("Test29: Failed to run test: mkdir(unsafedir) failed: %s\n", strerror(errno));
			else
			{
				if (symlink("../unsafedir", "safedir/unsafelink") == -1)
					++errors, printf("Test29: symlink(../unsafedir, safedir/unsafelink) failed: %s\n", strerror(errno));
				else
				{
					TEST_PATH_IS_SAFE(28, "safedir/unsafelink", 0, 0)

					if (unlink("safedir/unsafelink") == -1)
						++errors, printf("Test29: Failed to unlink(safedir/unsafelink): %s\n", strerror(errno));
				}

				if (rmdir("unsafedir") == -1)
					++errors, printf("Test29: Failed to rmdir(unsafedir): %s\n", strerror(errno));
			}

			if (rmdir("safedir") == -1)
				++errors, printf("Test29: Failed to rmdir(safedir): %s\n", strerror(errno));

			umask(mask);
		}

		/* Test relative symbolic link from unsafe directory to safe directory */

		mask = umask((mode_t)0);

		if (mkdir("unsafedir", S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP) == -1)
			++errors, printf("Test30: Failed to run test: mkdir(unsafedir) failed: %s\n", strerror(errno));
		else
		{
			if (symlink("unsafedir", "unsafelink") == -1)
				++errors, printf("Test30: symlink(../unsafedir, unsafelink) failed: %s\n", strerror(errno));
			else
			{
				if (mkdir("unsafelink/unsafedir", S_IRUSR | S_IWUSR | S_IXUSR) == -1)
					++errors, printf("Test30: Failed to run test: mkdir(unsafelink/unsafedir) failed: %s\n", strerror(errno));
				else
				{
					TEST_PATH_IS_SAFE(30, "unsafelink/unsafedir", 0, 0)

					if (rmdir("unsafelink/unsafedir") == -1)
						++errors, printf("Test30: Failed to rmdir(unsafelink/unsafedir): %s\n", strerror(errno));
				}

				if (unlink("unsafelink") == -1)
					++errors, printf("Test30: Failed to unlink(unsafelink): %s\n", strerror(errno));
			}

			if (rmdir("unsafedir") == -1)
				++errors, printf("Test30: Failed to rmdir(unsafedir): %s\n", strerror(errno));
		}

		umask(mask);
	}

	/* Test daemon_parse_config() */

	config_name = "daemon_parse_config.testfile";

	if (config_test1(31, config_name))
	{
		int errors_save = errors;
		data1->test = 31;
		daemon_parse_config(config_name, data1, parse_test1);
		if (errors == errors_save && data1->i != final_pair)
			++errors, printf("Test31: failed to parse entire config file\n");
		unlink(config_name);
	}

	if (config_test2(32, config_name))
	{
		int errors_save = errors;
		data2->test = 32;
		daemon_parse_config(config_name, data2, parse_test2);
		if (errors == errors_save && (data2->i != final_line || data2->j != final_word))
			++errors, printf("Test32: failed to parse entire config file\n");
		unlink(config_name);
	}

	/* Test daemon_init() and daemon_close() */

	switch (pid = fork())
	{
		case -1:
		{
			++errors, printf("Test33: Failed to run test: fork: %s\n", strerror(errno));
			break;
		}

		case 0:
		{
			if (signal_set_handler(SIGTERM, 0, term) == -1)
			{
				syslog(facility, "%s: Test33: signal_set_handler() failed: %s", *av, strerror(errno));
				return 1;
			}

			if (daemon_init(prog_basename(*av)) == -1)
			{
				syslog(facility, "%s: Test33: daemon_init() failed: %s", *av, strerror(errno));
			}
			else
			{
				syslog(facility, "%s succeeded", *av);

				if (kill(getpid(), SIGTERM) == -1)
					syslog(facility, "%s: Test33: kill(%d, SIGTERM) failed: %s", *av, pid, strerror(errno));

				signal_handle_all();

				/*
				** We can only get here if the signal hasn't arrived yet.
				** If so, exit anyway.
				*/

				syslog(facility, "%s: Test33: signal_handle_all() failed", *av);
				_exit(0);
			}

			return 0; /* unreached */
		}

		default:
		{
			int status;

			if (waitpid(pid, &status, 0) == -1)
			{
				printf("Test33: Failed to evaluate test: waitpid: %s\n", strerror(errno));
				break;
			}

			if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
				++errors, printf("Test33: Failed to run test: signal_set_handler() failed\n");
		}
	}

	if (errors)
		printf("%d/33 tests failed\n", errors);
	else
		printf("All tests passed\n");

	printf("\n");
	printf("    Note: Can't verify syslog daemon.err output (don't know where it goes).\n");
	printf("    Look for \"%s succeeded\" (not \"%s failed\").\n", *av, *av);

	if (no_privileges)
	{
		printf("\n");
		printf("    Note: Can't test daemon_revoke_privileges().\n");
		printf("    Rerun test suid and/or sgid as someone other than the tester.\n");
	}

	if (not_safe)
	{
		printf("\n");
		printf("    Note: Can't perform all tests on daemon_path_is_safe().\n");
		printf("    Rerun test from a safe directory (writable by the tester).\n");
	}

	return 0;
}

#endif

/* vi:set ts=4 sw=4: */
