#!/usr/bin/env python
# Copyright (c) 2012 The Chromium Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""This script is used to download prebuilt clang binaries.

It is also used by package.py to build the prebuilt clang binaries."""

import argparse
import distutils.spawn
import glob
import os
import pipes
import re
import shutil
import subprocess
import stat
import sys
import tarfile
import tempfile
import time
import urllib2
import zipfile


# Do NOT CHANGE this if you don't know what you're doing -- see
# https://chromium.googlesource.com/chromium/src/+/master/docs/updating_clang.md
# Reverting problematic clang rolls is safe, though.
CLANG_REVISION = '327688'

use_head_revision = bool(os.environ.get('LLVM_FORCE_HEAD_REVISION', '0')
                         in ('1', 'YES'))
if use_head_revision:
  CLANG_REVISION = 'HEAD'

# This is incremented when pushing a new build of Clang at the same revision.
CLANG_SUB_REVISION=1

PACKAGE_VERSION = "%s-%s" % (CLANG_REVISION, CLANG_SUB_REVISION)

# Path constants. (All of these should be absolute paths.)
THIS_DIR = os.path.abspath(os.path.dirname(__file__))
CHROMIUM_DIR = os.path.abspath(os.path.join(THIS_DIR, '..', '..', '..'))
GCLIENT_CONFIG = os.path.join(os.path.dirname(CHROMIUM_DIR), '.gclient')
THIRD_PARTY_DIR = os.path.join(CHROMIUM_DIR, 'third_party')
LLVM_DIR = os.path.join(THIRD_PARTY_DIR, 'llvm')
LLVM_BOOTSTRAP_DIR = os.path.join(THIRD_PARTY_DIR, 'llvm-bootstrap')
LLVM_BOOTSTRAP_INSTALL_DIR = os.path.join(THIRD_PARTY_DIR,
                                          'llvm-bootstrap-install')
CHROME_TOOLS_SHIM_DIR = os.path.join(LLVM_DIR, 'tools', 'chrometools')
LLVM_BUILD_DIR = os.path.join(CHROMIUM_DIR, 'third_party', 'llvm-build',
                              'Release+Asserts')
THREADS_ENABLED_BUILD_DIR = os.path.join(LLVM_BUILD_DIR, 'threads_enabled')
COMPILER_RT_BUILD_DIR = os.path.join(LLVM_BUILD_DIR, 'compiler-rt')
CLANG_DIR = os.path.join(LLVM_DIR, 'tools', 'clang')
LLD_DIR = os.path.join(LLVM_DIR, 'tools', 'lld')
# compiler-rt is built as part of the regular LLVM build on Windows to get
# the 64-bit runtime, and out-of-tree elsewhere.
# TODO(thakis): Try to unify this.
if sys.platform == 'win32':
  COMPILER_RT_DIR = os.path.join(LLVM_DIR, 'projects', 'compiler-rt')
else:
  COMPILER_RT_DIR = os.path.join(LLVM_DIR, 'compiler-rt')
LIBCXX_DIR = os.path.join(LLVM_DIR, 'projects', 'libcxx')
LIBCXXABI_DIR = os.path.join(LLVM_DIR, 'projects', 'libcxxabi')
LLVM_BUILD_TOOLS_DIR = os.path.abspath(
    os.path.join(LLVM_DIR, '..', 'llvm-build-tools'))
STAMP_FILE = os.path.normpath(
    os.path.join(LLVM_DIR, '..', 'llvm-build', 'cr_build_revision'))
VERSION = '7.0.0'
ANDROID_NDK_DIR = os.path.join(
    CHROMIUM_DIR, 'third_party', 'android_ndk')

# URL for pre-built binaries.
CDS_URL = os.environ.get('CDS_CLANG_BUCKET_OVERRIDE',
    'https://commondatastorage.googleapis.com/chromium-browser-clang')

LLVM_REPO_URL='https://llvm.org/svn/llvm-project'
if 'LLVM_REPO_URL' in os.environ:
  LLVM_REPO_URL = os.environ['LLVM_REPO_URL']

# Bump after VC updates.
DIA_DLL = {
  '2013': 'msdia120.dll',
  '2015': 'msdia140.dll',
  '2017': 'msdia140.dll',
}


def DownloadUrl(url, output_file):
  """Download url into output_file."""
  CHUNK_SIZE = 4096
  TOTAL_DOTS = 10
  num_retries = 3
  retry_wait_s = 5  # Doubled at each retry.

  while True:
    try:
      sys.stdout.write('Downloading %s ' % url)
      sys.stdout.flush()
      response = urllib2.urlopen(url)
      total_size = int(response.info().getheader('Content-Length').strip())
      bytes_done = 0
      dots_printed = 0
      while True:
        chunk = response.read(CHUNK_SIZE)
        if not chunk:
          break
        output_file.write(chunk)
        bytes_done += len(chunk)
        num_dots = TOTAL_DOTS * bytes_done / total_size
        sys.stdout.write('.' * (num_dots - dots_printed))
        sys.stdout.flush()
        dots_printed = num_dots
      if bytes_done != total_size:
        raise urllib2.URLError("only got %d of %d bytes" %
                               (bytes_done, total_size))
      print ' Done.'
      return
    except urllib2.URLError as e:
      sys.stdout.write('\n')
      print e
      if num_retries == 0 or isinstance(e, urllib2.HTTPError) and e.code == 404:
        raise e
      num_retries -= 1
      print 'Retrying in %d s ...' % retry_wait_s
      time.sleep(retry_wait_s)
      retry_wait_s *= 2


def EnsureDirExists(path):
  if not os.path.exists(path):
    os.makedirs(path)


def DownloadAndUnpack(url, output_dir, path_prefix=None):
  """Download an archive from url and extract into output_dir. If path_prefix is
     not None, only extract files whose paths within the archive start with
     path_prefix."""
  with tempfile.TemporaryFile() as f:
    DownloadUrl(url, f)
    f.seek(0)
    EnsureDirExists(output_dir)
    if url.endswith('.zip'):
      assert path_prefix is None
      zipfile.ZipFile(f).extractall(path=output_dir)
    else:
      t = tarfile.open(mode='r:gz', fileobj=f)
      members = None
      if path_prefix is not None:
        members = [m for m in t.getmembers() if m.name.startswith(path_prefix)]
      t.extractall(path=output_dir, members=members)


def ReadStampFile(path=STAMP_FILE):
  """Return the contents of the stamp file, or '' if it doesn't exist."""
  try:
    with open(path, 'r') as f:
      return f.read().rstrip()
  except IOError:
    return ''


