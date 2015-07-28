The pending tasks for the distcc work
  * what happens if you pass already-preprocessed input like .i and .ii to the include server?

nice to have, not critical:
  * move the language bits in tests into helper objects so we don't have to repeat so much in the tests
  * DISTCC\_HOSTS token to indicate that xcodedistcc zeroconf hosts should be collected, similar to +zeroconf for native distcc zeroconf hosts


done:
  * -x support
    * -x in distcc/distccd -- code and tests added
    * -x in include\_server -- parse\_command already picks it all up, and compiler\_defaults moves it along collecting the system dirs correctly.
  * probably don't need to do anything with these except stop collecting -arch:
    * -arch support in the include\_server doesn't seem to do anything w/ what it collects
    * -arch support probably needs to support building >1 arch at a time like gcc does.
  * -include - already worked, added test case
  * double check DashONoSpace\_Case since gcc -ofoo -c foo.c seems to work?
  * don't force a .d file to be created on the server since it can conflict w/ more then one -arch option.
  * --hostinfo compiler version reporting
  * fix paths in debug info for Mach-O files
  * finish up --hostinfo
  * xcode developer dir
    * if distcc is doing cpp on the server:
      * send through the sysroot so the server can match the right sdk on the server
      * map the developer dir into a marker for file/link lists
      * on the server reverse it to the right developer dir
    * if distcc is doing cpp on the client
      * strip.c:dcc\_strip\_local\_args needs to remove the sysroot since it's not needed during the compile
    * in both cases, map the developer dir to a token when sending and reverse map in when receiving within all argv entries.
      * handles compiler with developer dir in its path
      * handles sdk for isysroot, etc.
  * -isysroot/--sysroot
    * index\_server needs to collect it and use it when collecting compiler defaults
      * add another dimension for (i)sysroots or combine them w/ the compiler or arch to build the key for lookup.
  * remove GNU Make-ism (filter-out) from Makefile, move the logic into configure.  Make -DXCODE\_INTEGRATION something that configure knows how to set.
  * update the makefile to not compile the xci files if xcode integration isn't enabled.
  * reintegrate xcodedistcc advertisement via dns\_sd (apple)/avahi (linux)
  * extend --host-info to include what SDKs are on the machine for use, it just needs to list the sdk nams, not the paths since those are handled via the developer dir.
  * -iframework to match like -F
  * see if we need to beef up -F support in the include server - beefed up
    * interleave them with -I - done
    * teach the include server to look in the Frameworks's Headers dir - done
    * handle subframework includes like apple gcc - done
  * support header maps

Things to point out to apple
  * the way that /System/Library/LaunchDaemons/distccd.plist starts distccd must change.  USE\_XCODE\_SELECT\_PATH is no longer used.  distccd should be invoked as distccd --no-detach --daemon --zeroconf --priority WHATEVER --allow 0.0.0.0/0
  * The way xcode picks slaves needs to take into account SDKs now:
    * if no SDK set in xcode project/target, only pick machines on the same major OS version.
    * if SDK set in xcode project/target, then as picking hosts only pick hosts that have that SDK installed.
  * DISTCC\_HOSTS needs to include the ",lzo,cpp" to enable lzo and pump mode, it currently turns off all features.
  * It might be nice to add --randomize to DISTCC\_HOSTS, but that might interfere with Xcode's priority scheme.  Still, Xcode can randomize DISTCC\_HOSTS a bit before invoking each distcc.
  * It might be nice to add localhost to DISTCC\_HOSTS, to allow some compilations to occur on the local host.  It would be advisable to set the number of slots for localhost to no more than the number of CPUs present on the local machine.  The 2 extra slots that Xcode generally uses for remote hosts should not be added for localhost, to allow other build-related work to be done on the local machine.  Note that distccd does not need to be running on localhost for this to work; distcc treats localhost specially and just executes the compilation command locally.
  * When Xcode builds a DISTCC\_HOSTS line, it adds an entry for each host and sets the number of slots for the host to 2 more than the number of CPUs that the host reported it has available.   It gets this information from the CPUS= line of "distcc --host-info".  This figure is not correct if the host has been configured to process a different number of jobs by running "distccd --jobs".  The --host-info output contains a JOBS= line which should be used instead of CPUS= to determine the appropriate number of slots per host.
  * If distccd is started on a nonstandard port, Xcode doesn't honor the nonstandard port.  Xcode will hear the Bonjour advertisement but attempt to query it on the default port, 3632.  If the port is not the default, Xcode should append :port to the hostname, for example, box-o-bits.local:3633 if distccd is running on port 3633.  Both "distcc --host-info box-o-bits.local:3633" and putting box-o-bits.local:3633 in DISTCC\_HOSTS already do the right thing.

Things to point out to distcc
  * The include\_server requirement that client\_root contain exactly three components is ludicrous.  Rather than having distcc strip off exactly three components from the beginnings of paths to see inside the client root, the include\_server should communicate its client\_root back to distcc, so that distcc knows exactly what to strip off.