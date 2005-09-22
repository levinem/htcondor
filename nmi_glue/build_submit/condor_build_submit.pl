#!/usr/bin/env perl

###########################################################################
# NOTE: This scripts builds condor 
#
# 1. Get the buildtags from the nighlty build-tag on the CSL machine or 
#    from the command line arguments
# 2. For each tag passed 
#    - Generate the condor_build.desc and sources files
#    - run nmi_submit
###########################################################################

use strict;
use File::Basename;
my $LIBDIR;
BEGIN {
    my $dir1 = dirname($0);
    my $dir2 = dirname($dir1);
    if( $dir1 eq $dir2 ) {
	$LIBDIR = $dir2 . "/../lib";
    } else {
	$LIBDIR = $dir2 . "/lib";
    }
}
use lib "$LIBDIR";
use CondorGlue;

CondorGlue::Initialize("build");

my $default_platforms = 
    "alpha_osf_V5.1, " .
    "alpha_rh_7.2, " .
    "hppa_hpux_B.10.20, " .
    "ia64_sles_8, " .
    "irix_6.5, " .
    "ppc_aix_5.2, " .
    "ppc_macos_10.3, " .
    "sun4u_sol_5.8, " .
    "sun4u_sol_5.9, " .
    "x86_64_rhas_3, " .
    "x86_rh_7.2, " .
    "x86_rh_8.0, " .
    "x86_rh_9, " .
    "x86_winnt_5.1";

CondorGlue::ProcessOptions( $default_platforms );

# will exit when finished
CondorGlue::buildLoop( \&customizeCmdFile ); 


sub customizeCmdFile() {
    my ($fh, $tag, $module) = @_;

    # define the glue scripts we want
    print $fh "pre_all = nmi_glue/build/pre_all\n";
    print $fh "remote_declare = nmi_glue/build/remote_declare\n";
    print $fh "remote_pre = nmi_glue/build/remote_pre\n";
    print $fh "remote_task = nmi_glue/build/remote_task\n";
    print $fh "remote_post = nmi_glue/build/remote_post\n";
    print $fh "platform_post = nmi_glue/build/platform_post\n";
    if( $CondorGlue::no_tests ) {
        print $fh "platform_post_args = --without-tests\n";
    }
    #print $fh "post_all = nmi_glue/build/post_all\n";
    #print $fh "post_all_args = $tag $module\n";

    # print the tag and module in the cmdfile as the user variables
    print $fh "tag = $tag\n";
    print $fh "module = $module\n";

    # misc administrative stuff
    CondorGlue::printPrereqs( *$fh );
}