def WriteStampFile(s, path=STAMP_FILE):
  """Write s to the stamp file."""
  EnsureDirExists(os.path.dirname(path))
  with open(path, 'w') as f:
    f.write(s)
    f.write('\n')


def GetSvnRevision(svn_repo):
  """Returns current revision of the svn repo at svn_repo."""
  svn_info = subprocess.check_output('svn info ' + svn_repo, shell=True)
  m = re.search(r'Revision: (\d+)', svn_info)
  return m.group(1)


def RmTree(dir):
  """Delete dir."""
  def ChmodAndRetry(func, path, _):
    # Subversion can leave read-only files around.
    if not os.access(path, os.W_OK):
      os.chmod(path, stat.S_IWUSR)
      return func(path)
    raise

  shutil.rmtree(dir, onerror=ChmodAndRetry)


def RmCmakeCache(dir):
  """Delete CMake cache related files from dir."""
  for dirpath, dirs, files in os.walk(dir):
    if 'CMakeCache.txt' in files:
      os.remove(os.path.join(dirpath, 'CMakeCache.txt'))
    if 'CMakeFiles' in dirs:
      RmTree(os.path.join(dirpath, 'CMakeFiles'))


def RunCommand(command, msvc_arch=None, env=None, fail_hard=True):
  """Run command and return success (True) or failure; or if fail_hard is
     True, exit on failure.  If msvc_arch is set, runs the command in a
     shell with the msvc tools for that architecture."""

  if msvc_arch and sys.platform == 'win32':
    command = GetVSVersion().SetupScript(msvc_arch) + ['&&'] + command

  # https://docs.python.org/2/library/subprocess.html:
  # "On Unix with shell=True [...] if args is a sequence, the first item
  # specifies the command string, and any additional items will be treated as
  # additional arguments to the shell itself.  That is to say, Popen does the
  # equivalent of:
  #   Popen(['/bin/sh', '-c', args[0], args[1], ...])"
  #
  # We want to pass additional arguments to command[0], not to the shell,
  # so manually join everything into a single string.
  # Annoyingly, for "svn co url c:\path", pipes.quote() thinks that it should
  # quote c:\path but svn can't handle quoted paths on Windows.  Since on
  # Windows follow-on args are passed to args[0] instead of the shell, don't
  # do the single-string transformation there.
  if sys.platform != 'win32':
    command = ' '.join([pipes.quote(c) for c in command])
  print 'Running', command
  if subprocess.call(command, env=env, shell=True) == 0:
    return True
  print 'Failed.'
  if fail_hard:
    sys.exit(1)
  return False


def CopyFile(src, dst):
  """Copy a file from src to dst."""
  print "Copying %s to %s" % (src, dst)
  shutil.copy(src, dst)


def CopyDirectoryContents(src, dst, filename_filter=None):
  """Copy the files from directory src to dst
  with an optional filename filter."""
  dst = os.path.realpath(dst)  # realpath() in case dst ends in /..
  EnsureDirExists(dst)
  for root, _, files in os.walk(src):
    for f in files:
      if filename_filter and not re.match(filename_filter, f):
        continue
      CopyFile(os.path.join(root, f), dst)


def Checkout(name, url, dir):
  """Checkout the SVN module at url into dir. Use name for the log message."""
  print "Checking out %s r%s into '%s'" % (name, CLANG_REVISION, dir)

  command = ['svn', 'checkout', '--force', url + '@' + CLANG_REVISION, dir]
  if RunCommand(command, fail_hard=False):
    return

  if os.path.isdir(dir):
    print "Removing %s." % (dir)
    RmTree(dir)

  print "Retrying."
  RunCommand(command)


