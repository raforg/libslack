#!/usr/bin/perl -w
use strict;

# Compares the contents of each module's header file against the synposis
# section of the corresponding manpage which should contain all typedefs
# and function prototypes that appear in the header file. All differences
# are reported.

die("usage: $0 *.c\n") if $#ARGV == -1;

my @src;
my @hdr;

for my $src (@ARGV)
{
	next unless $src =~ /\.c$/;
	next if $src eq 'getopt.c';
	my $hdr = $src;
	$hdr =~ s/\.c$/.h/;

	my $src_state = '';
	my $hdr_state = '';

	@src = ();
	@hdr = ();

	my $done_rwlock_skip = 0;

	open(SRC, $src) or die("failed to open $src\n");

	while (<SRC>)
	{
		next if $_ =~ /^$/;
		next if $_ =~ /^\s+#/;
		$src_state = $1, next if $_ =~ /^=head1 (.*)$/;
		last if $src_state eq 'DESCRIPTION';

		if ($src_state eq 'SYNOPSIS')
		{
			my $line = $_;
			$line =~ s/^    //;
			chop($line);
			push(@src, $line);
		}
	}

	close(SRC);

	open(HDR, $hdr) or die("failed to open $hdr\n");

	while (<HDR>)
	{
		if ($_ =~ /^#ifndef HAVE_PTHREAD_RWLOCK/ && $done_rwlock_skip == 0)
		{
			my $jnk;
			do { $jnk = <HDR>; } while $jnk !~ /^#endif$/;
			$done_rwlock_skip = 1;
		}

		next if $_ =~ /^$/;
		next if $_ =~ /^#/;
		next if $_ =~ /^\//;
		next if $_ =~ /^ ?\*/;

		$hdr_state = 'decls', next if $_ =~ /^_begin_decls$/;
		last if $_ =~ /^_end_decls$/;

		if ($hdr_state eq '')
		{
			my $line = $_;
			chop($line);
			$line =~ s/	/    /g;
			$line =~ s/\/\*.*\*\/$//;
			$line =~ s/\s+$//;
			push(@hdr, $line);
		}

		if ($hdr_state eq 'decls')
		{
			my $line = $_;
			$line =~ s/ _args \(//;
			$line =~ s/\)\);/);/;
			chop($line);
			push(@hdr, $line);
		}
	}

	close(HDR);

	show("$src != $hdr (line number mismatch)"), next if $#src != $#hdr;

	my $i;
	my $first = 1;
	for ($i = 0; $i <= $#src; ++$i)
	{
		if ($src[$i] ne $hdr[$i])
		{
			print("$src != $hdr\n") if $first;
			print("syn = $src[$i]\n");
			print("hdr = $hdr[$i]\n");
			$first = 0;
		}
	}

	print("\n") unless $first;
}

sub show
{
	my ($msg) = @_;

	print("$msg\n");
	print("-- syn ---\n");
	my $i;
	for ($i = 0; $i <= $#src; ++$i)
	{
		print("$src[$i]\n");
	}

	print("-- hdr ---\n");

	for ($i = 0; $i <= $#hdr; ++$i)
	{
		print("$hdr[$i]\n");
	}

	print("----------\n\n");
}

# vi set ts=4 sw=4
