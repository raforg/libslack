/*
* libslack - http://libslack.org/
*
* Copyright (C) 1999, 2000 raf <raf@raf.org>
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
* 20000902 raf <raf@raf.org>
*/

/*

=head1 NAME

I<vsscanf()> - I<sscanf()> with a I<va_list> parameter

=head1 SYNOPSIS

    #include <slack/vsscanf.h>

    int vsscanf(const char *str, const char *fmt, va_list args);

=head1 DESCRIPTION

Similar to I<sscanf(3)> with the variable argument list specified directly
as for I<vprintf(3)>.

Note that this may not be identical in behaviour to the I<sscanf(3)> on your
system because this was implemented from scratch for systems that lack
I<vsscanf()>. So your I<sscanf(3)> and this I<vsscanf()> share no common
code. Your I<sscanf(3)> may support extensions that I<vsscanf()> does not
support. I<vsscanf()> complies with most of the ANSI C requirements for
I<sscanf()> except:

=over 4

=item *

C<fmt> may not be a multibyte character string;

=item *

The current locale is ignored; and

=item *

Scanning a pointer (C<"%p">) may not exactly match the format that your
I<printf(3)> uses to print pointers on your system.

=back

=head1 BUGS

Does not support multibyte C<fmt> string.

The current locale is ignored.

Scanning C<"%p"> might not exactly match the format used by I<printf(3)> on
your system.

=head1 NOTE

I<gcc(1)> warns that:

    warning: ANSI C does not support the `L' length modifier
    warning: use of `l' length character with `e' type character

However, the ANSI C standard (Section 7.9.6.2) states that:

"Finally, the conversion specifiers C<e>, C<f>, and C<g> shall be preceeded
by C<l> if the corresponding argument is a pointer to C<double> rather than
a pointer to C<float>, or by C<L> if it is a pointer to C<long double>."

I have chosen to disregard the I<gcc(1)> warnings in favour of the standard.
If you see the above warnings when compiling the unit tests for
I<vsscanf()>, just ignore them.

=head1 SEE ALSO

L<sscanf(3)|sscanf(3)>

=head1 AUTHOR

20000902 raf <raf@raf.org>

=cut

*/

#include "std.h"