def CheckoutRepos(args):
  if args.skip_checkout:
    return

  Checkout('LLVM', LLVM_REPO_URL + '/llvm/trunk', LLVM_DIR)
  Checkout('Clang', LLVM_REPO_URL + '/cfe/trunk', CLANG_DIR)
  if True:
    Checkout('LLD', LLVM_REPO_URL + '/lld/trunk', LLD_DIR)
  elif os.path.exists(LLD_DIR):
    # In case someone sends a tryjob that temporary adds lld to the checkout,
    # make sure it's not around on future builds.
    RmTree(LLD_DIR)
  Checkout('compiler-rt', LLVM_REPO_URL + '/compiler-rt/trunk', COMPILER_RT_DIR)
  if sys.platform == 'darwin':
    # clang needs a libc++ checkout, else -stdlib=libc++ won't find includes
    # (i.e. this is needed for bootstrap builds).
    Checkout('libcxx', LLVM_REPO_URL + '/libcxx/trunk', LIBCXX_DIR)
    # We used to check out libcxxabi on OS X; we no longer need that.
    if os.path.exists(LIBCXXABI_DIR):
      RmTree(LIBCXXABI_DIR)


def DeleteChromeToolsShim():
  OLD_SHIM_DIR = os.path.join(LLVM_DIR, 'tools', 'zzz-chrometools')
  shutil.rmtree(OLD_SHIM_DIR, ignore_errors=True)
  shutil.rmtree(CHROME_TOOLS_SHIM_DIR, ignore_errors=True)


def CreateChromeToolsShim():
  """Hooks the Chrome tools into the LLVM build.

  Several Chrome tools have dependencies on LLVM/Clang libraries. The LLVM build
  detects implicit tools in the tools subdirectory, so this helper install a
  shim CMakeLists.txt that forwards to the real directory for the Chrome tools.

  Note that the shim directory name intentionally has no - or _. The implicit
  tool detection logic munges them in a weird way."""
  assert not any(i in os.path.basename(CHROME_TOOLS_SHIM_DIR) for i in '-_')
  os.mkdir(CHROME_TOOLS_SHIM_DIR)
  with file(os.path.join(CHROME_TOOLS_SHIM_DIR, 'CMakeLists.txt'), 'w') as f:
    f.write('# Automatically generated by tools/clang/scripts/update.py. ' +
            'Do not edit.\n')
    f.write('# Since tools/clang is located in another directory, use the \n')
    f.write('# two arg version to specify where build artifacts go. CMake\n')
    f.write('# disallows reuse of the same binary dir for multiple source\n')
    f.write('# dirs, so the build artifacts need to go into a subdirectory.\n')
    f.write('if (CHROMIUM_TOOLS_SRC)\n')
    f.write('  add_subdirectory(${CHROMIUM_TOOLS_SRC} ' +
              '${CMAKE_CURRENT_BINARY_DIR}/a)\n')
    f.write('endif (CHROMIUM_TOOLS_SRC)\n')


def DownloadHostGcc(args):
  """Downloads gcc 4.8.5 and makes sure args.gcc_toolchain is set."""
  if not sys.platform.startswith('linux') or args.gcc_toolchain:
    return
  gcc_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, 'gcc485precise')
  if not os.path.exists(gcc_dir):
    print 'Downloading pre-built GCC 4.8.5...'
    DownloadAndUnpack(
        CDS_URL + '/tools/gcc485precise.tgz', LLVM_BUILD_TOOLS_DIR)
  args.gcc_toolchain = gcc_dir


def AddSvnToPathOnWin():
  """Download svn.exe and add it to PATH."""
  if sys.platform != 'win32':
    return
  svn_ver = 'svn-1.6.6-win'
  svn_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, svn_ver)
  if not os.path.exists(svn_dir):
    DownloadAndUnpack(CDS_URL + '/tools/%s.zip' % svn_ver, LLVM_BUILD_TOOLS_DIR)
  os.environ['PATH'] = svn_dir + os.pathsep + os.environ.get('PATH', '')


def AddCMakeToPath(args):
  """Download CMake and add it to PATH."""
  if args.use_system_cmake:
    return
  if sys.platform == 'win32':
    zip_name = 'cmake-3.4.3-win32-x86.zip'
    cmake_dir = os.path.join(LLVM_BUILD_TOOLS_DIR,
                             'cmake-3.4.3-win32-x86', 'bin')
  else:
    suffix = 'Darwin' if sys.platform == 'darwin' else 'Linux'
    zip_name = 'cmake343_%s.tgz' % suffix
    cmake_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, 'cmake343', 'bin')
  if not os.path.exists(cmake_dir):
    DownloadAndUnpack(CDS_URL + '/tools/' + zip_name, LLVM_BUILD_TOOLS_DIR)
  os.environ['PATH'] = cmake_dir + os.pathsep + os.environ.get('PATH', '')


def AddGnuWinToPath():
  """Download some GNU win tools and add them to PATH."""
  if sys.platform != 'win32':
    return

  gnuwin_dir = os.path.join(LLVM_BUILD_TOOLS_DIR, 'gnuwin')
  GNUWIN_VERSION = '7'
  GNUWIN_STAMP = os.path.join(gnuwin_dir, 'stamp')
  if ReadStampFile(GNUWIN_STAMP) == GNUWIN_VERSION:
    print 'GNU Win tools already up to date.'
  else:
    zip_name = 'gnuwin-%s.zip' % GNUWIN_VERSION
    DownloadAndUnpack(CDS_URL + '/tools/' + zip_name, LLVM_BUILD_TOOLS_DIR)
    WriteStampFile(GNUWIN_VERSION, GNUWIN_STAMP)

  os.environ['PATH'] = gnuwin_dir + os.pathsep + os.environ.get('PATH', '')


