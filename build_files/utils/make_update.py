#!/usr/bin/env python3

import os
import re
import subprocess
import sys

def call(cmd):
    print(" ".join(cmd))
    retcode = subprocess.call(cmd)
    if retcode != 0:
      sys.exit(retcode)

def print_stage(text):
    print("")
    print(text)
    print("")

# Test if we are building a specific release version
try:
    branch = subprocess.check_output(["git", "rev-parse", "--abbrev-ref", "HEAD"])
except subprocess.CalledProcessError as e:
    sys.stderr("Failed to get Blender git branch\n")
    sys.exit(1)

branch = branch.strip().decode('utf8')
release_version = re.search("^blender-v(.*)-release$", branch)
if release_version:
    release_version = release_version.group(1)
    print("Using Release Blender v" + release_version)

# Setup for precompiled libraries and tests from svn
lib_dirpath = os.path.join('..', 'lib')

if release_version:
    svn_branch = "tags/blender-" + release_version + "-release"
else:
    svn_branch = "trunk"
svn_url = "https://svn.blender.org/svnroot/bf-blender/" + svn_branch + "/lib/"

# Checkout precompiled libraries
if sys.platform == 'darwin':
    lib_platform = "darwin"
elif sys.platform == 'win32':
    # Windows checkout is usually handled by bat scripts since python3 to run
    # this script is bundled as part of the precompiled libraries. However it
    # is used by the buildbot.
    lib_platform = "win64_vc14"
else:
    # No precompiled libraries for Linux.
    lib_platform = None

if lib_platform:
    lib_platform_dirpath = os.path.join(lib_dirpath, lib_platform)

    if not os.path.exists(lib_platform_dirpath):
        print_stage("Checking out Precompiled Libraries")

        svn_url_platform = svn_url + lib_platform
        call(["svn", "checkout", svn_url_platform, lib_platform_dirpath])

# Update precompiled libraries and tests
print_stage("Updating Precompiled Libraries and Tests")

if os.path.isdir(lib_dirpath):
  for dirname in os.listdir(lib_dirpath):
    if dirname == ".svn":
        continue

    dirpath = os.path.join(lib_dirpath, dirname)
    svn_dirpath = os.path.join(dirpath, ".svn")
    svn_root_dirpath = os.path.join(lib_dirpath, ".svn")

    if os.path.isdir(dirpath) and \
       (os.path.exists(svn_dirpath) or os.path.exists(svn_root_dirpath)):
        call(["svn", "cleanup", dirpath])
        call(["svn", "switch", svn_url + dirname, dirpath])
        call(["svn", "update", dirpath])

# Update blender repository and submodules
print_stage("Updating Blender Git Repository and Submodules")

call(["git", "pull", "--rebase"])
call(["git", "submodule", "update", "--init", "--recursive"])

if not release_version:
    # Update submodules to latest master if not building a specific release.
    # In that case submodules are set to a specific revision, which is checked
    # out by running "git submodule update".
    call(["git", "submodule", "foreach", "git", "checkout", "master"])
    call(["git", "submodule", "foreach", "git", "pull", "--rebase", "origin", "master"])
