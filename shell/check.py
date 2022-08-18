#!/usr/bin/env python3

import os
import pexpect
import random
import re
import shlex
import shutil
import signal
import string
import subprocess
import sys
import time
import argparse
import traceback


STUDENT_SHELL = './mysh'

# Some global state - set by one (or more) test and used later to subtract
# points
compiler_warnings = None

# C files added by student - we need these during compilation
additional_sources = ''


def basic_tests():
    return [
        TestGroup('Valid submission', 'compile', 1.0,
            Test('Make', check_compile),
            stop_if_fail=True
        ),
        TestGroup('Simple commands', 'simple', 1.0,
            Test('Simple', bash_cmp('uname')),
            Test('Arguments', bash_cmp('echo -n "Hello, shell"')),
            Test('Wait for 1 proc', test_wait('sleep 1', 1)),
            Test('Interactive use', test_interactive),
            stop_if_fail=True
        ),
        TestGroup('exit builtin', 'exit', 1.0,
            Test('exit', test_exit),
        ),
        TestGroup('cd builtin', 'cd', 1.0,
            Test('cd', test_cd),
        ),
        TestGroup('Sequences', 'seq', 1.5,
            Test('Simple sequence', bash_cmp('pwd -L; uname')),
            Test('Nested sequences', bash_cmp('pwd -L; cd /; pwd -L')),
            Test('Wait for sequence', test_wait('sleep 0.5; sleep 1', 1.5)),
        ),
        TestGroup('Pipes', 'pipe', 1.5,
            Test('One pipe 1', bash_cmp('ls -alh . | grep tmp'),
                stop_group_on_fail=True),
            Test('One pipe 2', bash_cmp('cat _tmpfile1 | sort'),
                stop_group_on_fail=True),
            Test('cd in pipe', bash_cmp('cd / | pwd -L')),
            Test('exit in pipe', bash_cmp('exit 1 | pwd -L')),
            Test('Wait for pipes 1', test_wait('sleep 1 | sleep 0.5', 1.0)),
            Test('Wait for pipes 2', test_wait('sleep 0.5 | sleep 1', 1.0)),
        ),
        TestGroup('Compiler warnings', '', -1,
            Test('No warnings', check_warnings),
        ),
        TestGroup('Errors', 'err', -1.0,
            Test('Binary not in path', test_errors),
        ),
        TestGroup('Signals', 'ctrlc', -1.0,
            Test('Ctrl-c', test_ctrl_c),
        ),
    ]

