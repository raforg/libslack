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
* 20011109 raf <raf@raf.org>
*/

/*

=head1 NAME

I<libslack(coproc)> - coprocess module

=head1 SYNOPSIS

    #include <slack/coproc.h>

    pid_t coproc_open(int *to, int *from, const char *cmd, char * const *argv, char * const *envv);
    int coproc_close(pid_t pid, int *to, int *from);
    pid_t coproc_pty_open(int *masterfd, char *slavename, size_t slavenamesize, const struct termios *slave_termios, const struct winsize *slave_winsize, const char *cmd, char * const *argv, char * const *envv);
    int coproc_pty_close(pid_t pid, int *masterfd, const char *slavename);

=head1 DESCRIPTION

This module contains functions for creating coprocesses that use either pipes
or pseudo terminals for communication.

=over 4

=cut

*/

#include "config.h"
#include "std.h"

#include <sys/wait.h>

#include "coproc.h"
#include "daemon.h"
#include "pseudo.h"
#include "err.h"

#ifndef HAVE_SNPRINTF
#include "snprintf.h"
#endif

#ifndef TEST

extern char **environ;

#ifndef SHELL_META_CHARACTERS
#define SHELL_META_CHARACTERS "|&;()<>[]{}$`'~\"\\*? \t\r\n"
#endif

#ifndef DEFAULT_ROOT_PATH
#define DEFAULT_ROOT_PATH "/bin:/usr/bin"
#endif

#ifndef DEFAULT_USER_PATH
#define DEFAULT_USER_PATH ":/bin:/usr/bin"
#endif

#define RD 0
#define WR 1

/*

=item C<pid_t coproc_open(int *to, int *from, const char *cmd, char * const *argv, char * const *envv)>

Starts a coprocess. C<cmd> is the name of the process or a shell command.
C<argv> is the command line argument vector to be passed to I<execve()>.
C<envv> is the environment variable vector to be passed to I<execve()>. If
C<envv> is C<null>, the current environment is used. If C<cmd> is the name
of a program, C<argv> must not be C<null>. If C<cmd> contains shell
metacharacters, it will executed by C<sh -c> and C<argv> must be C<null>.
This provides some protection from unintentionally invoking C<sh -c>. If
C<cmd> does not contain any shell metacharacters, but does contain a slash
character (C</>), it is passed directly to I<execve()>. If it doesn't
contain a slash character, we search for the executable in the directories
specified by the C<PATH> variable. If the C<PATH> variable is not set, a
default path is used: C</bin:/usr/bin> for root; C<:/bin:/usr/bin> for other
users. If permission is denied for a file (I<execve()> returns C<EACCES>),
then searching continues. If the header of a file isn't recognised
(I<execve()> returns C<ENOEXEC>), then C</bin/sh> will be executed with
C<cmd> as its first argument. This is done so that shell scripts without a
C<#!> line can be used. If this attempt fails, no further searching is done.
Communication with the coprocess occurs over pipes. Data written to C<*to>
can be read from the standard input of the coprocess. Data written to the
standard output or standard error of the coprocess may be read from
C<*from>. On success, returns the process id of the coprocess. On error,
returns C<-1> with C<errno> set appropriately.

Note: That this can only be used with coprocesses that do not buffer I/O or
that explicitly set line buffering with I<setbuf()> or I<setvbuf()>. If a
potential coprocess uses standard I/O and you don't have access to the
source code, you will need to use I<coproc_pty_open()> instead.

B<Note: If cmd does contain shell metacharacters, make sure that the program
provides the command to execute. If the command comes from outside the
program, do not trust it. Verify that it is safe to execute.>

=cut

*/

static char * const *new_shargv(const char *cmd, char * const *argv)
{
	char **shargv;
	int nargs = 0;

	while (argv[nargs])
		++nargs;

	if (!(shargv = malloc((nargs + 2) * sizeof(char **))))
		return NULL;

	shargv[0] = "/bin/sh";
	shargv[1] = (char *)cmd;

	for (nargs = 1; argv[nargs]; ++nargs)
		shargv[nargs + 1] = argv[nargs];

	shargv[nargs + 1] = NULL;

	return (char * const *)shargv;
}

