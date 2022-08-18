#!/usr/bin/env python3

import argparse
import os
import re
import shlex
import shutil
import signal
import subprocess
import sys
import time
import traceback

TEST_BIN = "./test"
LIB = "libmyalloc.so"

# Maximum runtime per test in seconds. Tests are considered failed if the
# execution took longer than this.
TIMEOUT = 30
TIMEOUT_HEAPFILL = 30
TIMEOUT_LDPRELOAD = 30


# Global state - set by one (or more) test and used later to subtract points
g_compiler_warnings = None

# C files added by student - we need these during compilation
g_additional_sources = ""

# Start using calloc if we determine it's supported
g_use_calloc = False


def alloc_tests():
    return [
        TestGroup("Valid submission", 'compile', 1.0,
            Test("Make", check_compile),
            stop_if_fail=True),
        TestGroup("Malloc", 'malloc', 1.0,
            Test("Simple", alloc("malloc-simple")),
            Test("Zero size", alloc("malloc-zero")),
            Test("Orders", alloc("malloc-orders")),
            Test("Random", alloc("malloc-random")),
            stop_if_fail=True),
        TestGroup("Calloc", 'calloc', 0.5,
            Test("Calloc", test_calloc),
        ),
        TestGroup("Free", 'free', 2.0,
            Test("Reuse", alloc("free-reuse"), stop_all_on_fail=True),
            Test("Random", alloc("free-random")),
            Test("Split free chunks", alloc("free-reuse-split")),
            Test("Merge free chunks", alloc("free-reuse-merge")),
        ),
        TestGroup("Realloc", 'realloc', 1.0,
            Test("Basic", alloc("realloc")),
            Test("Zero", alloc("realloc-zero")),
            Test("Optimized", alloc("realloc-opt")),
        ),
        TestGroup("Batching", 'batch', 1.0,
            Test("Brk can contain more allocs", alloc("batch")),
        ),
        TestGroup("Fragmentation", 'frag', 2.0,
            Test("Amortized overhead <=16", alloc("fragmentation-16"),
                stop_group_on_fail=True),
            Test("Amortized overhead <=8", alloc("fragmentation-8")),
        ),
        TestGroup("Locality", 'local', 0.5,
            Test("Temporal locality", alloc("locality")),
        ),
        TestGroup("Unmap", 'unmap', 1.0,
            Test("Give back memory", alloc("unmap")),
        ),
        TestGroup("Alternative design", 'alt', 1.0,
            Test("Out-of-band metadata", alloc("out-of-band-metadata")),
        ),
        TestGroup("System malloc", 'sysmalloc', 2.0,
            Test("malloc", alloc("system-malloc"), stop_group_on_fail=True),
            Test("preload ls", test_preload("ls -al /")),
            Test("preload python", test_preload("python -c 'print(\"hello, world\\n\")'")),
            Test("preload grep", test_preload("grep -E '^ro+t' /etc/passwd")),
        ),
        TestGroup("Dynamic heap size", 'dynamic', -2.0,
            Test("128K heap",
                alloc("heap-fill", ["-m", "%d" % (128 * 1024)],
                      timeout=TIMEOUT_HEAPFILL)),
            Test("256M heap",
                alloc("heap-fill", ["-m", "%d" % (256 * 1024 * 1024)],
                      timeout=TIMEOUT_HEAPFILL)),
        ),
        TestGroup("Compiler warnings", '', -1,
            Test("No warnings", check_warnings),
        ),
    ]


class TestError(Exception):
    pass


class Test():
    """A single test case, with a name and a function to execute)."""
    def __init__(self, name, func, stop_group_on_fail=False,
            stop_all_on_fail=False):
        self.name, self.func = name, func
        self.stop_group_on_fail = stop_group_on_fail
        self.stop_all_on_fail = stop_all_on_fail


class TestGroup():
    """Collection of test cases, which are together worth n points. A testgroup
    is usually a single point in the grade scheme, and individual test cases
    award an (equal) fraction of those points when passed."""

    def __init__(self, fullname, codename, points, *tests, stop_if_fail=False):
        self.fullname = fullname
        self.codename = codename
        self.points = float(points)
        self.tests = tests
        self.stop_if_fail = stop_if_fail


    def run_tests(self, output):
        succeeded = 0
        for test in self.tests:
            output.write('\t' + test.name, end=': ')
            try:
                test.func()
            except TestError as e:
                output.write('FAIL', color='red')
                output.write(e.args[0])
                if g_debug_testerror:
                    output.write_traceback()
                    sys.exit(1)
                if test.stop_all_on_fail:
                    self.stop_if_fail = True
                if self.stop_if_fail or test.stop_group_on_fail:
                    break
            else:
                output.write('OK', color='green')
                succeeded += 1

        self.last_run_had_failing_tests = succeeded != len(self.tests)
        return succeeded


    def run(self, output):
        output.write(self.fullname, color='blue', bold=True, end='')
        if self.codename:
            output.write(' (%s)' % self.codename, color='gray', end='')
        output.write()

        succeeded = self.run_tests(output)

        perc = ((1. * succeeded) / len(self.tests))
        if self.points < 0:
            perc = 1 - perc
        points = round(self.points * perc, 2)

        if self.points > 0:
            output.write(' Passed %d/%d tests, %.2f/%.2f points'
                    % (succeeded, len(self.tests), points, self.points))
        else:
            if perc > 0:
                output.write(' Failed, subtracting %.2f points' % abs(points))

        return points