def advanced_tests():
    return [
        TestGroup('Pipes >2p with seqs or pipes', 'pipe2', 1.0,
            Test('Multiple pipes',
                bash_cmp('cat _tmpfile1 | grep "[1-4]$" | grep -v 3 | tac')),
            Test('Seq in pipe',
                bash_cmp('cat _tmpfile1 | { grep "[1-3]$" ; cat _tmpfile2; } | tac')),
            Test('Seq wait',
                test_wait('{ sleep 1 | sleep 2 | sleep 0.5 | echo aaa; }; echo bbb',
                    timeout=2, out='aaa\nbbb\n')),
        ),
        TestGroup('Redirections', 'redir', 1.0,
            Test('To/from file', bash_cmp('>_tmpout cat _tmpfile2; <_tmpout wc -l')),
            Test('Overwrite', bash_cmp('>_tmpout ls /; >_tmpout cat _tmpfile1; cat _tmpout')),
            Test('Append', bash_cmp('>_tmpout cat _tmpfile2; >>_tmpout pwd -L; cat _tmpout')),
            Test('Errors', bash_cmp('>_tmpout 2>&1 cat _tmpfile2 /_idontexist; cat _tmpout', check_rv=False)),
            Test('Errors to out', bash_cmp('2>&1 >/dev/null cat _tmpfile1 /_idontexist',
                check_rv=False)),
        ),
        TestGroup('Detached commands', 'detach', 0.5,
            Test('sleep', test_detach),
            Test('Sequence',
                bash_cmp('{ sleep 0.1; echo hello; }& echo world; sleep 0.3')),
        ),
        TestGroup('Subshells', 'subsh', 0.5,
            Test('exit', bash_cmp('(pwd -L; exit 2); exit 1')),
            Test('cd', bash_cmp('cd /usr; pwd -L; (cd /; pwd -L); pwd -L')),
        ),
        TestGroup('Environment variables', 'env', 0.5,
            Test('Simple', manual_cmp('set hello=world; env | grep hello',
                out='hello=world\n', err='')),
            Test('Subshell',
                manual_cmp('set hello=world; (set hello=bye); env | grep hello',
                out='hello=world\n', err='')),
            Test('Unset', manual_cmp('set hoi=daar; env | grep ^hoi; ' +
                                        'unset hoi; env | grep ^hoi',
                                        out='hoi=daar\n', err='', rv=0)),
        ),
        TestGroup('Prompt', 'prompt', 0.5,
            Test('Username', test_prompt('u$r=\\u $r $')),
            Test('Hostname', test_prompt('h$r=\h $r $')),
            Test('Working dir', test_prompt('w$r=\w $r $')),
            Test('Combined', test_prompt('u$r=\\u h=$r\h w$r=\w $r $')),
        ),
        TestGroup('Job control', 'job', 2.0,
            Test('Ctrl-z', test_ctrl_z),
            Test('Ctrl-z + bg + fg', test_bg_fg),
            Test('Detach + fg', test_detach_fg),
            Test('Detach + fg + Ctrl-z + bg + fg', test_advanced_jobs),
        ),
    ]


class TestError(Exception):
    pass


class Test():
    """A single test case, with a name and a function to execute)."""
    def __init__(self, name, func, stop_group_on_fail=False):
        self.name, self.func, = name, func
        self.stop_group_on_fail = stop_group_on_fail


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
            pretest_setup()
            output.write('\t' + test.name, end=': ')
            try:
                test.func()
            except TestError as e:
                output.write('FAIL', color='red')
                output.write(e.args[0])
                if self.stop_if_fail or test.stop_group_on_fail:
                    break
            else:
                output.write('OK', color='green')
                succeeded += 1
            posttest_cleanup()

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
    points = test_groups(basic_tests(), output)
    totalpoints = sum(g.points for g in basic_tests() if g.points > 0)

    if points >= 5.0:
        output.write('Passed basic tests with enough points, doing advanced '
                'tests')
        points += test_groups(advanced_tests(), output)
        totalpoints += sum(g.points for g in advanced_tests() if g.points > 0)
    else:
        output.write('Didnt get enough points for the basic tests, aborting.')
        output.write('Got %.2f points, need at least 5.0' % points)

    output.write()
    output.write('Executed all tests, got %.2f/%.2f points in total' % (points,
        totalpoints))

    return points


def partial_run(tests, output):
    all_tests = basic_tests() + advanced_tests()
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


def pretest_setup():
    with open('_tmpfile1', 'w') as f:
        f.write('\n'.join(randstring(10) + str(i) for i in range(6)) + '\n')
    with open('_tmpfile2', 'w') as f:
        f.write('\n'.join(randstring(10) for i in
            range(random.randrange(6, 11))) + '\n')


def posttest_cleanup():
    for fname in ('_tmpfile1', '_tmpfile2', '_tmpout'):
        if os.path.isfile(fname):
            os.remove(fname)