static void do_exec(int has_meta, const char *cmd, char * const *argv, char * const *envv)
{
	if (has_meta)
	{
		char const *shargv[4];

		shargv[0] = "sh";
		shargv[1] = "-c";
		shargv[2] = cmd;
		shargv[3] = NULL;

		execve("/bin/sh", (char * const *)shargv, (envv) ? envv : environ);
	}
	else if (strchr(cmd, PATH_SEP))
	{
		execve(cmd, argv, (envv) ? envv : environ);

		if (errno == ENOEXEC)
		{
			char * const *shargv = new_shargv(cmd, argv);
			execve("/bin/sh", shargv, (envv) ? envv : environ);
			free((void *)shargv);
		}
	}
	else
	{
		char *path, *s, *f;
		char cmdbuf[512];

		if (!(path = getenv("PATH")))
			path = geteuid() ? DEFAULT_USER_PATH : DEFAULT_ROOT_PATH;

		for (s = path; s; s = (*f) ? f + 1 : NULL)
		{
			if (!(f = strchr(s, PATH_LIST_SEP)))
				f = s + strlen(s);

			if (snprintf(cmdbuf, 512, "%.*s%s%s", f - s, s, (f - s) ? PATH_SEP_STR : "", cmd) >= 512)
				continue;

			if (execve(cmdbuf, argv, (envv) ? envv : environ) == -1)
			{
				if (errno == EACCES)
					continue;

				if (errno == ENOEXEC)
				{
					char * const *shargv = new_shargv(cmdbuf, argv);
					execve("/bin/sh", shargv, (envv) ? envv : environ);
					free((void *)shargv);
					break;
				}
			}
		}
	}
}

pid_t coproc_open(int *to, int *from, const char *cmd, char * const *argv, char * const *envv)
{
	int to_pipe[2];   /* pipe for writing to the coprocess */
	int from_pipe[2]; /* pipe for reading from the coprocess */
	pid_t pid;        /* process id of the coprocess */
	int has_meta;     /* does cmd contain shell meta characters? */

	/* Check arguments */

	if (!to || !from || !cmd)
		return set_errno(EINVAL);

	has_meta = (cmd[strcspn(cmd, SHELL_META_CHARACTERS)] != '\0');

	if ((has_meta && argv) || (!has_meta && !argv))
		return set_errno(EINVAL);

	/* Create pipes */

	if (pipe(to_pipe) == -1)
		return -1;

	if (pipe(from_pipe) == -1)
	{
		close(to_pipe[RD]);
		close(to_pipe[WR]);
		return -1;
	}

	/* Create child process */

	switch (pid = fork())
	{
		case -1:
		{
			close(to_pipe[RD]);
			close(to_pipe[WR]);
			close(from_pipe[RD]);
			close(from_pipe[WR]);
			return -1;
		}

		case 0:
		{
			/* Attach pipes to stdin, stdout and stderr */

			close(to_pipe[WR]);
			close(from_pipe[RD]);

			if (to_pipe[RD] != STDIN_FILENO)
			{
				if (dup2(to_pipe[RD], STDIN_FILENO) == -1)
					_exit(1);

				close(to_pipe[RD]);
			}

			if (from_pipe[WR] != STDOUT_FILENO)
			{
				if (dup2(from_pipe[WR], STDOUT_FILENO) == -1)
					_exit(1);

				close(from_pipe[WR]);
			}

			if (dup2(STDOUT_FILENO, STDERR_FILENO) == -1)
				_exit(1);

			/* Execute co-process */

			do_exec(has_meta, cmd, argv, envv);
			_exit(EXIT_FAILURE);
		}

		default:
		{
			/* Return the pipe descriptors and the coprocess id to the caller */

			close(to_pipe[RD]);
			close(from_pipe[WR]);

			*to = to_pipe[WR];
			*from = from_pipe[RD];

			return pid;
		}
	}
}

