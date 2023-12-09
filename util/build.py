#!/usr/bin/env python3

import os;
import argparse;
import shutil;
import multiprocessing;

from datetime import datetime;

def parse_arg():
    l_argparse = argparse.ArgumentParser(description="Build GEM5.");
    l_argparse.add_argument('-j', nargs='?',
                            default=argparse.SUPPRESS, dest="maxjobs",
                            type=int, help="build jobs in parallel");
    l_argparse.add_argument('-f', action="store_true", dest="fast",
                            default=False, help="build fast binary");
    l_argparse.add_argument('-o', action="store_true", dest="opt",
                            default=False, help="build opt binary");
    l_argparse.add_argument('-d', action="store_true", dest="debug",
                            default=False, help="build debug binary");
    l_argparse.add_argument('--prof', action="store_true", dest="prof",
                            default=False, help="build profiling binary");
    l_argparse.add_argument('--pgo', action="store_true", dest="pgo",
                            default=False, help="build profile guided optimization binary");
    l_argparse.add_argument('--pgo-gen', action="store_true", dest="pgo",
                            default=False, help="profile generating binary for pgo");
    l_argparse.add_argument('--with-lto', action="store_true", dest="with_lto",
                            default=False, help="profile generating binary for pgo");
    l_argparse.add_argument('-c', action="store_true", dest="clean",
                            default=False, help="clean the build");
    l_argparse.add_argument('-a', action="store_true", dest="all",
                            default=False, help="build all binaries");
    l_argparse.add_argument('-n', '--nop', action="store_true", dest="nop",
                            default=False, help="print build command and exit");
    l_argparse.add_argument('-v', action="store_true", dest="verbose",
                            default=False, help="build with verbose commands");
    l_argparse.add_argument('-s', action="store_true", dest="sanitize",
                            default=False, help="build with sanitizers enabled");
    l_argparse.add_argument('-m', action="store_true", dest="mold",
                            default=False, help="build with mold linker");
    l_argparse.add_argument('--clang', action="store_true", dest="clang",
                            default=False, help="Use clang to build binary");

    return l_argparse;

def main():
    l_args = parse_arg().parse_args();

    start_time = datetime.now().replace(microsecond=0);

    if shutil.which('mold') != None:
        l_args.mold = True;
    elif l_args.mold:
        print("Mold linker unavailable.");
        l_args.mold = False;

    if not hasattr(l_args, 'maxjobs'):
        l_args.maxjobs = multiprocessing.cpu_count();

    cmd = "";
    if l_args.clang:
        if shutil.which('clang++') != None:
            cmd += "export CC=clang\n";
            cmd += "export CXX=clang++\n\n";

    cmd += "scons ";

    if l_args.with_lto:
        cmd += " --with-lto";

    target = "";
    if l_args.fast or l_args.all:
        target += " build/ARM/gem5.fast";
    if l_args.opt or l_args.all:
        target += " build/ARM/gem5.opt";
    if l_args.debug or l_args.all:
        target += " build/ARM/gem5.debug";

    if target == "":
        target = " build/ARM/gem5.debug";

    cmd += target;
    cmd += f" -j{l_args.maxjobs}";

    if l_args.clean:
        cmd += " -c";

    if l_args.verbose:
        cmd += " --verbose";

    if l_args.sanitize:
        cmd += " --with-asan --with-ubsan";

    if l_args.mold:
        cmd += " --linker=mold";

    print(cmd);
    if not l_args.nop:
        os.system(cmd);

    elapsed = str(datetime.now().replace(microsecond=0) - start_time);

    print("\n\033[1mCompile completed in ", elapsed.split(':')[1], "min, ",
                                            elapsed.split(':')[2], "sec\n");

if __name__ == '__main__':
    main();