class InteractiveShell():
    """Wrapper around pexpect for interacting with a shell (as if a user is
    typing commands)."""

    def __init__(self, env=None):
        self.last_command = ''
        self.last_output = ''
        self.last_prompt = None
        self.shell = pexpect.spawn(STUDENT_SHELL, encoding='utf-8', timeout=5,
                echo=False, env=env)
        self.shell.setwinsize(10, 1024) # Otherwise long prompts get truncated
        self.wait_for_prompt()

    def wait_for_prompt(self):
        try:
            self.shell.expect_exact('$')
        except pexpect.EOF:
            err = 'Your shell exited when waiting for prompt ($) to appear'
            if self.last_command:
                err += ' after executing command "%s"' % self.last_command
            raise TestError(err)
        except pexpect.TIMEOUT:
            err = 'Timeout waiting for prompt ($) to appear'
            if self.last_command:
                err += ' after executing command "%s"' % self.last_command
            raise TestError(err)

        output = self.shell.before
        if self.last_prompt is None:
            if output.count('\n') > 0:
                raise TestError('Prompt of shell contained one or more '
                        'newlines: %s' % output)
            self.last_prompt = output + '$'
        else:
            lines = output.lstrip().split('\r\n')
            self.last_output = '\n'.join(lines[:-1])
            self.last_prompt = lines[-1] + '$'
        return self.last_output

    def command(self, cmd, wait_for_prompt=True):
        self.last_command = cmd
        self.shell.sendline(cmd)

        if wait_for_prompt:
            return self.wait_for_prompt()

    def exit_code(self):
        if self.shell.isalive():
            try:
                self.shell.expect_exact('$')
            except pexpect.EOF:
                # This is what we expect to happen on exit
                self.shell.wait()
            except pexpect.TIMEOUT:
                raise TestError('Timeout waiting for shell to exit')
            else:
                raise TestError('Prompt ($) reappeared when we expected shell '
                        ' to exit')

        return self.shell.exitstatus

    def send_ctrl_c(self):
        self.shell.send(chr(3))

    def send_ctrl_z(self):
        self.shell.send(chr(26))


def eq(a, b, name='Objects'):
    if a != b:
        raise TestError('%s not equal:\n Your output:\n  %s\n '
                        'Correct output:\n  %s' % (name, repr(a), repr(b)))


def randstring(length):
    return ''.join(random.choice(string.ascii_lowercase) for i in range(length))


def get_printable(data, maxlen=None, trunc_str=''):
    # don't consider whitespace as printable
    printable_chars = string.ascii_letters + string.digits + string.punctuation
    if isinstance(data, bytes):
        printable_chars = printable_chars.encode('utf-8')
    did_truncate = False
    if maxlen:
        did_truncate = len(data) > maxlen
        data = data[:maxlen]
    if not all(c in printable_chars for c in data):
        data = repr(data)
    if did_truncate:
        data += trunc_str
    return data


def run_cmd(cmd, check_rv=False):
    """Run any command in the native shell, returning the status code, stdout
    and stderr."""

    if isinstance(cmd, str):
        cmd = shlex.split(cmd)
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, stdin=subprocess.PIPE)
    stdout, stderr = p.communicate()
    try:
        stdout, stderr = stdout.decode('utf-8'), stderr.decode('utf-8')
    except UnicodeDecodeError:
        raise TestError(f'Could not decode the output of command as unicode.\n'
                        f'Command: {cmd}\n'
                        f'Raw stdout: {stdout}\n'
                        f'Raw stderr: {stderr}')

    rv = p.returncode

    if check_rv and rv:
        raise TestError("Command returned non-zero value.\n" +
                "Command: %s\nReturn code: %d\nstdout: %s\nstderr: %s" %
                (cmd, rv, stdout, stderr))
    return rv, stdout, stderr


def run_shell_cmd(cmd, shell=None, prefix=None):
    """Run a command inside a shell (by default the student shell), and return
    the status code, stdout and stderr."""

    shell = shell or STUDENT_SHELL
    prefix = prefix or []
    prefix = prefix if not isinstance(prefix, str) else [prefix]

    cmd = prefix + [shell, '-c', cmd]
    return run_cmd(cmd)


def check_warnings():
    if compiler_warnings is not None:
        raise TestError("Got compiler warnings:\n%s" % compiler_warnings)