/*

=item C<int coproc_close(pid_t pid, int *to, int *from)>

Closes the coprocess referred to by C<pid> which must have been obtained
from I<coproc_open()>. C<*to> will be closed and set to C<-1> if it is not
already C<-1>. C<*from> will be closed and set to C<-1> if it is not already
C<-1>. The current process will then wait for the coprocess to terminate by
calling I<waitpid()>. On success, returns the status of the child process as
determined by I<waitpid()>. On error, returns C<-1> with C<errno> set
appropriately.

=cut

*/

int coproc_close(pid_t pid, int *to, int *from)
{
	int status = 0;

	if (to && *to != -1)
	{
		close(*to);
		*to = -1;
	}

	if (from && *from != -1)
	{
		close(*from);
		*from = -1;
	}

	if (pid != -1 && waitpid(pid, &status, 0) == -1)
		return -1;

	return status;
}

/*

=item C<pid_t coproc_pty_open(int *masterfd, char *slavename, size_t slavenamesize, const struct termios *slave_termios, const struct winsize *slave_winsize, const char *cmd, char * const *argv, char * const *envv)>

Equivalent to I<coproc_open()> except that communication with the coprocess
occurs over a pseudo terminal. This is useful when the coprocess uses
standard I/O and you don't have the source code. Standard I/O is fully
buffered unless connected to a terminal. C<*masterfd> is set to the master
side of a pseudo terminal. Data written to C<*masterfd> can be read from the
standard input of the coprocess. Data written to the standard output or
error of the coprocess can be read from C<*masterfd>. The device name of the
slave side of the pseudo terminal is stored in the buffer pointed to by
C<slavename> which must be able to store at least 64 bytes. C<slavenamesize>
is the size of the buffer pointed to by C<slavename>. No more than
C<slavenamesize> bytes will be written into the buffer pointed to by
C<slavename> including the terminating C<nul> byte. If C<slave_termios> is
not null, it is passed to I<tcsetattr()> with the command C<TCSANOW> to set
the terminal attributes of the slave device.  If C<slave_winsize> is not
null, it is passed to I<ioctl()> with the command C<TIOCSWINSZ> to set the
window size of the slave device. On success, returns C<0>. On error, returns
C<-1> with C<errno> set appropriately.

=cut

*/

pid_t coproc_pty_open(int *masterfd, char *slavename, size_t slavenamesize, const struct termios *slave_termios, const struct winsize *slave_winsize, const char *cmd, char * const *argv, char * const *envv)
{
	pid_t pid;        /* process id of the coprocess */
	int has_meta;     /* does cmd contain shell meta characters? */

	/* Check arguments */

	if (!masterfd || !slavename || slavenamesize < 64 || !cmd)
		return set_errno(EINVAL);

	has_meta = (cmd[strcspn(cmd, SHELL_META_CHARACTERS)] != '\0');

	if ((has_meta && argv) || (!has_meta && !argv))
		return set_errno(EINVAL);

	/* Create pty and child process */

	switch (pid = pty_fork(masterfd, slavename, slavenamesize, slave_termios, slave_winsize))
	{
		case -1:
			return -1;

		case 0:
			do_exec(has_meta, cmd, argv, envv);
			_exit(EXIT_FAILURE);

		default:
			return pid;
	}
}

/*

=item C<int coproc_pty_close(pid_t pid, int *masterfd, const char *slavename)>

Closes the coprocess referred to by C<pid> which must have been obtained
from I<coproc_pty_open()>. The slave side of the pseudo terminal is released
with I<pty_release()> and C<*masterfd> is closed and set to C<-1> if it is
not already C<-1>. The current process will then wait for the coprocess to
terminate by calling I<waitpid()>. On success, returns the status of the
child process as determined by I<waitpid()>. On error, returns C<-1> with
C<errno> set appropriately.

=cut

*/

int coproc_pty_close(pid_t pid, int *masterfd, const char *slavename)
{
	int status = 0;

	if (masterfd && *masterfd != -1)
	{
		pty_release(slavename);
		close(*masterfd);
		*masterfd = -1;
	}

	if (pid != -1 && waitpid(pid, &status, 0) == -1)
		return -1;

	return status;
}