int vsscanf(const char *str, const char *fmt, va_list args)
{
	const char *f, *s;
	int cnv = 0;

	for (s = str, f = fmt; *f; ++f)
	{
		if (*f == '%')
		{
			int size = 0;
			int width = 0;
			int do_cnv = 1;

			if (*++f == '*')
				++f, do_cnv = 0;

			for (; isdigit((int)*f); ++f)
				width *= 10, width += *f - '0';

			if (*f == 'h' || *f == 'l' || *f == 'L')
				size = *f++;

			if (*f != '[' && *f != 'c' && *f != 'n')
				while (isspace((int)*s))
					++s;

#define COPY                         *b++ = *s++, --width
#define MATCH(cond)                  if (width && (cond)) COPY;
#define MATCH_ACTION(cond, action)   if (width && (cond)) { COPY; action; }
#define MATCHES_ACTION(cond, action) while (width && (cond)) { COPY; action; }
#define FAIL                         (cnv) ? cnv : EOF
			switch (*f)
			{
				case 'd': case 'i': case 'o': case 'u': case 'x': case 'X':
				case 'p':
				{
					static char types[] = "diouxXp";
					static int bases[] = { 10, 0, 8, 10, 16, 16, 16 };
					static char digitset[] = "0123456789abcdefABCDEF";
					static int setsizes[] = { 10, 0, 0, 0, 0, 0, 0, 0, 8, 0, 10, 0, 0, 0, 0, 0, 22 };
					int base = bases[strchr(types, *f) - types];
					int setsize;
					char buf[513];
					char *b = buf;
					int digit = 0;
					if (width <= 0 || width > 512) width = 512;
					MATCH(*s == '+' || *s == '-')
					MATCH_ACTION(*s == '0',
						digit = 1;
						MATCH_ACTION((*s == 'x' || *s == 'X') && (base == 0 || base == 16), base = 16) else base = 8;
					)
					setsize = setsizes[base];
					MATCHES_ACTION(memchr(digitset, *s, setsize), digit = 1)
					if (!digit) return FAIL;
					*b = '\0';
					if (do_cnv)
					{
						if (*f == 'd' || *f == 'i')
						{
							long data = strtol(buf, NULL, base);
							if (size == 'h')
								*va_arg(args, short *) = (short)data;
							else if (size == 'l')
								*va_arg(args, long *) = data;
							else
								*va_arg(args, int *) = (int)data;
						}
						else
						{
							unsigned long data = strtoul(buf, NULL, base);
							if (size == 'p')
								*va_arg(args, void **) = (void *)data;
							else if (size == 'h')
								*va_arg(args, unsigned short *) = (unsigned short)data;
							else if (size == 'l')
								*va_arg(args, unsigned long *) = data;
							else
								*va_arg(args, unsigned int *) = (unsigned int)data;
						}
						++cnv;
					}
					break;
				}

				case 'e': case 'E': case 'f': case 'g': case 'G':
				{
					char buf[513];
					char *b = buf;
					int digit = 0;
					if (width <= 0 || width > 512) width = 512;
					MATCH(*s == '+' || *s == '-')
					MATCHES_ACTION(isdigit((int)*s), digit = 1)
					MATCH(*s == '.')
					MATCHES_ACTION(isdigit((int)*s), digit = 1)
					MATCHES_ACTION(digit && (*s == 'e' || *s == 'E'),
						MATCH(*s == '+' || *s == '-')
						digit = 0;
						MATCHES_ACTION(isdigit((int)*s), digit = 1)
					)
					if (!digit) return FAIL;
					*b = '\0';
					if (do_cnv)
					{
						double data = strtod(buf, NULL);
						if (size == 'l')
							*va_arg(args, double *) = data;
						else if (size == 'L')
							*va_arg(args, long double *) = (long double)data;
						else
							*va_arg(args, float *) = (float)data;
						++cnv;
					}
					break;
				}

				case 's':
				{
					char *arg = va_arg(args, char *);
					if (width <= 0) width = INT_MAX;
					while (width-- && *s && !isspace((int)*s))
						if (do_cnv) *arg++ = *s++;
					if (do_cnv) *arg = '\0', ++cnv;
					break;
				}

				case '[':
				{
					char *arg = va_arg(args, char *);
					int setcomp = 0;
					size_t setsize;
					const char *end;
					if (width <= 0) width = INT_MAX;
					if (*++f == '^') setcomp = 1, ++f;
					end = strchr((*f == ']') ? f + 1 : f, ']');
					if (!end) return FAIL;
					setsize = end - f;
					while (width-- && *s)
					{
						if (!setcomp && !memchr(f, *s, setsize)) break;
						if (setcomp && memchr(f, *s, setsize)) break;
						if (do_cnv) *arg++ = *s++;
					}
					if (do_cnv) *arg = '\0', ++cnv;
					f = end;
					break;
				}

				case 'c':
				{
					char *arg = va_arg(args, char *);
					if (width <= 0) width = 1;
					while (width--)
					{
						if (!*s) return FAIL;
						if (do_cnv) *arg++ = *s++;
					}
					if (do_cnv) ++cnv;
					break;
				}

				case 'n':
				{
					if (size == 'h')
						*va_arg(args, short *) = (short)(s - str);
					else if (size == 'l')
						*va_arg(args, long *) = (long)(s - str);
					else
						*va_arg(args, int *) = (int)(s - str);
					break;
				}

				case '%':
				{
					if (*s++ != '%') return cnv;
					break;
				}

				default:
					return FAIL;
			}
		}
		else if (isspace((int)*f))
		{
			while (isspace((int)f[1]))
				++f;
			while (isspace((int)*s))
				++s;
		}
		else
		{
			if (*s++ != *f)
				return cnv;
		}
	}

	return cnv;
}

#ifdef TEST

#undef _ISOC9X_SOURCE
#undef __USE_ISOC9X
#include <math.h>
#include <float.h>

int test_sscanf(const char *str, const char *fmt, ...)
{
	int rc;
	va_list args;
	va_start(args, fmt);
	rc = vsscanf(str, fmt, args);
	va_end(args);
	return rc;
}