vs_version = None
def GetVSVersion():
  global vs_version
  if vs_version:
    return vs_version

  # Try using the toolchain in depot_tools.
  # This sets environment variables used by SelectVisualStudioVersion below.
  sys.path.append(os.path.join(CHROMIUM_DIR, 'build'))
  import vs_toolchain
  vs_toolchain.SetEnvironmentAndGetRuntimeDllDirs()

  # Use gyp to find the MSVS installation, either in depot_tools as per above,
  # or a system-wide installation otherwise.
  sys.path.append(os.path.join(CHROMIUM_DIR, 'tools', 'gyp', 'pylib'))
  import gyp.MSVSVersion
  vs_version = gyp.MSVSVersion.SelectVisualStudioVersion(
      vs_toolchain.GetVisualStudioVersion())
  return vs_version


def CopyDiaDllTo(target_dir):
  # This script always wants to use the 64-bit msdia*.dll.
  dia_path = os.path.join(GetVSVersion().Path(), 'DIA SDK', 'bin', 'amd64')
  dia_dll = os.path.join(dia_path, DIA_DLL[GetVSVersion().ShortName()])
  CopyFile(dia_dll, target_dir)


def VeryifyVersionOfBuiltClangMatchesVERSION():
  """Checks that `clang --version` outputs VERSION.  If this fails, VERSION
  in this file is out-of-date and needs to be updated (possibly in an
  `if use_head_revision:` block in main() first)."""
  clang = os.path.join(LLVM_BUILD_DIR, 'bin', 'clang')
  if sys.platform == 'win32':
    # TODO: Parse `clang-cl /?` output for built clang's version and check that
    # to check the binary we're actually shipping? But clang-cl.exe is just
    # a copy of clang.exe, so this does check the same thing.
    clang += '.exe'
  version_out = subprocess.check_output([clang, '--version'])
  version_out = re.match(r'clang version ([0-9.]+)', version_out).group(1)
  if version_out != VERSION:
    print ('unexpected clang version %s (not %s), update VERSION in update.py'
           % (version_out, VERSION))
    sys.exit(1)


def GetPlatformUrlPrefix(platform):
  if platform == 'win32' or platform == 'cygwin':
    return CDS_URL + '/Win/'
  if platform == 'darwin':
    return CDS_URL + '/Mac/'
  assert platform.startswith('linux')
  return CDS_URL + '/Linux_x64/'


def DownloadAndUnpackClangPackage(platform, runtimes_only=False):
  cds_file = "clang-%s.tgz" %  PACKAGE_VERSION
  cds_full_url = GetPlatformUrlPrefix(platform) + cds_file
  try:
    path_prefix = None
    if runtimes_only:
      path_prefix = 'lib/clang/' + VERSION + '/lib/'
    DownloadAndUnpack(cds_full_url, LLVM_BUILD_DIR, path_prefix)
  except urllib2.URLError:
    print 'Failed to download prebuilt clang %s' % cds_file
    print 'Use --force-local-build if you want to build locally.'
    print 'Exiting.'
    sys.exit(1)