def test_groups(groups, output):
    points = 0.0
    for group in groups:
        points += group.run(output)

        if group.stop_if_fail and group.last_run_had_failing_tests:
            break

    return points


def full_run(output):
    points = test_groups(alloc_tests(), output)
    totalpoints = sum(g.points for g in alloc_tests() if g.points > 0)

    output.write()
    output.write('Executed all tests, got %.2f/%.2f points in total' % (points,
        totalpoints))

    return points


def partial_run(tests, output):
    all_tests = alloc_tests()
    testmap = {g.codename: g for g in all_tests if g.codename}

    points = 0.0
    for test in tests:
        if test not in testmap:
            output.write('Error: ', color='red', end='')
            output.write('Unknown test "%s". Valid options are: %s'
                    % (test, ', '.join(testmap.keys())))
            break
        group = testmap[test]
        if group.codename and group.codename in tests:
            points += group.run(output)
    return points


class Output:
    def __init__(self, enable_color=True, outfile=sys.stdout):
        self.enable_color = enable_color
        self.outfile = outfile


    def write(self, text='', end='\n', color=None, bold=False, underline=False,
            blink=False, hilight=False):

        if self.enable_color and any((color, bold, underline, blink, hilight)):
            text = self.colorize_shell(text, color=color, bold=bold,
                    underline=underline, blink=blink, hilight=hilight)

        print(text, end=end, file=self.outfile)

    def write_traceback(self):
        exc_type, exc_value, exc_traceback = sys.exc_info()
        tb = traceback.format_exception(exc_type, exc_value, exc_traceback)
        self.write(''.join(tb), end='')


    def colorize_shell(self, val, color=None, bold=False, underline=False,
            blink=False, hilight=False):
        C_RESET = '\033[0m'
        C_BOLD = '\033[1m'
        C_UNDERLINE = '\033[4m'
        C_BLINK = '\033[5m'
        C_HILIGHT = '\033[7m'
        C_GRAY = '\033[90m'
        C_RED = '\033[91m'
        C_GREEN = '\033[92m'
        C_YELLOW = '\033[93m'
        C_BLUE = '\033[94m'
        C_PINK = '\033[95m'
        C_CYAN = '\033[96m'

        codes = ''
        if bold: codes += C_BOLD
        if underline: codes += C_UNDERLINE
        if blink: codes += C_BLINK
        if hilight: codes += C_HILIGHT
        if color:
            codes += {'gray': C_GRAY,
                      'red': C_RED,
                      'green': C_GREEN,
                      'yellow': C_YELLOW,
                      'blue': C_BLUE,
                      'pink': C_PINK,
                      'cyan': C_CYAN}[color]

        return '%s%s%s' % (codes, val, C_RESET)


def check_cmd(cmd, add_env=None, timeout=None):
    timeout = timeout or TIMEOUT
    args = shlex.split(cmd)
    env = os.environ.copy()
    if add_env:
        env.update(add_env)
    proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, stdin=subprocess.PIPE)
    try:
        out, err = proc.communicate(timeout=timeout)
        out, err = out.decode('utf-8'), err.decode('utf-8')
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        out, err = out.decode('utf-8'), err.decode('utf-8')
        err += "Timeout of %d seconds expired - test took too long. " % timeout

    if proc.returncode:
        raise TestError("Command returned non-zero value.\n" +
                "Command: %s\nReturn code: %d\nstdout: %s\nstderr: %s" %
                (cmd, proc.returncode, out, err))
    return out, err


def run_alloc_test_bin(test, args=None, timeout=None):
    args = args or []
    timeout = timeout or TIMEOUT

    args = [TEST_BIN] + args + [test]

    proc = subprocess.Popen(args, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, stdin=subprocess.PIPE)

    try:
        out, err = proc.communicate(timeout=timeout)
        out, err = out.decode('utf-8'), err.decode('utf-8')
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        out, err = out.decode('utf-8'), err.decode('utf-8')
        err += "Timeout of %d seconds expired - test took too long. " % timeout
    if proc.returncode < 0:
        signame = dict((getattr(signal, n), n) \
            for n in dir(signal) if n.startswith('SIG') and '_' not in n)
        sig = -proc.returncode
        err += "%s (%d)" % (signame.get(sig, "Unknown"), sig)
    return proc.returncode, out, err


