#!/usr/bin/env python3

import argparse
import errno
import hashlib
import os
import random
import shutil
import signal
import string
import stat
import subprocess
import sys
import traceback
from contextlib import contextmanager, suppress


# Student binary (FUSE driver)
FUSE_BIN = './sfs'

# SFS filesystem tools (create and inspect images)
MKFS = './mkfs.sfs'
FSCK = './fsck.sfs'

# Maximum runtime per test in seconds.
TIMEOUT = 30

# Global state - set by one (or more) test and used later to subtract points
g_compiler_warnings = None

g_rand_seed = None
g_debug_testerror = False


def fs_tests():
    return [
        TestGroup('Valid submission', 'compile', 1.0,
            Test('Make', check_compile),
            stop_if_fail=True,
        ),
        TestGroup('Compiler warnings', '', -1,
            Test('No warnings', check_warnings),
        ),
        TestGroup('Listing rootdir', 'ls', 0.5,
            Test('Files', test_list_root_files),
            Test('Directories', test_list_root_dirs),
            Test('Full', test_list_root_full),
            Test('Random', test_list_root_random),
            stop_if_fail=True,
        ),
        TestGroup('Reading file from rootdir', 'read', 1.5,
            Test('Small file',
                test_read_root('Hello world!\n%s\n' % randstr(8, 12))),
            Test('Large file', test_read_root(randstr(4096 * 3, 4096 * 4))),
            Test('Binary data', test_read_root('%s\0%s' % (randbin(512, 1024),
                randbin(256, 512)))),
            Test('Reading non-existing file', test_read_noexist_root),
            Test('Reading from offset in large file', test_read_offset_root),
        ),
        TestGroup('Subdirecties', 'subdir', 1.0,
            Test('Listing 1 level', test_list_subdir_1),
            Test('Listing n levels', test_list_subdir_n),
            Test('Listing non-existing dir', test_list_subdir_noexist),
            Test('Read file 1 level', test_read_subdir_1),
            Test('Read file n levels', test_read_subdir_n),
            Test('Read non-existing file in subdir', test_read_subdir_noexist),
        ),
        TestGroup('Creating directories', 'mkdir', 1.0,
            Test('Create in root', test_mkdir_1),
            Test('Create nested', test_mkdir_n),
            Test('Create too long name', test_mkdir_toolong),
        ),
        TestGroup('Removing directories', 'rmdir', 1.0,
            Test('Remove from root', test_rmdir_root),
            Test('Remove tree', test_rmdir_tree),
            Test('Remove non-empty directory', test_rmdir_nonempty),
        ),
        TestGroup('Removing files', 'rm', 1.0,
            Test('Remove empty file', test_rm_empty),
            Test('Remove small file', test_rm_small),
            Test('Remove large file', test_rm_large),
            Test('Remove from subdir', test_rm_subdir),
        ),
        TestGroup('Creating files', 'create', 1.0,
            Test('Create in root', test_create_root),
            Test('Create in subdirs', test_create_subdir),
            Test('Create too long name', test_create_toolong),
        ),
        TestGroup('Truncating files', 'truncate', 1.5,
            Test('Grow a file', test_truncate_grow),
            Test('Shrink a file', test_truncate_shrink),
        ),
        TestGroup('Writing files', 'write', 2.0,
            Test('Simple', test_write_simple),
            Test('Multi-block', test_write_multiblock),
            Test('Subset', test_write_subset),
            Test('Offset', test_write_offset),
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
                    output.write('Image used for test (if any) preserved, see '
                            '_checker.img', bold=True)
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
    points = test_groups(fs_tests(), output)
    totalpoints = sum(g.points for g in fs_tests() if g.points > 0)

    output.write()
    output.write('Executed all tests, got %.2f/%.2f points in total' % (points,
        totalpoints))

    return points


def partial_run(tests, output):
    all_tests = fs_tests()
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


def get_printable(data, maxlen=None):
    # don't consider whitespace as printable
    printable_chars = string.ascii_letters + string.digits + string.punctuation
    if isinstance(data, bytes):
        printable_chars = printable_chars.encode('utf-8')
    if maxlen:
        data = data[:maxlen]
    if all(c in printable_chars for c in data):
        return data
    else:
        return repr(data)


def run_cmd(args, allow_err=False):
    proc = subprocess.Popen(args, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE, universal_newlines=True)
    try:
        out, err = proc.communicate(timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate()
        err += 'Timeout of %d seconds expired for command "%s"' % TIMEOUT

    if proc.returncode and not allow_err:
        raise TestError('Command returned non-zero value.\n' +
                'Command: %s\nReturn code: %d\nstdout: %s\nstderr: %s' %
                (' '.join(args), proc.returncode, out, err))

    if allow_err:
        return proc.returncode, out, err
    else:
        return out, err


def randstr(minlength, maxlength=None):
    length = random.randrange(minlength, maxlength or minlength + 1)
    return ''.join(random.choice(string.ascii_lowercase) for i in range(length))


def randbin(minlength, maxlength=None):
    length = random.randrange(minlength, maxlength or minlength)
    return ''.join(chr(random.randrange(0, 256)) for i in range(length))


def randpath(depth=0, minpartlen=3, maxpartlen=6, is_dir=False, avoid=None):
    avoid = avoid or []
    if not isinstance(avoid, (list, tuple, dict)):
        avoid = [avoid]
    avoid = [fname.strip('/') for fname in avoid]

    if maxpartlen < minpartlen:
        maxpartlen = minpartlen

    while True:
        path = '/' + '/'.join(randstr(minpartlen, maxpartlen + 1)
                                for _ in range(depth + 1))
        if is_dir:
            path = path + '/'
        if path.strip('/') not in avoid:
            return path


def generate_random_contents(num_files=5, num_dirs=3, max_depth=3, avoid=None,
        prefix_dir='/'):
    conts = []

    for _ in range(num_files):
        depth = random.randrange(0, max_depth + 1)
        fname = os.path.join(prefix_dir, randpath(depth=depth).lstrip('/'))
        fconts = randstr(10, 100)
        conts.append((fname, fconts))

    for _ in range(num_dirs):
        depth = random.randrange(0, max_depth + 1)
        dname = os.path.join(prefix_dir,
                             randpath(depth=depth, is_dir=True).lstrip('/'))
        conts.append(dname)

    return conts


def check_list_eq(ref_list, test_list, item_name='entry', container='image'):
    for entry in ref_list:
        if entry not in test_list:
            raise TestError('Expected {item_name} {entry} not found in '
                    '{container} (got list {test_list})'.format(**locals()))

    for entry in test_list:
        if entry not in ref_list:
            raise TestError('Unexpected {item_name} {entry} found in '
                    '{container} (expected: {ref_list})'.format(**locals()))


def is_in_dir(checkpath, dirname):
    if not dirname.endswith('/'): dirname = dirname + '/'
    if checkpath.endswith('/'): checkpath = checkpath[:-1]
    if not checkpath.startswith(dirname):
        return False
    remainder = checkpath[len(dirname):]
    if '/' in remainder:
        return False
    return True


def path_iter(fullpath, omit_file=False):
    is_dir = fullpath.endswith('/')
    *parts, last = fullpath.strip('/').split('/')
    path = '/'
    for part in parts:
        path = path + part + '/'
        yield path

    if is_dir or not omit_file:
        yield path + last + ('/' if is_dir else '')


@contextmanager
def lowlevel_open(*args, **kwargs):
    fd = os.open(*args, **kwargs)
    try:
        yield fd
    finally:
        os.close(fd)


class Filesystem:
    def __init__(self, *spec, padding=True, avoid=None):
        self.image_path = '_checker.img'
        self.mountpoint = '/tmp/vu-os-sfsmount'
        self.files = {}
        self.dirs = []
        self.fuse_proc = None

        self.populate(spec)

        if padding:
            avoid = avoid or []
            if isinstance(avoid, (list, tuple, dict)):
                avoid = list(avoid)
            else:
                avoid = [avoid]
            avoid += list(self.dirs) + list(self.files.keys())
            padding_conts = generate_random_contents(avoid=avoid)
            self.populate(padding_conts)


    def __enter__(self):
        self.mkfs()
        self.unmount()
        self.mount()
        return self


    def __exit__(self, exc_type, exc_val, exc_tb):
        # If there was already an exception, just clean up asap, otherwise, do
        # some more sanity-checking first
        if exc_type:
            with suppress(TestError):
                self.unmount()
            if not g_debug_testerror:
                self.remove_img()
        else:
            try:
                self.fsck()
            finally:
                unmount_ok = self.unmount()
                self.remove_img()

            if not unmount_ok:
                raise TestError('Unmounting the FUSE filesystem failed, '
                        'probably indicating it has crashed previously.')


    def add_intermediate_dirs(self, full_path):
        for path in path_iter(full_path, omit_file=True):
            if path not in self.dirs:
                self.dirs.append(path)


    def populate(self, spec):
        for entry in spec:
            if isinstance(entry, (tuple, list)):
                name, contents = entry
                if name.endswith('/'):
                    raise Exception('Cannot specify file contents for a'
                            'directory: "{name}" got contents "{contents}"'
                            .format(**locals()))
            else:
                name, contents = entry, ''

            if not name.startswith('/'):
                name = '/%s' % name

            if name == '/':
                raise Exception('Cannot create root directory')

            self.add_intermediate_dirs(name)
            if not name.endswith('/'):
                self.files[name] = contents


    def unmount(self):
        did_unmount = False

        rv, out, err = run_cmd(['fusermount', '-u', self.mountpoint],
                allow_err=True)
        if not rv:
            did_unmount = True

        if os.path.isdir(self.mountpoint):
            os.rmdir(self.mountpoint)

        return did_unmount


    def remove_img(self):
        os.remove(self.image_path)


    def dump(self):
        # Ideally this function would print files/dirs in order and, and add
        # other data similar to fsck.
        print('dirs', self.dirs)
        print('files', self.files)


    def mount(self):
        os.makedirs(self.mountpoint, exist_ok=True)

        # Runs fuse binary in background mode: voids stdout/stderr, exits when
        # unmounted (e.g., with fusermount)
        run_cmd([FUSE_BIN, '--background', '-i', self.image_path,
            self.mountpoint])


    def mkfs(self):
        mkfs_spec = []
        tmpfiles = []
        tmpfile_cnt = 0
        for d in self.dirs:
            mkfs_spec.append(d)
        for name, contents in self.files.items():
            if not contents:
                mkfs_spec.append(name)
            else:
                tmpfile = '_checker_tmpfile%d' % tmpfile_cnt
                with open(tmpfile, 'wb') as f:
                    f.write(contents.encode('utf-8'))
                tmpfile_cnt += 1
                tmpfiles.append(tmpfile)
                mkfs_spec.append('%s:%s' % (name, tmpfile))

        try:
            run_cmd([MKFS, '--randomize', '--seed', str(g_rand_seed), '--quiet',
                self.image_path] + mkfs_spec)
        finally:
            for tmpfile in tmpfiles:
                os.remove(tmpfile)


    def fsck(self):
        out, _ = run_cmd([FSCK, '--list', '--md5', self.image_path])
        fsck_files, fsck_dirs = {}, []
        for line in out.splitlines():
            name = line.split()[-1]
            if name.endswith('/'):
                fsck_dirs.append(name)
            else:
                md5 = line.split()[0]
                size = int(line.split()[1], 16)
                fsck_files[name] = (md5, size)

        check_list_eq(self.dirs, fsck_dirs, 'directory')
        check_list_eq(self.files, fsck_files, 'file')

        for fname, fcontents in self.files.items():
            md5 = hashlib.md5(fcontents.encode('utf-8')).hexdigest()
            if md5 != fsck_files[fname][0]:
                fmtcontents = get_printable(fcontents, 100)
                expsize = len(fcontents)
                imgmd5 = fsck_files[fname][0]
                imgsize = fsck_files[fname][1]
                raise TestError(('Contents of {fname} do not match:\n'
                                'expected hash: {md5} (filesize {expsize})\n'
                                'hash in image: {imgmd5} (filesize {imgsize})\n'
                                '(start of) expected contents: {fmtcontents}')
                                    .format(**locals()))


    def get_host_path(self, path):
        if not path.startswith('/'):
            raise Exception('{path} is not image path: does not start with /'
                    .format(**locals()))
        return os.path.join(self.mountpoint, path[1:])


    def get_image_path(self, path, is_dir=False, should_exist=False):
        if not path.startswith('/'):
            raise Exception('{path} is not image path: does not start with /'
                    .format(**locals()))
        if path == '/':
            return path
        if is_dir and not path.endswith('/'):
            path = path + '/'
        if should_exist and path not in self.dirs and path not in self.files:
            raise TestError('get_image_path: {path} is not a known file or '
                'directory'.format(**locals()))
        return path


    def checked(func):
        """Decorator that wraps a function to do two things:
         1) Call fsck before and after the function to ensure integrity of the
            filesystem.
         2) Add an expect_error argument to the function, which ensures the
            function *will* raise that error. If an integer is provided, it is
            assumed to be the errno of an OSError.
        """
        def wrapper(self, *args, **kwargs):
            expect_error = kwargs.pop('expect_error', None)
            path = kwargs.get('path', '')
            self.fsck()
            ret = None
            try:
                ret = func(self, *args, **kwargs)
            except OSError as e:
                if not isinstance(expect_error, int):
                    raise TestError('{func.__name__} {path} returned error '
                            '{e.__class__.__name__} {e}'.format(**locals())) \
                                    from e
                if e.errno != expect_error:
                    experr = errno.errorcode[expect_error]
                    errname = errno.errorcode[e.errno]
                    raise TestError('{func.__name__} {path} returned wrong '
                            'error. Expected {experr}, got {errname} '
                            '({e.strerror})'.format(**locals())) from e
            else:
                if expect_error:
                    errname = errno.errorcode[expect_error]
                    raise TestError('{func.__name__} {path} did not return an '
                            'error, should return {errname}'.format(**locals()))
            self.fsck()
            return ret
        return wrapper


    @checked
    def check_readdir(self, path):
        path = self.get_image_path(path, is_dir=True)
        hostpath = self.get_host_path(path)
        fuse_dirs, fuse_files = [], {}
        for entry in os.listdir(hostpath):
            entrypath = os.path.join(path, entry)
            s = os.stat(os.path.join(hostpath, entry))
            if stat.S_ISDIR(s.st_mode):
                fuse_dirs.append(entrypath + '/')
            elif stat.S_ISREG(s.st_mode):
                fuse_files[entrypath] = s.st_size
            else:
                raise TestError('readdir: {entrypath} is not a directory or '
                        'regular file according to FUSE (st_mode = {s.st_mode})'
                            .format(**locals()))

        ref_dirs = []
        for dirname in self.dirs:
            if is_in_dir(dirname, path):
                ref_dirs.append(dirname)

        ref_files = {}
        for fname, fcontents in self.files.items():
            if is_in_dir(fname, path):
                ref_files[fname] = len(fcontents)

        check_list_eq(ref_dirs, fuse_dirs, 'directory', 'fuse readdir')
        check_list_eq(ref_files, fuse_files, 'file', 'fuse readdir')

        for fname, fsize in ref_files.items():
            fuse_size = fuse_files[fname]
            if fsize != fuse_size:
                raise TestError('File size of {fname} reported via FUSE is '
                        'incorrect. Expected size: {fsize}, reported size: '
                        '{fuse_size}'.format(**locals()))


    @checked
    def check_read(self, path):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        with open(hostpath, 'rb') as f:
            fuse_contents = f.read()

        expected_contents = self.files[path].encode('utf-8')
        if expected_contents != fuse_contents:
            expfmt = get_printable(expected_contents)
            fusefmt = get_printable(fuse_contents)
            raise TestError('read: Data read from {path} through FUSE did not '
                    'match expected file contents.\n'
                    'Expected:  {expfmt}\n'
                    'Data read: {fusefmt}\n'.format(**locals()))


    @checked
    def check_pread(self, path, size, offset):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        expected_contents = self.files[path].encode('utf-8')
        expected_contents = expected_contents[offset:offset + size]

        with lowlevel_open(hostpath, os.O_RDONLY) as fd:
            fuse_contents = os.pread(fd, size, offset)

        if expected_contents != fuse_contents:
            expfmt = get_printable(expected_contents)
            fusefmt = get_printable(fuse_contents)
            raise TestError('pread: Data read from {path} at offset {offset}, '
                    'size {size}, through FUSE did not match expected file '
                    'contents.\n'
                    'Expected:  {expfmt}\n'
                    'Data read: {fusefmt}\n'.format(**locals()))


    @checked
    def check_mkdir(self, path):
        path = self.get_image_path(path, is_dir=True)
        hostpath = self.get_host_path(path)

        os.makedirs(hostpath, exist_ok=True)
        self.add_intermediate_dirs(path)


    @checked
    def check_rmdir(self, path):
        path = self.get_image_path(path, is_dir=True)
        hostpath = self.get_host_path(path)

        os.rmdir(hostpath)
        self.dirs.remove(path)


    @checked
    def check_rm(self, path):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        os.remove(hostpath)
        del self.files[path]


    @checked
    def check_create(self, path):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        with lowlevel_open(hostpath, os.O_WRONLY | os.O_CREAT) as fd:
            pass

        self.files[path] = ''


    @checked
    def check_truncate(self, path, size):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        os.truncate(hostpath, size)
        self.files[path] = self.files[path][:size] + \
                '\0' * (size - len(self.files[path]))


    @checked
    def check_write(self, path, data):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        with lowlevel_open(hostpath, os.O_WRONLY) as fd:
            os.write(fd, data.encode('utf-8'))

        self.files[path] = data + self.files[path][len(data):]


    @checked
    def check_pwrite(self, path, data, offset):
        path = self.get_image_path(path)
        hostpath = self.get_host_path(path)

        with lowlevel_open(hostpath, os.O_WRONLY) as fd:
            os.pwrite(fd, data.encode('utf-8'), offset)

        if len(self.files[path]) < offset:
            self.files[path] = self.files[path] + '\0' * (offset
                    - len(self.files[path]))
        self.files[path] = self.files[path][:offset] + data + \
                self.files[path][len(data) + offset:]


    @checked
    def check_exists(self, path):
        if path not in self.dirs and path not in self.files:
            raise Exception('{path} is not a known file or directory'
                    .format(**locals()))

        hostpath = self.get_host_path(path)
        try:
            os.stat(hostpath)
        except FileNotFoundError:
            raise TestError('Existing file {path} not found on filesystem '
                    'through FUSE'.format(**locals()))


    @checked
    def check_not_exists(self, path):
        if path in self.dirs or path in self.files:
            raise Exception('{path} exists on filesystem'.format(**locals()))

        hostpath = self.get_host_path(path)
        try:
            os.stat(hostpath)
        except FileNotFoundError:
            pass
        else:
            raise TestError('Non-existing file {path} was found on filesystem '
                    'through FUSE'.format(**locals()))


def test_list_root_files():
    emptyfile = randpath()
    conts = generate_random_contents(num_dirs=0, max_depth=0, avoid=emptyfile)
    with Filesystem(emptyfile, *conts, padding=False) as fs:
        fs.check_readdir('/')


def test_list_root_dirs():
    conts = generate_random_contents(num_files=0)
    with Filesystem(
                '/averylongdirectorynamethatjustneverseemstoendandjustkeeps/',
                *conts,
                padding=False
            ) as fs:
        fs.check_readdir('/')


def test_list_root_full():
    conts = ('file' + str(i) + random.choice(('', '/')) for i in range(64))
    with Filesystem(*conts, padding=False) as fs:
        fs.check_readdir('/')


def test_list_root_random():
    with Filesystem() as fs:
        fs.check_readdir('/')


def test_read_root(what):
    def _inner():
        target_filename = randpath()
        with Filesystem((target_filename, what)) as fs:
            fs.check_read(target_filename)
    return _inner


def test_read_noexist_root():
    existing_file = randpath()
    nonexisting_file = randpath(avoid=existing_file)
    with Filesystem(existing_file, avoid=nonexisting_file) as fs:
        fs.check_exists(existing_file)
        fs.check_not_exists(nonexisting_file)


def test_read_offset_root():
    fname = randpath()
    fconts = randstr(6 * 4096, 8 * 4096)
    with Filesystem((fname, fconts)) as fs:
        size = random.randrange(512 + 1, 4096)
        off = random.randrange(3 * 4096, 5 * 4096)
        fs.check_pread(fname, size, off)


def test_list_subdir_1():
    subdir = randpath(is_dir=True)
    subdir_conts = generate_random_contents(prefix_dir=subdir)
    with Filesystem(*subdir_conts) as fs:
        fs.check_readdir(subdir)


def test_list_subdir_n():
    subdir = randpath(depth=random.randrange(5, 10), is_dir=True)
    subdir_conts = generate_random_contents(prefix_dir=subdir)
    with Filesystem(*subdir_conts) as fs:
        fs.check_readdir(subdir)


def test_list_subdir_noexist():
    subdir = randpath(is_dir=True)
    subdir_conts = generate_random_contents(prefix_dir=subdir)
    subdir_invalid = randpath(is_dir=True)
    with Filesystem(*subdir_conts, avoid=subdir_invalid) as fs:
        fs.check_readdir(subdir)
        fs.check_readdir(subdir_invalid, expect_error=errno.ENOENT)


def test_read_subdir_1():
    fname = randpath(depth=1)
    with Filesystem((fname, randstr(8, 12))) as fs:
        fs.check_read(fname)


def test_read_subdir_n():
    fname = randpath(depth=random.randrange(5, 10))
    with Filesystem((fname, randstr(8, 12))) as fs:
        fs.check_read(fname)


def test_read_subdir_noexist():
    validfile = randpath(depth=1)
    invalidfile = randpath(depth=1)
    with Filesystem((validfile, randstr(8, 12)), avoid=invalidfile) as fs:
        fs.check_read(validfile)
        fs.check_read(invalidfile, expect_error=errno.ENOENT)


def test_mkdir_1():
    dirname = randpath(is_dir=True)
    with Filesystem(avoid=dirname) as fs:
        fs.check_mkdir(dirname)


def test_mkdir_n():
    dirname = randpath(depth=random.randrange(5, 10), is_dir=True)
    with Filesystem(avoid=dirname) as fs:
        fs.check_mkdir(dirname)


def test_mkdir_toolong():
    validname = randpath(minpartlen=57, is_dir=True)
    validtree = randpath(depth=60, minpartlen=1, maxpartlen=1, is_dir=True)
    invalidname = randpath(minpartlen=58, is_dir=True)
    with Filesystem(avoid=(validname, validtree)) as fs:
        # First, check if mkdir works in the first place
        fs.check_mkdir(validname)

        # And whether the limit is imposed on each subdir, not the full path
        fs.check_mkdir(validtree)

        # Then, try to create the too long entry and see if we get error
        fs.check_mkdir(invalidname, expect_error=errno.ENAMETOOLONG)


def test_rmdir_root():
    target = randpath(is_dir=True)
    with Filesystem(target) as fs:
        fs.check_rmdir(target)


def test_rmdir_tree():
    target = randpath(depth=5, is_dir=True)
    with Filesystem(target) as fs:
        for path in list(path_iter(target))[::-1]:
            fs.check_rmdir(path)


def test_rmdir_nonempty():
    emptydir = randpath(is_dir=True)
    target = randpath(depth=1)
    with Filesystem(emptydir, target) as fs:
        # Test if rmdir works normally
        fs.check_rmdir(emptydir)

        # Try to remove the non-empty dir
        fs.check_rmdir(os.path.dirname(target), expect_error=errno.ENOTEMPTY)


def test_rm_empty():
    emptyfile = randpath()
    with Filesystem(emptyfile) as fs:
        fs.check_rm(emptyfile)


def test_rm_small():
    smallfile = randpath()
    fconts = randstr(32, 500)
    with Filesystem((smallfile, fconts)) as fs:
        fs.check_rm(smallfile)


def test_rm_large():
    largefile = randpath()
    fconts = randstr(4096, 8192)
    with Filesystem((largefile, fconts)) as fs:
        fs.check_rm(largefile)


def test_rm_subdir():
    subdir = randpath(is_dir=True)
    subdir_file = subdir + randpath()[1:]
    subdir_padding = generate_random_contents(prefix_dir=subdir,
            avoid=subdir_file)
    fconts = randstr(32, 500)
    with Filesystem((subdir_file, fconts), *subdir_padding) as fs:
        fs.check_rm(subdir_file)


def test_create_root():
    fname = randpath()
    with Filesystem(avoid=fname) as fs:
        fs.check_create(fname)


def test_create_subdir():
    subdir = randpath(depth=random.randrange(5, 10), is_dir=True)
    fname = subdir + randpath()[1:]
    with Filesystem(subdir) as fs:
        fs.check_create(fname)


def test_create_toolong():
    subdir = randpath(depth=60, minpartlen=1, maxpartlen=1, is_dir=True)
    validfile = subdir + randpath(minpartlen=57)[1:]
    invalidfile = subdir + randpath(minpartlen=58)[1:]
    with Filesystem(subdir, avoid=validfile) as fs:
        fs.check_create(validfile)
        fs.check_create(invalidfile, expect_error=errno.ENAMETOOLONG)


def test_truncate_grow():
    emptyfile = randpath()
    smallfile = randpath()
    mediumfile = randpath()
    alignedfile = randpath()

    with Filesystem((emptyfile, ''),
                    (smallfile, randstr(400)),
                    (mediumfile, randstr(512 * 3 + 300)),
                    (alignedfile, randstr(512))) as fs:

        # Test truncating to same size
        fs.check_truncate(emptyfile, 0)
        fs.check_truncate(smallfile, 400)
        fs.check_truncate(mediumfile, 512 * 3 + 300)
        fs.check_truncate(alignedfile, 512)

        # Then grow each by at least 1 block
        fs.check_truncate(emptyfile, 800)
        fs.check_truncate(smallfile, 1100)
        fs.check_truncate(mediumfile, 512 * 5 + 200)
        fs.check_truncate(alignedfile, 512 * 3)


def test_truncate_shrink():
    emptyfile = randpath()
    smallfile = randpath()
    mediumfile = randpath()
    alignedfile = randpath()

    with Filesystem((emptyfile, ''),
                    (smallfile, randstr(400)),
                    (mediumfile, randstr(512 * 3 + 300)),
                    (alignedfile, randstr(512 * 4))) as fs:

        # Test truncating to same size
        fs.check_truncate(emptyfile, 0)
        fs.check_truncate(smallfile, 400)
        fs.check_truncate(mediumfile, 512 * 3 + 300)
        fs.check_truncate(alignedfile, 512 * 4)

        # Shrink each file by a bit
        fs.check_truncate(smallfile, 222)
        fs.check_truncate(mediumfile, 512 * 1 + 100)
        fs.check_truncate(alignedfile, 512 * 3)

        # Shrink each file to 0
        fs.check_truncate(smallfile, 0)
        fs.check_truncate(mediumfile, 0)
        fs.check_truncate(alignedfile, 0)


def test_write_simple():
    emptyfile = randpath()
    with Filesystem(emptyfile) as fs:
        fs.check_write(emptyfile, randstr(4, 10))
        fs.check_write(emptyfile, randstr(100, 200))
        fs.check_write(emptyfile, randstr(512))


def test_write_multiblock():
    emptyfile = randpath()
    with Filesystem(emptyfile) as fs:
        fs.check_write(emptyfile, randstr(1100, 1400))
        fs.check_write(emptyfile, randstr(2700, 3000))


def test_write_subset():
    emptyfile = randpath()
    with Filesystem(emptyfile) as fs:
        fs.check_write(emptyfile, randstr(1600, 2000))
        fs.check_write(emptyfile, randstr(600, 900))


def test_write_offset():
    emptyfile = randpath()
    somefile = randpath()
    alignedfile = randpath()
    with Filesystem(emptyfile,
                    (somefile, randstr(1600, 2000)),
                    (alignedfile, randstr(512 * 3))) as fs:
        fs.check_pwrite(emptyfile, randstr(600, 800),
                random.randrange(600, 800))
        fs.check_pwrite(emptyfile, randstr(1600, 2000),
                random.randrange(1100, 1500))
        fs.check_pwrite(somefile, randstr(600, 700), random.randrange(600, 700))
        fs.check_pwrite(alignedfile, randstr(512), 512)
        fs.check_pwrite(alignedfile, randstr(512), 512 * 5)


def check_warnings():
    if g_compiler_warnings is not None:
        raise TestError('Got compiler warnings:\n%s' % g_compiler_warnings)


def check_compile():
    run_cmd(['make', 'clean'])

    out, err = run_cmd(['make'])
    err = '\n'.join([l for l in err.split('\n') if not l.startswith('make:')])
    if 'warning' in err:
        global g_compiler_warnings
        g_compiler_warnings = err

    run_cmd([FUSE_BIN, '-h'])


def main():
    os.chdir(os.path.dirname(sys.argv[0]) or '.')

    parser = argparse.ArgumentParser(
        description='Run automated tests for the FS assignment, and output '
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
        '-d',
        '--debug-testerror',
        action='store_true',
        help='halt on the first test error, and preserve the test image',
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
    global g_rand_seed
    g_rand_seed = seed
    output.write('Using random seed %d (use --seed %d to repeat this run)'
            % (seed, seed))

    global g_debug_testerror
    g_debug_testerror = args.debug_testerror

    try:
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