int main(int ac, char **av)
{
	int errors = 0;
	short si1, si2;
	int i1, i2;
	long li1, li2;
	float f1, f2;
	double d1, d2;
	long double ld1, ld2;
	void *p1, *p2;
	char b1[128], b2[128];
	char c1[128], c2[128];
	char s1[128], s2[128];
	short sn1, sn2;
	int in1, in2;
	long ln1, ln2;
	unsigned short su1, su2;
	unsigned int u1, u2;
	unsigned long lu1, lu2;
	char str[512];
	int rc1, rc2;

	printf("Testing: vsscanf\n");

	sprintf(str, " abc -12 37 101 3.4e-1 12.34 102.23 xyz %p def ghi jkl %% ",
		p1 = (void *)0xdeadbeef
	);

	rc1 = sscanf(str,
		" abc %hd %d %ld %e %le %Le xyz %p %[^abc ] %3c %s%hn %n%% %ln",
		&si1, &i1, &li1, &f1, &d1, &ld1, &p1, b1, c1, s1, &sn1, &in1, &ln1
	);

	rc2 = test_sscanf(str,
		" abc %hd %d %ld %e %le %Le xyz %p %[^abc ] %3c %s%hn %n%% %ln",
		&si2, &i2, &li2, &f2, &d2, &ld2, &p2, b2, c2, s2, &sn2, &in2, &ln2
	);

	if (rc1 != rc2)
		++errors, printf("Test1: failed (returned %d, not %d)\n", rc2, rc1);
	if (si1 != si2)
		++errors, printf("Test2: failed (%%hd scanned %hd, not %hd)\n", si2, si1);
	if (i1 != i2)
		++errors, printf("Test3: failed (%%d scanned %d, not %d)\n", i2, i1);
	if (li1 != li2)
		++errors, printf("Test4: failed (%%ld scanned %ld, not %ld)\n", li2, li1);
	if (fabs(f2 - 3.4e-1) / 3.4e-1 >= 4 * FLT_EPSILON)
		++errors, printf("Test5: failed (%%e scanned %e, not %e)\n", f2, f1);
	if (fabs(d2 - 12.34) / 12.34 >= 4 * DBL_EPSILON)
		++errors, printf("Test6: failed (%%le scanned %le, not %le)\n", d2, d1);
	if (fabs(ld2 - 102.23) / 102.23 >= 4 * LDBL_EPSILON)
		++errors, printf("Test7: failed (%%Le scanned %Le, not %Le)\n", ld2, ld1);
	if (p1 != p2)
		++errors, printf("Test8: failed (%%p scanned %p, not %p)\n", p2, p1);
	if (strcmp(b1, b2))
		++errors, printf("Test9: failed (%%[^abc ] scanned \"%s\", not \"%s\")\n", b2, b1);
	if (memcmp(c1, c2, 3))
		++errors, printf("Test10: failed (%%3c scanned \"%3.3s\", not \"%3.3s\")\n", c2, c1);
	if (strcmp(s1, s2))
		++errors, printf("Test11: failed (%%s scanned \"%s\", not \"%s\")\n", s2, s1);
	if (sn1 != sn2)
		++errors, printf("Test12: failed (%%hn scanned %hd, not %hd)\n", sn2, sn1);
	if (in1 != in2)
		++errors, printf("Test13: failed (%%n scanned %d, not %d)\n", in2, in1);
	if (ln1 != ln2)
		++errors, printf("Test14: failed (%%ln scanned %ld, not %ld)\n", ln2, ln1);

	/* Test different numeric bases */

#define TEST_NUM(i, var, tst, str, fmt) \
	rc1 = sscanf(str, fmt, &s##var##1, &var##1, &l##var##1); \
	rc2 = test_sscanf(str, fmt, &s##var##2, &var##2, &l##var##2); \
	if (rc1 != rc2) \
		++errors, printf("Test%d: failed (returned %d, not %d)\n", (i), rc2, rc1); \
	if (s##var##1 != s##var##2) \
		++errors, printf("Test%d: failed (%%h%c scanned %hd, not %hd)\n", (i), tst, s##var##2, s##var##1); \
	if (var##1 != var##2) \
		++errors, printf("Test%d: failed (%%%c scanned %d, not %d)\n", (i), tst, var##2, var##1); \
	if (l##var##1 != l##var##2) \
		++errors, printf("Test%d: failed (%%l%c scanned %ld, not %ld)\n", (i), tst, l##var##2, l##var##1)

#define TEST_STR(i, len, str, fmt) \
	rc1 = sscanf(str, fmt, b1, c1, s1); \
	rc2 = test_sscanf(str, fmt, b2, c2, s2); \
	if (rc1 != rc2) \
		++errors, printf("Test%d: failed (returned %d, not %d)\n", (i), rc2, rc1); \
	if (strcmp(b1, b2)) \
		++errors, printf("Test%d: failed (%%%d[ scanned \"%s\", not \"%s\")\n", (i), (len), b2, b1); \
	if (memcmp(c1, c2, len)) \
		++errors, printf("Test%d: failed (%%%dc scanned \"%*.*s\", not \"%*.*s\")\n", (i), (len), (len), (len), c2, (len), (len), c1); \
	if (strcmp(s1, s2)) \
		++errors, printf("Test%d: failed (%%%ds scanned \"%s\", not \"%s\")\n", (i), (len), s2, s1)

	TEST_NUM(15, i, 'i', "37 21 53", "%hi %i %li");
	TEST_NUM(16, i, 'i', "037 021 053", "%hi %i %li");
	TEST_NUM(17, i, 'i', "0x37 0x21 0x53", "%hi %i %li");
	TEST_NUM(18, u, 'o', "037 021 053", "%ho %o %lo");
	TEST_NUM(19, u, 'u', "37 21 53", "%hu %u %lu");
	TEST_NUM(20, u, 'x', "0x37 0x21 0x53", "%hx %x %lx");
	TEST_NUM(21, u, 'X', "0x37 0x21 0x53", "%hx %x %lx");

	/* Test field width handling */

	TEST_NUM(22, i, 'd', "123456789", "%3hd %2d %4ld");
	TEST_NUM(23, i, 'i', "123456789", "%3hi %2i %4li");
	TEST_NUM(24, i, 'i', "012340789", "%3hi %2i %4li");
	TEST_NUM(25, i, 'x', "123456789", "%3hi %2i %4li");
	TEST_NUM(26, u, 'o', "012340789", "%3ho %2o %4lo");
	TEST_NUM(27, u, 'u', "123456789", "%3hu %2u %4lu");
	TEST_NUM(28, u, 'x', "123456789", "%3hx %2x %4lx");
	TEST_NUM(29, u, 'X', "123456789", "%3hx %2X %4lX");
	TEST_STR(30, 1, "abcd", "%1[a]%c%1s");

	/* Test error reporting */

#define TEST_ERR(i, str, fmt) \
	rc1 = sscanf(str, fmt); \
	rc2 = test_sscanf(str, fmt); \
	if (rc1 != rc2) \
		++errors, printf("Test%d: failed (returned %d, not %d)\n", (i), rc2, rc1)

#define TEST_ERR_ARG(i, str, fmt, var) \
	rc1 = sscanf(str, fmt, &var##1); \
	rc2 = test_sscanf(str, fmt, &var##1); \
	if (rc1 != rc2) \
		++errors, printf("Test%d: failed (returned %d, not %d)\n", (i), rc2, rc1)

	TEST_ERR_ARG(31, "", "%d", i);
	TEST_ERR_ARG(32, "", "%i", i);
	TEST_ERR_ARG(33, "", "%o", u);
	TEST_ERR_ARG(34, "", "%u", u);
	TEST_ERR_ARG(35, "", "%x", u);
	TEST_ERR_ARG(36, "", "%X", u);
	TEST_ERR_ARG(37, "", "%p", p);
	TEST_ERR_ARG(38, "", "%e", f);
	TEST_ERR_ARG(39, "", "%E", f);
	TEST_ERR_ARG(40, "", "%f", f);
	TEST_ERR_ARG(41, "", "%g", f);
	TEST_ERR_ARG(42, "", "%G", f);
	TEST_ERR_ARG(43, "", "%[^]", *b);
	TEST_ERR_ARG(44, "", "%c", *c);
	TEST_ERR(45, "a", "%%");
	TEST_ERR(46, "a", "b");

	if (errors)
		printf("%d/46 tests failed\n", errors);
	else
		printf("All tests passed\n");

	return 0;
}

#endif

/* vi:set ts=4 sw=4: */
