#! /usr/bin/env perl

$file = "job_dagman_usedagdir-" . $ARGV[0] . "-post.out";
open(OUT, ">$file") or die "Can't open $file\n";

print OUT "$ARGV[0] post script\n";

close(OUT);
