#!/usr/bin/perl -w
use strict;

# Compares the function prototypes in the pod manpage source against the
# actual function header itself. All differences are reported.

die("usage: $0 *.c\n") if $#ARGV == -1;

my $state = 0;
my $doc;
my $src;
my $jnk;
my $line = 0;

while ($_ = <>)
{
	++$line;
# printf("line $line $_");

	if ($state == 0 && /^=item C[<](.*)[>]$/) # pod function prototype doco
	{
		next if $1 =~ /^E/ || $1 =~ / #def/;
		$state = 1;
		$doc = $1;
	}
	elsif ($state == 1 && /^=back$/) # bail out
	{
		$state = 0;
	}
	elsif ($state == 1 && /^=cut$/) # end of pod section
	{
		$state = 2;
	}
	elsif ($state == 2 && /^\*\/$/) # end of pod comment
	{
		do { $_ = <>; ++$line; } while /^$/; # skip blank lines

		while (/^static / || /^#/) # skip static functions and macros
		{
			if (/^static.*;$/) # skip forward declaration
			{
				do { $_ = <>; ++$line; } while /^$/; # skip blank lines
			}
			elsif (/^static/) # static functions or forward declarations
			{
				do { $_ = <>; ++$line; } until /^}$/; # end of static helper function
				do { $_ = <>; ++$line; } while /^$/; # blank lines between functions
			}
			elsif (/^#define/) # macros
			{
				while (/\\$/) { $_ = <>; ++$line; } # skip multi line macros
				do { $_ = <>; ++$line; } while /^$/; # skip blank lines
			}
			else # other cpp directives
			{
				do { $_ = <>; ++$line; } while /^$/; # skip blank lines
			}
		}

		chop();

		if ($doc ne $_) # handle special case of avoiding cpp expansion
		{
			my ($doc_type, $doc_name, $doc_args) = $doc =~ /^(\w+ \*?)([a-zA-Z_0-9]+)\((.*)\)/;
			my ($src_type, $src_name, $src_args) = $_ =~ /^(\w+ \*?)([()a-zA-Z_0-9]+)\((.*)\)/;

# if (!defined $doc_type || !defined $doc_name || !defined $doc_args || !defined $src_type || !defined $src_name || !defined $src_args)
# {
# print 'doc = ', $doc, "\n";
# print 'src = ', $_, "\n";
# print 'line = ', $line, "\n";
# print 'doc_type = ', $doc_type, "\n";
# print 'doc_name = ', $doc_name, "\n";
# print 'doc_args = ', $doc_args, "\n";
# print 'src_type = ', $src_type, "\n";
# print 'src_name = ', $src_name, "\n";
# print 'src_args = ', $src_args, "\n";
# }
			print("doc = '$doc'\nsrc = '$_'\n\n")
				if $src_type ne $doc_type || $src_name ne "($doc_name)" || $src_args ne $doc_args;
		}

		$state = 0;
		$doc = undef;
	}
}

# vi:set ts=4 sw=4