/*

=back

=head1 ERRORS

Additional errors may be generated and returned from the underlying system calls.
See their manual pages.

=over 4

=item EINVAL

Invalid arguments were passed to I<coproc_open()> or I<coproc_pty_open()>.

=back

=head1 MT-Level

MT-Safe (I<coproc_pty_open()> is MT-Safe iff the I<pseudo> module is MT-Safe).

=head1 SEE ALSO

L<libslack(3)|libslack(3)>,
L<execve(2)|execve(2)>,
L<system(3)|system(3)>,
L<popen(3)|popen(3)>,
L<waitpid(2)|waitpid(2)>,
L<sh(1)|sh(1)>,
L<pseudo(3)|pseudo(3>>

=head1 AUTHOR

20011109 raf <raf@raf.org>

=cut

*/

#endif

#ifdef TEST

#include <fcntl.h>

#include "fio.h"

int cwd_in_path()
{
	const char *path = getenv("PATH");
	const char *s, *r;

	if (!path)
		return 1;

	for (r = path, s = strchr(path, PATH_LIST_SEP); s; r = s, s = strchr(s + 1, PATH_LIST_SEP))
		if ((r == s) || (s - r == 1 && r[0] == '.') || (s - r == 2 && r[0] == ':' && r[1] == '.'))
			return 1;

	if (r[0] == '\0' || (r[0] == ':' && r[1] == '\0') || (r[0] == ':' && r[1] == '.' && r[2] == '\0'))
		return 1;

	return 0;
}