def check_compile():
    run_cmd('make moreclean ADDITIONAL_SOURCES="%s\"' % additional_sources)

    rv, out, err = run_cmd('make ADDITIONAL_SOURCES="%s"' %
                         additional_sources, check_rv=True)
    err = '\n'.join(l for l in err.split('\n') if not l.startswith('make:'))
    if 'warning' in err:
        global compiler_warnings
        compiler_warnings = err

    run_cmd('%s -h' % STUDENT_SHELL, check_rv=True)


def bash_cmp(cmd, check_rv=True):
    def bash_cmp_inner():
        rv1, stdout1, stderr1 = run_shell_cmd(cmd)
        rv2, stdout2, stderr2 = run_shell_cmd(cmd, shell='bash')

        try:
            eq(stdout1, stdout2, 'stdout')
            eq(stderr1, stderr2, 'stderr')
            if check_rv:
                eq(rv1, rv2, 'return value')
        except TestError as e:
            outfmt = get_printable(stdout1, 64, '...')
            errfmt = get_printable(stderr1, 64, '...')
            raise TestError(f'Error while comparing your shell output to '
                            f'expected output.\n'
                            f'Command: {cmd}\n'
                            f'stdout: {outfmt}\n'
                            f'stderr: {errfmt}\n'
                            f'{e.args[0]}')
    return bash_cmp_inner


def manual_cmp(cmd, out=None, err=None, rv=None):
    def manual_cmp_inner():
        rv1, stdout1, stderr1 = run_shell_cmd(cmd)

        try:
            if out is not None:
                eq(stdout1, out, 'stdout')
            if err is not None:
                eq(stderr1, err, 'stderr')
            if rv is not None:
                eq(rv1, rv, 'return value')
        except TestError as e:
            outfmt = get_printable(stdout1, 64, '...')
            errfmt = get_printable(stderr1, 64, '...')
            raise TestError(f'Error while comparing your shell output to '
                            f'expected output.\n'
                            f'Command: {cmd}\n'
                            f'stdout: {outfmt}\n'
                            f'stderr: {errfmt}\n'
                            f'{e.args[0]}')
    return manual_cmp_inner


def test_wait(cmd, timeout, out='', err='', offset=0.3):
    timeout = float(timeout)
    def wait():
        start_time = time.time()
        rv, stdout, stderr = run_shell_cmd(cmd)
        end_time = time.time()
        eq(stdout, out, 'stdout')
        eq(stderr, err, 'stderr')

        if not (timeout - offset < end_time - start_time < timeout + offset):
            raise TestError('Command did not finish in expected time.\n'
                    'Command: %s\nExpected time: %f\nTime taken: %f' % (cmd,
                        timeout, end_time - start_time))
    return wait


def test_interactive():
    shell = InteractiveShell()

    inp = randstring(8)
    output = shell.command('echo %s' % inp)
    if output != inp:
        raise TestError('"echo %s" produced incorrect output (%s)'
                % (inp, output))

    inp = randstring(8)
    output = shell.command('echo %s' % inp)
    if output != inp:
        raise TestError('"echo %s" produced incorrect output (%s)'
                % (inp, output))


def test_exit():
    shell = InteractiveShell()
    num = random.randrange(1, 100)
    shell.command('exit %s' % num, wait_for_prompt=False)
    exitcode = shell.exit_code()

    eq(exitcode, num, 'exit status')


def test_prompt(prompt):
    # Insert some randomness in the prompt to prevent hardcoding.
    while '$r' in prompt:
        prompt = prompt.replace('$r', randstring(3), 1)

    def expected_prompt(cwd=None):
        import getpass, socket
        return prompt.replace('\\u', getpass.getuser())\
                     .replace('\h', socket.gethostname())\
                     .replace('\w', cwd or os.getcwd())
    def test_prompt_inner():
        shell = InteractiveShell(env={'PS1': prompt})
        if shell.last_prompt != expected_prompt():
            raise TestError('Prompt incorrect for "%s", expected "%s", got '
                    '"%s"' % (prompt, expected_prompt(), shell.last_prompt))

        if '\w' in prompt:
            shell.command('cd /')

            if shell.last_prompt != expected_prompt('/'):
                raise TestError('Prompt incorrect for "%s", expected "%s", '
                        'got "%s"' % (prompt, expected_prompt('/'),
                                        shell.last_prompt))

    return test_prompt_inner


