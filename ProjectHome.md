Toolwhip is an experimental set of patches to speed up Mac builds, and most relevantly, the Mac [Chromium](http://www.chromium.org/) build.
-
Goals:
  * Make the Apple toolchain, including [cctools](http://www.opensource.apple.com/darwinsource/DevToolsOct2008/cctools-698.1/), [ld64](http://www.opensource.apple.com/darwinsource/DevToolsOct2008/ld64-85.2.1/), and Apple’s [customized](http://www.opensource.apple.com/darwinsource/DevToolsOct2008/gcc_42-5566/) [gcc](http://gcc.gnu.org/), run on Linux.
  * Introduce [Xcode](http://developer.apple.com/tools/xcode/) to a more modern [distcc](http://distcc.googlecode.com/) including [pump mode](http://google-opensource.blogspot.com/2008/08/distccs-pump-mode-new-design-for.html).
  * Combine the above to whip your build into shape.

Progress:
  * The Apple toolchain has been ported to Linux.  See [cctools](http://toolwhip.googlecode.com/svn/trunk/cctools.README), [ld64](http://toolwhip.googlecode.com/svn/trunk/ld64.README), [gcc](http://toolwhip.googlecode.com/svn/trunk/gcc_42-5566.README), and [usr\_include](http://toolwhip.googlecode.com/svn/trunk/usr_include_maker.README).
  * Apple-style [distccd](http://toolwhip.googlecode.com/svn/trunk/distcc_apple.README) was ported to Linux.  However, Apple distcc is outdated and much better performance is available with a newer distcc featuring pump mode.
  * [distcc 3.1](http://toolwhip.googlecode.com/svn/trunk/distcc.README) and its pump mode have been modified to work properly with Apple's compiler (and the Apple cross-compiler when running under Linux.)  It has also been modified for Xcode integration.  It is now possible to build Xcode projects with pump mode by invoking "pump xcodebuild."  The results are very promising.
  * (Update, September 16): We will turn our attention to the Snow Leopard toolchain in the next couple of weeks.