def UpdateClang(args):
  # Read target_os from .gclient so we know which non-native runtimes we need.
  # TODO(pcc): See if we can download just the runtimes instead of the entire
  # clang package, and do that from DEPS instead of here.
  target_os = []
  try:
    env = {}
    execfile(GCLIENT_CONFIG, env, env)
    target_os = env.get('target_os', target_os)
  except:
    pass

  expected_stamp = ','.join([PACKAGE_VERSION] + target_os)
  if ReadStampFile() == expected_stamp and not args.force_local_build:
    return 0

  # Reset the stamp file in case the build is unsuccessful.
  WriteStampFile('')

  if not args.force_local_build:
    if os.path.exists(LLVM_BUILD_DIR):
      RmTree(LLVM_BUILD_DIR)

    DownloadAndUnpackClangPackage(sys.platform)
    if 'win' in target_os:
      DownloadAndUnpackClangPackage('win32', runtimes_only=True)
    if sys.platform == 'win32':
      CopyDiaDllTo(os.path.join(LLVM_BUILD_DIR, 'bin'))
    WriteStampFile(expected_stamp)
    return 0

  if args.with_android and not os.path.exists(ANDROID_NDK_DIR):
    print 'Android NDK not found at ' + ANDROID_NDK_DIR
    print 'The Android NDK is needed to build a Clang whose -fsanitize=address'
    print 'works on Android. See '
    print 'https://www.chromium.org/developers/how-tos/android-build-instructions'
    print 'for how to install the NDK, or pass --without-android.'
    return 1

  print 'Locally building Clang %s...' % PACKAGE_VERSION

  DownloadHostGcc(args)
  AddCMakeToPath(args)
  AddGnuWinToPath()

  DeleteChromeToolsShim()

  CheckoutRepos(args)

  if args.skip_build:
    return

  cc, cxx = None, None
  libstdcpp = None
  if args.gcc_toolchain:  # This option is only used on Linux.
    # Use the specified gcc installation for building.
    cc = os.path.join(args.gcc_toolchain, 'bin', 'gcc')
    cxx = os.path.join(args.gcc_toolchain, 'bin', 'g++')

    if not os.access(cc, os.X_OK):
      print 'Invalid --gcc-toolchain: "%s"' % args.gcc_toolchain
      print '"%s" does not appear to be valid.' % cc
      return 1

    # Set LD_LIBRARY_PATH to make auxiliary targets (tablegen, bootstrap
    # compiler, etc.) find the .so.
    libstdcpp = subprocess.check_output(
        [cxx, '-print-file-name=libstdc++.so.6']).rstrip()
    os.environ['LD_LIBRARY_PATH'] = os.path.dirname(libstdcpp)

  cflags = []
  cxxflags = []
  ldflags = []

  base_cmake_args = ['-GNinja',
                     '-DCMAKE_BUILD_TYPE=Release',
                     '-DLLVM_ENABLE_ASSERTIONS=ON',
                     # Statically link MSVCRT to avoid DLL dependencies.
                     '-DLLVM_USE_CRT_RELEASE=MT',
                     ]

  if sys.platform != 'win32':
    # libxml2 is required by the Win manifest merging tool used in cross-builds.
    base_cmake_args.append('-DLLVM_ENABLE_LIBXML2=FORCE_ON')

  if args.bootstrap:
    print 'Building bootstrap compiler'
    EnsureDirExists(LLVM_BOOTSTRAP_DIR)
    os.chdir(LLVM_BOOTSTRAP_DIR)
    bootstrap_args = base_cmake_args + [
        '-DLLVM_TARGETS_TO_BUILD=X86;ARM;AArch64',
        '-DCMAKE_INSTALL_PREFIX=' + LLVM_BOOTSTRAP_INSTALL_DIR,
        '-DCMAKE_C_FLAGS=' + ' '.join(cflags),
        '-DCMAKE_CXX_FLAGS=' + ' '.join(cxxflags),
        ]
    if cc is not None:  bootstrap_args.append('-DCMAKE_C_COMPILER=' + cc)
    if cxx is not None: bootstrap_args.append('-DCMAKE_CXX_COMPILER=' + cxx)
    RmCmakeCache('.')
    RunCommand(['cmake'] + bootstrap_args + [LLVM_DIR], msvc_arch='x64')
    RunCommand(['ninja'], msvc_arch='x64')
    if args.run_tests:
      if sys.platform == 'win32':
        CopyDiaDllTo(os.path.join(LLVM_BOOTSTRAP_DIR, 'bin'))
      RunCommand(['ninja', 'check-all'], msvc_arch='x64')
    RunCommand(['ninja', 'install'], msvc_arch='x64')

    if sys.platform == 'win32':
      cc = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'clang-cl.exe')
      cxx = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'clang-cl.exe')
      # CMake has a hard time with backslashes in compiler paths:
      # https://stackoverflow.com/questions/13050827
      cc = cc.replace('\\', '/')
      cxx = cxx.replace('\\', '/')
    else:
      cc = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'clang')
      cxx = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'clang++')

    if args.gcc_toolchain:
      # Tell the bootstrap compiler to use a specific gcc prefix to search
      # for standard library headers and shared object files.
      cflags = ['--gcc-toolchain=' + args.gcc_toolchain]
      cxxflags = ['--gcc-toolchain=' + args.gcc_toolchain]
    print 'Building final compiler'

  # LLVM uses C++11 starting in llvm 3.5. On Linux, this means libstdc++4.7+ is
  # needed, on OS X it requires libc++. clang only automatically links to libc++
  # when targeting OS X 10.9+, so add stdlib=libc++ explicitly so clang can run
  # on OS X versions as old as 10.7.
  deployment_target = ''

  if sys.platform == 'darwin' and args.bootstrap:
    # When building on 10.9, /usr/include usually doesn't exist, and while
    # Xcode's clang automatically sets a sysroot, self-built clangs don't.
    cflags = ['-isysroot', subprocess.check_output(
        ['xcrun', '--show-sdk-path']).rstrip()]
    cxxflags = ['-stdlib=libc++'] + cflags
    ldflags += ['-stdlib=libc++']
    deployment_target = '10.7'
    # Running libc++ tests takes a long time. Since it was only needed for
    # the install step above, don't build it as part of the main build.
    # This makes running package.py over 10% faster (30 min instead of 34 min)
    RmTree(LIBCXX_DIR)


  # If building at head, define a macro that plugins can use for #ifdefing
  # out code that builds at head, but not at CLANG_REVISION or vice versa.
  if use_head_revision:
    cflags += ['-DLLVM_FORCE_HEAD_REVISION']
    cxxflags += ['-DLLVM_FORCE_HEAD_REVISION']

  # Build PDBs for archival on Windows.  Don't use RelWithDebInfo since it
  # has different optimization defaults than Release.
  # Also disable stack cookies (/GS-) for performance.
  if sys.platform == 'win32':
    cflags += ['/Zi', '/GS-']
    cxxflags += ['/Zi', '/GS-']
    ldflags += ['/DEBUG', '/OPT:REF', '/OPT:ICF']

  deployment_env = None
  if deployment_target:
    deployment_env = os.environ.copy()
    deployment_env['MACOSX_DEPLOYMENT_TARGET'] = deployment_target

  # Build lld and code coverage tools. This is done separately from the rest of
  # the build because these tools require threading support.
  tools_with_threading = [ 'lld', 'llvm-cov', 'llvm-profdata' ]
  print 'Building the following tools with threading support: %s' % (
        str(tools_with_threading))

  if os.path.exists(THREADS_ENABLED_BUILD_DIR):
    RmTree(THREADS_ENABLED_BUILD_DIR)
  EnsureDirExists(THREADS_ENABLED_BUILD_DIR)
  os.chdir(THREADS_ENABLED_BUILD_DIR)

  threads_enabled_cmake_args = base_cmake_args + [
      '-DCMAKE_C_FLAGS=' + ' '.join(cflags),
      '-DCMAKE_CXX_FLAGS=' + ' '.join(cxxflags),
      '-DCMAKE_EXE_LINKER_FLAGS=' + ' '.join(ldflags),
      '-DCMAKE_SHARED_LINKER_FLAGS=' + ' '.join(ldflags),
      '-DCMAKE_MODULE_LINKER_FLAGS=' + ' '.join(ldflags)]
  if cc is not None:
    threads_enabled_cmake_args.append('-DCMAKE_C_COMPILER=' + cc)
  if cxx is not None:
    threads_enabled_cmake_args.append('-DCMAKE_CXX_COMPILER=' + cxx)

  if args.lto_lld:
    # Build lld with LTO. That speeds up the linker by ~10%.
    # We only use LTO for Linux now.
    #
    # The linker expects all archive members to have symbol tables, so the
    # archiver needs to be able to create symbol tables for bitcode files.
    # GNU ar and ranlib don't understand bitcode files, but llvm-ar and
    # llvm-ranlib do, so use them.
    ar = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'llvm-ar')
    ranlib = os.path.join(LLVM_BOOTSTRAP_INSTALL_DIR, 'bin', 'llvm-ranlib')
    threads_enabled_cmake_args += [
        '-DCMAKE_AR=' + ar,
        '-DCMAKE_RANLIB=' + ranlib,
        '-DLLVM_ENABLE_LTO=thin',
        '-DLLVM_USE_LINKER=lld']

  RmCmakeCache('.')
  RunCommand(['cmake'] + threads_enabled_cmake_args + [LLVM_DIR],
             msvc_arch='x64', env=deployment_env)
  RunCommand(['ninja'] + tools_with_threading, msvc_arch='x64')

  # Build clang and other tools.
  CreateChromeToolsShim()

  cmake_args = []
  # TODO(thakis): Unconditionally append this to base_cmake_args instead once
  # compiler-rt can build with clang-cl on Windows (http://llvm.org/PR23698)
  cc_args = base_cmake_args if sys.platform != 'win32' else cmake_args
  if cc is not None:  cc_args.append('-DCMAKE_C_COMPILER=' + cc)
  if cxx is not None: cc_args.append('-DCMAKE_CXX_COMPILER=' + cxx)
  default_tools = ['plugins', 'blink_gc_plugin', 'translation_unit']
  chrome_tools = list(set(default_tools + args.extra_tools))
  cmake_args += base_cmake_args + [
      '-DLLVM_ENABLE_THREADS=OFF',
      '-DCMAKE_C_FLAGS=' + ' '.join(cflags),
      '-DCMAKE_CXX_FLAGS=' + ' '.join(cxxflags),
      '-DCMAKE_EXE_LINKER_FLAGS=' + ' '.join(ldflags),
      '-DCMAKE_SHARED_LINKER_FLAGS=' + ' '.join(ldflags),
      '-DCMAKE_MODULE_LINKER_FLAGS=' + ' '.join(ldflags),
      '-DCMAKE_INSTALL_PREFIX=' + LLVM_BUILD_DIR,
      '-DCHROMIUM_TOOLS_SRC=%s' % os.path.join(CHROMIUM_DIR, 'tools', 'clang'),
      '-DCHROMIUM_TOOLS=%s' % ';'.join(chrome_tools)]

  EnsureDirExists(LLVM_BUILD_DIR)
  os.chdir(LLVM_BUILD_DIR)
  RmCmakeCache('.')
  RunCommand(['cmake'] + cmake_args + [LLVM_DIR],
             msvc_arch='x64', env=deployment_env)
  RunCommand(['ninja'], msvc_arch='x64')

  # Copy in the threaded versions of lld and other tools.
  if sys.platform == 'win32':
    CopyFile(os.path.join(THREADS_ENABLED_BUILD_DIR, 'bin', 'lld-link.exe'),
             os.path.join(LLVM_BUILD_DIR, 'bin'))
    CopyFile(os.path.join(THREADS_ENABLED_BUILD_DIR, 'bin', 'lld.pdb'),
             os.path.join(LLVM_BUILD_DIR, 'bin'))
  else:
    for tool in tools_with_threading:
      CopyFile(os.path.join(THREADS_ENABLED_BUILD_DIR, 'bin', tool),
               os.path.join(LLVM_BUILD_DIR, 'bin'))

  if chrome_tools:
    # If any Chromium tools were built, install those now.
    RunCommand(['ninja', 'cr-install'], msvc_arch='x64')

  VeryifyVersionOfBuiltClangMatchesVERSION()

  # Do an out-of-tree build of compiler-rt.
  # On Windows, this is used to get the 32-bit ASan run-time.
  # TODO(hans): Remove once the regular build above produces this.
  # On Mac and Linux, this is used to get the regular 64-bit run-time.
  # Do a clobbered build due to cmake changes.
  if os.path.isdir(COMPILER_RT_BUILD_DIR):
    RmTree(COMPILER_RT_BUILD_DIR)
  os.makedirs(COMPILER_RT_BUILD_DIR)
  os.chdir(COMPILER_RT_BUILD_DIR)
  # TODO(thakis): Add this once compiler-rt can build with clang-cl (see
  # above).
  #if args.bootstrap and sys.platform == 'win32':
    # The bootstrap compiler produces 64-bit binaries by default.
    #cflags += ['-m32']
    #cxxflags += ['-m32']
  compiler_rt_args = base_cmake_args + [
      '-DLLVM_ENABLE_THREADS=OFF',
      '-DCMAKE_C_FLAGS=' + ' '.join(cflags),
      '-DCMAKE_CXX_FLAGS=' + ' '.join(cxxflags)]
  if sys.platform == 'darwin':
    compiler_rt_args += ['-DCOMPILER_RT_ENABLE_IOS=ON']
  if sys.platform != 'win32':
    compiler_rt_args += ['-DLLVM_CONFIG_PATH=' +
                         os.path.join(LLVM_BUILD_DIR, 'bin', 'llvm-config'),
                        '-DSANITIZER_MIN_OSX_VERSION="10.7"']
  # compiler-rt is part of the llvm checkout on Windows but a stand-alone
  # directory elsewhere, see the TODO above COMPILER_RT_DIR.
  RmCmakeCache('.')
  RunCommand(['cmake'] + compiler_rt_args +
             [LLVM_DIR if sys.platform == 'win32' else COMPILER_RT_DIR],
             msvc_arch='x86', env=deployment_env)
  RunCommand(['ninja', 'compiler-rt'], msvc_arch='x86')
  if sys.platform != 'win32':
    RunCommand(['ninja', 'fuzzer'])

  # Copy select output to the main tree.
  # TODO(hans): Make this (and the .gypi and .isolate files) version number
  # independent.
  if sys.platform == 'win32':
    platform = 'windows'
  elif sys.platform == 'darwin':
    platform = 'darwin'
  else:
    assert sys.platform.startswith('linux')
    platform = 'linux'
  asan_rt_lib_src_dir = os.path.join(COMPILER_RT_BUILD_DIR, 'lib', platform)
  if sys.platform == 'win32':
    # TODO(thakis): This too is due to compiler-rt being part of the checkout
    # on Windows, see TODO above COMPILER_RT_DIR.
    asan_rt_lib_src_dir = os.path.join(COMPILER_RT_BUILD_DIR, 'lib', 'clang',
                                       VERSION, 'lib', platform)
  asan_rt_lib_dst_dir = os.path.join(LLVM_BUILD_DIR, 'lib', 'clang',
                                     VERSION, 'lib', platform)
  # Blacklists:
  CopyDirectoryContents(os.path.join(asan_rt_lib_src_dir, '..', '..'),
                        os.path.join(asan_rt_lib_dst_dir, '..', '..'),
                        r'^.*blacklist\.txt$')
  # Headers:
  if sys.platform != 'win32':
    CopyDirectoryContents(
        os.path.join(COMPILER_RT_BUILD_DIR, 'include/sanitizer'),
        os.path.join(LLVM_BUILD_DIR, 'lib/clang', VERSION, 'include/sanitizer'))
  # Static and dynamic libraries:
  CopyDirectoryContents(asan_rt_lib_src_dir, asan_rt_lib_dst_dir)
  if sys.platform == 'darwin':
    for dylib in glob.glob(os.path.join(asan_rt_lib_dst_dir, '*.dylib')):
      # Fix LC_ID_DYLIB for the ASan dynamic libraries to be relative to
      # @executable_path.
      # TODO(glider): this is transitional. We'll need to fix the dylib
      # name either in our build system, or in Clang. See also
      # http://crbug.com/344836.
      subprocess.call(['install_name_tool', '-id',
                       '@executable_path/' + os.path.basename(dylib), dylib])

  if args.with_android:
    make_toolchain = os.path.join(
        ANDROID_NDK_DIR, 'build', 'tools', 'make_standalone_toolchain.py')
    for target_arch in ['aarch64', 'arm', 'i686']:
      # Make standalone Android toolchain for target_arch.
      toolchain_dir = os.path.join(
          LLVM_BUILD_DIR, 'android-toolchain-' + target_arch)
      api_level = '21' if target_arch == 'aarch64' else '19'
      RunCommand([
          make_toolchain,
          '--api=' + api_level,
          '--force',
          '--install-dir=%s' % toolchain_dir,
          '--stl=libc++',
          '--arch=' + {
              'aarch64': 'arm64',
              'arm': 'arm',
              'i686': 'x86',
          }[target_arch]])

      # Build sanitizer runtimes for Android in a separate build tree.
      build_dir = os.path.join(LLVM_BUILD_DIR, 'android-' + target_arch)
      if not os.path.exists(build_dir):
        os.mkdir(os.path.join(build_dir))
      os.chdir(build_dir)
      target_triple = target_arch
      abi_libs = 'c++abi'
      if target_arch == 'arm':
        target_triple = 'armv7'
        abi_libs += ';unwind'
      target_triple += '-linux-android' + api_level
      cflags = ['--target=%s' % target_triple,
                '--sysroot=%s/sysroot' % toolchain_dir,
                '-B%s' % toolchain_dir]
      android_args = base_cmake_args + [
        '-DLLVM_ENABLE_THREADS=OFF',
        '-DCMAKE_C_COMPILER=' + os.path.join(LLVM_BUILD_DIR, 'bin/clang'),
        '-DCMAKE_CXX_COMPILER=' + os.path.join(LLVM_BUILD_DIR, 'bin/clang++'),
        '-DLLVM_CONFIG_PATH=' + os.path.join(LLVM_BUILD_DIR, 'bin/llvm-config'),
        '-DCMAKE_C_FLAGS=' + ' '.join(cflags),
        '-DCMAKE_CXX_FLAGS=' + ' '.join(cflags),
        '-DSANITIZER_CXX_ABI=none',
        '-DSANITIZER_CXX_ABI_LIBRARY=' + abi_libs,
        '-DCMAKE_SHARED_LINKER_FLAGS=-Wl,-u__cxa_demangle',
        '-DANDROID=1']
      RmCmakeCache('.')
      RunCommand(['cmake'] + android_args + [COMPILER_RT_DIR])
      RunCommand(['ninja', 'asan', 'ubsan'])

      # And copy them into the main build tree.
      for f in glob.glob(os.path.join(build_dir, 'lib/linux/*.so')):
        shutil.copy(f, asan_rt_lib_dst_dir)

  # Run tests.
  if args.run_tests or use_head_revision:
    os.chdir(LLVM_BUILD_DIR)
    RunCommand(['ninja', 'cr-check-all'], msvc_arch='x64')
  if args.run_tests:
    if sys.platform == 'win32':
      CopyDiaDllTo(os.path.join(LLVM_BUILD_DIR, 'bin'))
    os.chdir(LLVM_BUILD_DIR)
    RunCommand(['ninja', 'check-all'], msvc_arch='x64')

  WriteStampFile(PACKAGE_VERSION)
  print 'Clang update was successful.'
  return 0