def test_detach():
    shell = InteractiveShell()

    start_time = time.time()
    shell.command('sleep 1 &')
    if time.time() - start_time > 0.1:
        raise TestError('Detach did not return immediately.\n'
                        'Command: sleep 1 &')

    start_time = time.time()
    procs = shell.command('ps')
    if time.time() - start_time > 0.1:
        raise TestError('Command after detach did not return immediately.\n'
                        'Command: sleep 1 & \\n ps')

    if 'sleep' not in procs:
        raise TestError('Detached command not in ps output.\n'
                        'Command: sleep 1 &')


def test_cd():
    shell = InteractiveShell()

    shell.command('cd /tmp')
    output = shell.command('pwd -L')

    eq(output, '/tmp', 'pwd output')


def proc_status(shell, procname):
    procs = shell.command('ps t')
    for procline in procs.split('\n'):
        if not len(procline)==0:
            pid, ptty, pstat, ptime, pcmd = procline.split(maxsplit=4)
            if pcmd.startswith(procname):
                return pstat

    raise TestError('Process %s not found in background.' % procname)

    p.sendline('ps t')
    p.expect('\$')

    for line in p.before.split('\n'):
        if 'sleep' in line:
            return line.split()[2]

    raise TestError('Sleep not found in background.')


def test_ctrl_z():
    shell = InteractiveShell()
    start_time = time.time()
    shell.command('sleep 2', wait_for_prompt=False)
    shell.send_ctrl_z()
    shell.wait_for_prompt()

    time_taken = time.time() - start_time
    if time_taken > 0.5:
        raise TestError('Sleep was not stopped by SIGTSTP (Ctrl-Z) in '
                'time (prompt took %.2f sec to appear again). Are you '
                'using setpgid() and forwarding the signal?'
                    % time_taken)

    stat = proc_status(shell, 'sleep')
    if stat == 'T+':
        raise TestError('Sleep correctly stopped in background, but still '
                        'in foreground process group. Are you using '
                        'setpgid() correctly?')
    if stat != 'T':
        raise TestError('Sleep found in background, but not stopped '
                        '("ps t" should show status "T").')


def test_bg_fg():
    shell = InteractiveShell()
    start_time = time.time()
    shell.command('sleep 1', wait_for_prompt=False)
    shell.send_ctrl_z()
    shell.wait_for_prompt()

    time_taken = time.time() - start_time
    if time_taken > 0.5:
        raise TestError('Sleep was not stopped by SIGTSTP (Ctrl-Z) in '
                'time (prompt took %.2f sec to appear again). Are you '
                'using setpgid() and forwarding the signal?'
                    % time_taken)

    stat = proc_status(shell, 'sleep')
    if stat == 'T+':
        raise TestError('Sleep correctly stopped in background, but still '
                        'in foreground process group. Are you using '
                        'setpgid() correctly?')
    if stat != 'T':
        raise TestError('Sleep found in background, but not stopped '
                        '("ps t" should show status "T").')


    shell.command('bg')
    if proc_status(shell, 'sleep') != 'S':
        raise TestError('Sleep did not continue in background after bg '
                        'command.')

    shell.command('fg')
    if time.time() - start_time < 0.95:
        raise TestError('Sleep did not finish properly with a wait after fg.')


def test_detach_fg():
    shell = InteractiveShell()
    start_time = time.time()
    shell.command('sleep 0.5 &')
    if proc_status(shell, 'sleep') != 'S':
        raise TestError('Sleep not running in background.')

    shell.command('fg')
    if time.time() - start_time < 0.45:
        raise TestError('Sleep did not finish properly with a wait after fg.')