int main()
{
	int errors = 0;
	int to, from, masterfd;
	int status;
	pid_t pid;
	int fd;
	char *argv[2] = { "cat", NULL };
	char *argv2[5] = { "arkleseizure", "a", "b", "c", NULL };
	char buf[BUFSIZ];
	char slavename[64];
	size_t bytes;

	printf("Testing: coproc\n");

	/* Test coproc_open("cat") - searches path, locating binary executable */

	if ((pid = coproc_open(&to, &from, "cat", argv, NULL)) == -1)
		++errors, printf("Test1: coproc_open(\"cat\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(to, 5, 0) == -1 || write(to, "abc\n", 4) != 4)
			++errors, printf("Test2: write_timeout(to) or write(to, \"abc\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "def\n", 4) != 4)
			++errors, printf("Test3: write_timeout(to) or write(to, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "ghi\n", 4) != 4)
			++errors, printf("Test4: write_timeout(to) or write(to, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else
		{
			close(to);
			to = -1;

			if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test5: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test6: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "abc\n", 4))
				++errors, printf("Test7: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "abc\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test8: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test9: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "def\n", 4))
				++errors, printf("Test10: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "def\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test11: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test12: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "ghi\n", 4))
				++errors, printf("Test13: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "ghi\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test14: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 0)
				++errors, printf("Test15: read(from) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));

			if ((status = coproc_close(pid, &to, &from)) == -1)
				++errors, printf("Test16: coproc_close() failed (%s)\n", strerror(errno));
			else if (WIFSIGNALED(status))
				++errors, printf("Test17: coproc received signal %d\n", WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				++errors, printf("Test18: coproc returned %d\n", WEXITSTATUS(status));
		}
	}

	/* Test coproc_open("/bin/cat") - does not search path */

	if ((pid = coproc_open(&to, &from, "/bin/cat", argv, NULL)) == -1)
		++errors, printf("Test19: coproc_open(\"/bin/cat\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(to, 5, 0) == -1 || write(to, "abc\n", 4) != 4)
			++errors, printf("Test20: write_timeout(to) or write(to, \"abc\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "def\n", 4) != 4)
			++errors, printf("Test21: write_timeout(to) or write(to, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "ghi\n", 4) != 4)
			++errors, printf("Test22: write_timeout(to) or write(to, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else
		{
			close(to);
			to = -1;

			if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test23: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test24: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "abc\n", 4))
				++errors, printf("Test25: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "abc\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test26: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test27: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "def\n", 4))
				++errors, printf("Test28: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "def\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test29: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test30: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "ghi\n", 4))
				++errors, printf("Test31: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "ghi\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test32: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 0)
				++errors, printf("Test33: read(from) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));

			if ((status = coproc_close(pid, &to, &from)) == -1)
				++errors, printf("Test34: coproc_close() failed (%s)\n", strerror(errno));
			else if (WIFSIGNALED(status))
				++errors, printf("Test35: coproc received signal %d\n", WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				++errors, printf("Test36: coproc returned %d\n", WEXITSTATUS(status));
		}
	}

	/* Test coproc_open("cat | sort") - uses "sh -c cmd" to handle meta characters */

	if ((pid = coproc_open(&to, &from, "cat | sort", NULL, NULL)) == -1)
		++errors, printf("Test37: coproc_open(\"cat | sort\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(to, 5, 0) == -1 || write(to, "ghi\n", 4) != 4)
			++errors, printf("Test38: write_timeout(to) or write(to, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "def\n", 4) != 4)
			++errors, printf("Test39: write_timeout(to) or write(to, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (write_timeout(to, 5, 0) == -1 || write(to, "abc\n", 4) != 4)
			++errors, printf("Test40: write_timeout(to) or write(to, \"abc\\n\") failed (%s)\n", strerror(errno));
		else
		{
			close(to);
			to = -1;

			if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test41: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test42: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "abc\n", 4))
				++errors, printf("Test43: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "abc\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test44: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test45: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "def\n", 4))
				++errors, printf("Test46: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "def\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test47: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 4)
				++errors, printf("Test48: read(from) failed (returned %d, not %d) (%s)\n", bytes, 4, strerror(errno));
			else if (memcmp(buf, "ghi\n", 4))
				++errors, printf("Test49: read(from) failed (read \"%.4s\", not \"%.4s\")\n", buf, "ghi\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test50: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, 4)) != 0)
				++errors, printf("Test51: read(from) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));

			if ((status = coproc_close(pid, &to, &from)) == -1)
				++errors, printf("Test52: coproc_close() failed (%s)\n", strerror(errno));
			else if (WIFSIGNALED(status))
				++errors, printf("Test53: coproc received signal %d\n", WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				++errors, printf("Test54: coproc returned %d\n", WEXITSTATUS(status));
		}
	}

	/* Test coproc_open() - path search of sh script without #! line (cwd must be in $PATH) */

	if (cwd_in_path())
	{
		if ((fd = open("arkleseizure", O_WRONLY | O_CREAT, 0700)) == -1 || write(fd, "echo $*\n", 8) != 8 || close(fd) == -1)
			++errors, printf("Test55: failed to perform test: open(arkleseizure) failed\n");
		else if ((pid = coproc_open(&to, &from, "arkleseizure", argv2, NULL)) == -1)
			++errors, printf("Test56: coproc_open(\"arkleseizure a b c\") failed (%s)\nIs the current directory in $PATH?\n", strerror(errno));
		else
		{
			close(to);
			to = -1;

			if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test57: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, BUFSIZ)) != 6)
				++errors, printf("Test58: read(from) failed (returned %d, not %d) (%s)\n", bytes, 6, strerror(errno));
			else if (memcmp(buf, "a b c\n", 6))
				++errors, printf("Test59: read(from) failed (read \"%.6s\", not \"%.6s\")\n", buf, "a b c\n");
			else if (read_timeout(from, 5, 0) == -1)
				++errors, printf("Test60: read_timeout(from) failed (%s)\n", strerror(errno));
			else if ((bytes = read(from, buf, BUFSIZ)) != 0)
				++errors, printf("Test61: read(from) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));

			if ((status = coproc_close(pid, &to, &from)) == -1)
				++errors, printf("Test62: coproc_close() failed (%s)\n", strerror(errno));
			else if (WIFSIGNALED(status))
				++errors, printf("Test63: coproc received signal %d\n", WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status))
				++errors, printf("Test64: coproc returned %d\n", WEXITSTATUS(status));
		}

		unlink("arkleseizure");
	}

	/* Test coproc_open() - sh script without #! line without path search */

	if ((fd = open("arkleseizure", O_WRONLY | O_CREAT, 0700)) == -1 || write(fd, "echo $*\n", 8) != 8 || close(fd) == -1)
		++errors, printf("Test65: failed to perform test: open(arkleseizure) failed\n");
	else if ((pid = coproc_open(&to, &from, "./arkleseizure", argv2, NULL)) == -1)
		++errors, printf("Test66: coproc_open(\"arkleseizure a b c\") failed (%s)\n", strerror(errno));
	else
	{
		close(to);
		to = -1;

		if (read_timeout(from, 5, 0) == -1)
			++errors, printf("Test67: read_timeout(from) failed (%s)\n", strerror(errno));
		else if ((bytes = read(from, buf, BUFSIZ)) != 6)
			++errors, printf("Test68: read(from) failed (returned %d, not %d) (%s)\n", bytes, 6, strerror(errno));
		else if (memcmp(buf, "a b c\n", 6))
			++errors, printf("Test69: read(from) failed (read \"%.6s\", not \"%.6s\")\n", buf, "a b c\n");
		else if (read_timeout(from, 5, 0) == -1)
			++errors, printf("Test70: read_timeout(from) failed (%s)\n", strerror(errno));
		else if ((bytes = read(from, buf, BUFSIZ)) != 0)
			++errors, printf("Test71: read(from) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));

		if ((status = coproc_close(pid, &to, &from)) == -1)
			++errors, printf("Test72: coproc_close() failed (%s)\n", strerror(errno));
		else if (WIFSIGNALED(status))
			++errors, printf("Test73: coproc received signal %d\n", WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			++errors, printf("Test74: coproc returned %d\n", WEXITSTATUS(status));
	}

	unlink("arkleseizure");

	/* Test coproc_open() error reporting */

	if (coproc_open(NULL, &from, "cmd", argv, NULL) != -1)
		++errors, printf("Test75: coproc_open(to == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test76: coproc_open(to == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_open(&to, NULL, "cmd", argv, NULL) != -1)
		++errors, printf("Test77: coproc_open(from == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test78: coproc_open(from == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_open(&to, &from, NULL, argv, NULL) != -1)
		++errors, printf("Test79: coproc_open(cmd == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test80: coproc_open(cmd == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_open(&to, &from, "cmd", NULL, NULL) != -1)
		++errors, printf("Test81: coproc_open(cmd has no meta but argv is null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test82: coproc_open(cmd has no meta but argv is null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_open(&to, &from, "cmd || cmd", argv, NULL) != -1)
		++errors, printf("Test83: coproc_open(cmd has meta and argv is not null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test84: coproc_open(cmd has meta but argv is not null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	/* Test coproc_pty_open("cat") - searches path, locating binary executable */

	if ((pid = coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "cat", argv, NULL)) == -1)
		++errors, printf("Test85: coproc_pty_open(\"cat\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "abc\n", 4) != 4)
			++errors, printf("Test86: write_timeout(masterfd) or write(masterfd, \"abc\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test87: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test88: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "abc", 3))
			++errors, printf("Test89: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "abc");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "def\n", 4) != 4)
			++errors, printf("Test90: write_timeout(masterfd) or write(masterfd, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test91: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test92: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "def", 3))
			++errors, printf("Test93: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "def");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "ghi\n", 4) != 4)
			++errors, printf("Test94: write_timeout(masterfd) or write(masterfd, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test95: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test96: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "ghi", 3))
			++errors, printf("Test97: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "ghi");

		if ((status = coproc_pty_close(pid, &masterfd, slavename)) == -1)
			++errors, printf("Test98: coproc_pty_close() failed (%s)\n", strerror(errno));
		else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGHUP)
			++errors, printf("Test99: coproc received signal %d\n", WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			++errors, printf("Test100: coproc returned %d\n", WEXITSTATUS(status));
	}

	/* Test coproc_pty_open("/bin/cat") - does not search path */

	if ((pid = coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "/bin/cat", argv, NULL)) == -1)
		++errors, printf("Test101: coproc_pty_open(\"/bin/cat\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "abc\n", 4) != 4)
			++errors, printf("Test102: write_timeout(masterfd) or write(masterfd, \"abc\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test103: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test104: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "abc", 3))
			++errors, printf("Test105: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "abc");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "def\n", 4) != 4)
			++errors, printf("Test106: write_timeout(masterfd) or write(masterfd, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test107: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test108: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "def", 3))
			++errors, printf("Test109: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "def");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "ghi\n", 4) != 4)
			++errors, printf("Test110: write_timeout(masterfd) or write(masterfd, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test111: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test112: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "ghi", 3))
			++errors, printf("Test113: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "ghi");

		if ((status = coproc_pty_close(pid, &masterfd, slavename)) == -1)
			++errors, printf("Test114: coproc_pty_close() failed (%s)\n", strerror(errno));
		else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGHUP)
			++errors, printf("Test115: coproc received signal %d\n", WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			++errors, printf("Test116: coproc returned %d\n", WEXITSTATUS(status));
	}

	/* Test coproc_pty_open("cat | cat") - uses "sh -c cmd" to handle meta characters */

	if ((pid = coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "cat | cat", NULL, NULL)) == -1)
		++errors, printf("Test117: coproc_pty_open(\"cat | cat\") failed (%s)\n", strerror(errno));
	else
	{
		if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "abc\n", 4) != 4)
			++errors, printf("Test118: write_timeout(masterfd) or write(masterfd, \"abc\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test119: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test120: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "abc", 3))
			++errors, printf("Test121: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "abc");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "def\n", 4) != 4)
			++errors, printf("Test122: write_timeout(masterfd) or write(masterfd, \"def\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test123: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test124: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "def", 3))
			++errors, printf("Test125: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "def");
		else if (write_timeout(masterfd, 5, 0) == -1 || write(masterfd, "ghi\n", 4) != 4)
			++errors, printf("Test126: write_timeout(masterfd) or write(masterfd, \"ghi\\n\") failed (%s)\n", strerror(errno));
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test127: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, 5)) != 5)
			++errors, printf("Test128: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 5, strerror(errno));
		else if (memcmp(buf, "ghi", 3))
			++errors, printf("Test129: read(masterfd) failed (read \"%.3s\", not \"%.3s\")\n", buf, "ghi");

		if ((status = coproc_pty_close(pid, &masterfd, slavename)) == -1)
			++errors, printf("Test130: coproc_pty_close() failed (%s)\n", strerror(errno));
		else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGHUP)
			++errors, printf("Test131: coproc received signal %d\n", WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status))
			++errors, printf("Test132: coproc returned %d\n", WEXITSTATUS(status));
	}

	/* Test coproc_pty_open() - path search of sh script without #! line (cwd must be in $PATH) */

	if (cwd_in_path())
	{
		if ((fd = open("arkleseizure", O_WRONLY | O_CREAT, 0700)) == -1 || write(fd, "echo $*\n", 8) != 8 || close(fd) == -1)
			++errors, printf("Test133: failed to perform test: open(arkleseizure) failed\n");
		else if ((pid = coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "arkleseizure", argv2, NULL)) == -1)
			++errors, printf("Test134: coproc_pty_open(\"arkleseizure a b c\") failed (%s)\nIs the current directory in $PATH?\n", strerror(errno));
		else
		{
			if (read_timeout(masterfd, 5, 0) == -1)
				++errors, printf("Test135: read_timeout(masterfd) failed (%s)\n", strerror(errno));
			else if ((bytes = read(masterfd, buf, BUFSIZ)) != 7)
				++errors, printf("Test136: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 7, strerror(errno));
			else if (memcmp(buf, "a b c", 5))
				++errors, printf("Test137: read(masterfd) failed (read \"%.5s\", not \"%.5s\")\n", buf, "a b c");
#ifndef linux
			else if (read_timeout(masterfd, 5, 0) == -1)
				++errors, printf("Test138: read_timeout(masterfd) failed (%s)\n", strerror(errno));
			else if ((bytes = read(masterfd, buf, BUFSIZ)) != 0)
				++errors, printf("Test139: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));
#endif

			if ((status = coproc_pty_close(pid, &masterfd, slavename)) == -1)
				++errors, printf("Test140: coproc_pty_close() failed (%s)\n", strerror(errno));
			else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGHUP)
				++errors, printf("Test141: coproc received signal %d\n", WTERMSIG(status));
			else if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 128 + SIGHUP)
				++errors, printf("Test142: coproc returned %d\n", WEXITSTATUS(status));
		}

		unlink("arkleseizure");
	}

	/* Test coproc_pty_open() - sh script without #! line without path search */

	if ((fd = open("arkleseizure", O_WRONLY | O_CREAT, 0700)) == -1 || write(fd, "echo $*\n", 8) != 8 || close(fd) == -1)
		++errors, printf("Test143: failed to perform test: open(arkleseizure) failed\n");
	else if ((pid = coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "./arkleseizure", argv2, NULL)) == -1)
		++errors, printf("Test144: coproc_pty_open(\"arkleseizure a b c\") failed (%s)\n", strerror(errno));
	else
	{
		if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test145: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, BUFSIZ)) != 7)
			++errors, printf("Test146: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 7, strerror(errno));
		else if (memcmp(buf, "a b c", 5))
			++errors, printf("Test147: read(masterfd) failed (read \"%.5s\", not \"%.5s\")\n", buf, "a b c");
#ifndef linux
		else if (read_timeout(masterfd, 5, 0) == -1)
			++errors, printf("Test148: read_timeout(masterfd) failed (%s)\n", strerror(errno));
		else if ((bytes = read(masterfd, buf, BUFSIZ)) != 0)
			++errors, printf("Test149: read(masterfd) failed (returned %d, not %d) (%s)\n", bytes, 0, strerror(errno));
#endif

		if ((status = coproc_pty_close(pid, &masterfd, slavename)) == -1)
			++errors, printf("Test150: coproc_pty_close() failed (%s)\n", strerror(errno));
		else if (WIFSIGNALED(status) && WTERMSIG(status) != SIGHUP)
			++errors, printf("Test151: coproc received signal %d\n", WTERMSIG(status));
		else if (WIFEXITED(status) && WEXITSTATUS(status) != 0 && WEXITSTATUS(status) != 128 + SIGHUP)
			++errors, printf("Test152: coproc returned %d\n", WEXITSTATUS(status));
	}

	unlink("arkleseizure");

	/* Test coproc_pty_open() error reporting */

	if (coproc_pty_open(NULL, slavename, 64, NULL, NULL, "cmd", argv, NULL) != -1)
		++errors, printf("Test153: coproc_pty_open(masterfd == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test154: coproc_pty_open(masterfd == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_pty_open(&masterfd, NULL, 64, NULL, NULL, "cmd", argv, NULL) != -1)
		++errors, printf("Test155: coproc_pty_open(slavename == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test156: coproc_pty_open(slavename == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_pty_open(&masterfd, slavename, 63, NULL, NULL, "cmd", argv, NULL) != -1)
		++errors, printf("Test157: coproc_pty_open(slavenamesize < 64) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test158: coproc_pty_open(slavenamesize < 64) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, NULL, argv, NULL) != -1)
		++errors, printf("Test159: coproc_pty_open(cmd == null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test160: coproc_pty_open(cmd == null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "cmd", NULL, NULL) != -1)
		++errors, printf("Test161: coproc_pty_open(cmd has no meta but argv is null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test162: coproc_pty_open(cmd has no meta but argv is null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (coproc_pty_open(&masterfd, slavename, 64, NULL, NULL, "cmd || cmd", argv, NULL) != -1)
		++errors, printf("Test163: coproc_pty_open(cmd has meta and argv is not null) failed\n");
	else if (errno != EINVAL)
		++errors, printf("Test164: coproc_pty_open(cmd has meta but argv is not null) failed (errno == %s, not %s\n", strerror(errno), strerror(EINVAL));

	if (errors)
		printf("%d/%d tests failed\n", errors, 164);
	else
		printf("All tests passed\n");

	if (!cwd_in_path())
	{
		printf("\n");
		printf("    Note: Can't perform the path search tests.\n");
		printf("    Audit the code and rerun the test with \".\" in $PATH.\n");
	}

	return EXIT_SUCCESS;
}

#endif

/* vi:set ts=4 sw=4: */