def main():
  parser = argparse.ArgumentParser(description='Build Clang.')
  parser.add_argument('--bootstrap', action='store_true',
                      help='first build clang with CC, then with itself.')
  # TODO(phajdan.jr): remove --if-needed after fixing callers. It's no-op.
  parser.add_argument('--if-needed', action='store_true',
                      help="run only if the script thinks clang is needed")
  parser.add_argument('--force-local-build', action='store_true',
                      help="don't try to download prebuild binaries")
  parser.add_argument('--gcc-toolchain', help='set the version for which gcc '
                      'version be used for building; --gcc-toolchain=/opt/foo '
                      'picks /opt/foo/bin/gcc')
  parser.add_argument('--lto-lld', action='store_true',
                      help='build lld with LTO')
  parser.add_argument('--llvm-force-head-revision', action='store_true',
                      help=('use the revision in the repo when printing '
                            'the revision'))
  parser.add_argument('--print-revision', action='store_true',
                      help='print current clang revision and exit.')
  parser.add_argument('--print-clang-version', action='store_true',
                      help='print current clang version (e.g. x.y.z) and exit.')
  parser.add_argument('--run-tests', action='store_true',
                      help='run tests after building; only for local builds')
  parser.add_argument('--skip-build', action='store_true',
                      help='do not build anything')
  parser.add_argument('--skip-checkout', action='store_true',
                      help='do not create or update any checkouts')
  parser.add_argument('--extra-tools', nargs='*', default=[],
                      help='select additional chrome tools to build')
  parser.add_argument('--use-system-cmake', action='store_true',
                      help='use the cmake from PATH instead of downloading '
                      'and using prebuilt cmake binaries')
  parser.add_argument('--verify-version',
                      help='verify that clang has the passed-in version')
  parser.add_argument('--without-android', action='store_false',
                      help='don\'t build Android ASan runtime (linux only)',
                      dest='with_android',
                      default=sys.platform.startswith('linux'))
  args = parser.parse_args()

  if args.lto_lld and not args.bootstrap:
    print '--lto-lld requires --bootstrap'
    return 1
  if args.lto_lld and not sys.platform.startswith('linux'):
    print '--lto-lld is only effective on Linux. Ignoring the option.'
    args.lto_lld = False

  # Get svn if we're going to use it to check the revision or do a local build.
  if (use_head_revision or args.llvm_force_head_revision or
      args.force_local_build):
    AddSvnToPathOnWin()

  if args.verify_version and args.verify_version != VERSION:
    print 'VERSION is %s but --verify-version argument was %s, exiting.' % (
        VERSION, args.verify_version)
    print 'clang_version in build/toolchain/toolchain.gni is likely outdated.'
    return 1

  global CLANG_REVISION, PACKAGE_VERSION
  if args.print_revision:
    if use_head_revision or args.llvm_force_head_revision:
      print GetSvnRevision(LLVM_DIR)
    else:
      print PACKAGE_VERSION
    return 0

  if args.print_clang_version:
    sys.stdout.write(VERSION)
    return 0

  # Don't buffer stdout, so that print statements are immediately flushed.
  # Do this only after --print-revision has been handled, else we'll get
  # an error message when this script is run from gn for some reason.
  sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', 0)

  if use_head_revision:
    # Use a real revision number rather than HEAD to make sure that the stamp
    # file logic works.
    CLANG_REVISION = GetSvnRevision(LLVM_REPO_URL)
    PACKAGE_VERSION = CLANG_REVISION + '-0'

    args.force_local_build = True
    if 'OS=android' not in os.environ.get('GYP_DEFINES', ''):
      # Only build the Android ASan rt on ToT bots when targetting Android.
      args.with_android = False

  return UpdateClang(args)


if __name__ == '__main__':
  sys.exit(main())