def test_advanced_jobs():
    shell = InteractiveShell()
    start_time = time.time()
    shell.command('sleep 1 &')

    if proc_status(shell, 'sleep') != 'S':
        raise TestError('Sleep not running in background.')

    shell.command('fg', wait_for_prompt=False)
    shell.send_ctrl_z()
    shell.wait_for_prompt()

    stat = proc_status(shell, 'sleep')
    if stat == 'T+':
        raise TestError('Sleep correctly stopped in background, but still in '
                        'foreground process group. Are you using setpgid() '
                        'correctly?')
    if stat != 'T':
        raise TestError('Sleep found in background, but not stopped ("ps t" '
                        'should show status "T").')

    shell.command('fg')
    if time.time() - start_time < 0.95:
        raise TestError('Sleep did not finish properly with a wait after fg.')


def test_ctrl_c():

    shell = InteractiveShell()
    shell.command('sleep 2', wait_for_prompt=False)

    start_time = time.time()
    shell.send_ctrl_c()
    shell.wait_for_prompt()

    if time.time() - start_time > 0.3:
        raise TestError('Sleep was not killed by SIGINT in time.')

    procs = shell.command('ps')
    if 'sleep' in procs:
        raise TestError('Sleep still active in background.')


def test_errors():
    rv, stdout, stderr = run_shell_cmd('blablabla')
    eq(stdout, '', 'stdout')
    if 'No such file or directory' not in stderr:
        raise TestError('String "No such file or directory" not found in '
                        'stderr: use perror if execvp fails.')


def do_additional_params(lst, name, suffix=''):
    for f in lst:
        if not f.endswith(suffix):
            raise TestError('File does not end with %s in %s: "%s"' %
                    (suffix, name, f))
        if '"' in f:
            raise TestError('No quotes allowed in %s: "%s"' % (name, f))
        if '/' in f:
            raise TestError('No slashes allowed in %s: "%s"' % (name, f))
        if '$' in f:
            raise TestError('No $ allowed in %s: "%s"' % (name, f))
        if f.startswith('-'):
            raise TestError('No flags allowed in %s: "%s"' % (name, f))


def fix_makefiles():
    with open('Makefile', 'r') as f:
        addsrc, addhdr = [], []
        for l in f:
            l = l.strip()
            if l.startswith('ADDITIONAL_SOURCES = '):
                addsrc = list(filter(bool, l.split(' ')[2:]))
            if l.startswith('ADDITIONAL_HEADERS = '):
                addhdr = list(filter(bool, l.split(' ')[2:]))
    do_additional_params(addsrc, 'ADDITIONAL_SOURCES', '.c')
    do_additional_params(addhdr, 'ADDITIONAL_HEADERS', '.h')

    global additional_sources
    additional_sources = ' '.join(addsrc)

    # On the server we overwrite the submitted makefile with a clean one. For
    # local tests this will fail, which is fine.
    try:
        shutil.copyfile('Makefile.orig', 'Makefile')
    except IOError:
        pass


def main():
    os.chdir(os.path.dirname(sys.argv[0]) or '.')

    parser = argparse.ArgumentParser(
        description='Run automated tests for the Shell assignment, and output '
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
        '-s',
        '--seed',
        type=int,
        help='seed to use for random values (default: random)',
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

    seed = args.seed if args.seed is not None else random.getrandbits(32)
    random.seed(seed)
    output.write('Using random seed %d (use --seed %d to repeat this run)'
            % (seed, seed))

    grade = 0
    try:
        fix_makefiles()

        if args.tests:
            grade = partial_run(args.tests, output)
        else:
            grade = full_run(output)
    except:
        exc_type, exc_value, exc_traceback = sys.exc_info()
        tb = traceback.format_exception(exc_type, exc_value, exc_traceback)
        output.write(''.join(tb), end='')


    if args.codegrade_out:
        fraction = min(max(1.0, grade), 10.0) / 10.
        print(fraction)


if __name__ == '__main__':
    main()