def alloc(test, args=None, timeout=None):
    args = args or []
    def alloc_inner():
        if g_use_calloc:
            args.append("-c")
        ret, out, err = run_alloc_test_bin(test, args, timeout=timeout)
        if ret:
            testname = '"%s"' % test
            if args:
                testname += ' (with %s)' % ' '.join(args)
            raise TestError("Test %s exited with error: %s" % (testname, err))
    return alloc_inner

def test_calloc():
    global g_use_calloc
    alloc("calloc")()
    g_use_calloc = True

def test_preload(cmd):
    env = {"LD_PRELOAD": "%s/%s" % (os.getcwd(), LIB)}
    def _inner():
        check_cmd(cmd, env, timeout=TIMEOUT_LDPRELOAD)
    return _inner

def check_warnings():
    if g_compiler_warnings is not None:
        raise TestError("Got compiler warnings:\n%s" % g_compiler_warnings)


def check_compile():
    check_cmd("make clean ADDITIONAL_SOURCES=\"%s\"" %
              g_additional_sources)

    out, err = check_cmd("make ADDITIONAL_SOURCES=\"%s\"" %
                         g_additional_sources)
    err = '\n'.join([l for l in err.split("\n") if not l.startswith("make:")])
    if "warning" in err:
        global g_compiler_warnings
        g_compiler_warnings = err

    check_cmd("%s -h" % TEST_BIN)


def do_additional_params(lst, name, suffix=''):
    for f in lst:
        if not f.endswith(suffix):
            raise TestError("File does not end with %s in %s: '%s'" %
                    (suffix, name, f))
        if '"' in f:
            raise TestError("No quotes allowed in %s: '%s'" % (name, f))
        if '/' in f:
            raise TestError("No slashes allowed in %s: '%s'" % (name, f))
        if '$' in f:
            raise TestError("No $ allowed in %s: '%s'" % (name, f))
        if f.startswith('-'):
            raise TestError("No flags allowed in %s: '%s'" % (name, f))


def fix_makefiles():
    with open('Makefile', 'r') as f:
        addsrc, addhdr = [], []
        for l in f:
            l = l.strip()
            if l.startswith("ADDITIONAL_SOURCES = "):
                addsrc = list(filter(bool, l.split(' ')[2:]))
            if l.startswith("ADDITIONAL_HEADERS = "):
                addhdr = list(filter(bool, l.split(' ')[2:]))
    do_additional_params(addsrc, "ADDITIONAL_SOURCES", ".c")
    do_additional_params(addhdr, "ADDITIONAL_HEADERS", ".h")

    global g_additional_sources
    g_additional_sources = ' '.join(addsrc)

    # On the server we overwrite the submitted makefile with a clean one. For
    # local tests this will fail, which is fine.
    try:
        shutil.copyfile('Makefile.orig', 'Makefile')
    except IOError:
        pass


def main():
    os.chdir(os.path.dirname(sys.argv[0]) or '.')

    parser = argparse.ArgumentParser(
        description='Run automated tests for the assignment, and output '
                    'a (tentative) grade.'
    )
    parser.add_argument(
        '--no-color',
        dest='color',
        action='store_const',
        const=False,
        help='disable colorized output',
    )
    parser.add_argument(
        '--force-color',
        dest='color',
        action='store_const',
        const=True,
        help='force colorized output when directing to file',
    )
    parser.add_argument(
        '--codegrade-out',
        action='store_true',
        help='output final result for codegrade',
    )
    parser.add_argument(
        '-o',
        '--out-file',
        type=argparse.FileType('w'),
        help='redirect output to this file (default: stdout)',
    )
    parser.add_argument(
        '-d',
        '--debug-testerror',
        action='store_true',
        help='halt on the first test error with a traceback',
    )
    parser.add_argument(
        nargs='*',
        dest='tests',
        help='which tests to run (default: run all tests). Test names are '
             'displayed in parenthesis with each category.',
    )
    args = parser.parse_args()

    color = args.color if args.color is not None else args.out_file is None
    output = Output(enable_color=color, outfile=args.out_file)

    global g_debug_testerror
    g_debug_testerror = args.debug_testerror

    try:
        fix_makefiles()

        if args.tests:
            grade = partial_run(args.tests, output)
        else:
            grade = full_run(output)
    except Exception:
        output.write_traceback()


    if args.codegrade_out:
        fraction = min(max(1.0, grade), 10.0) / 10.
        print(fraction)


if __name__ == '__main__':
    main()
