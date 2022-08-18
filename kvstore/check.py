#!/usr/bin/env python3

import argparse
import functools
import inspect
import multiprocessing
import os
import random
import shlex
import shutil
import socket
import string
import subprocess
import sys
import threading
import time
import traceback
from contextlib import contextmanager, suppress, ExitStack


SERVER_BIN = './kvstore'
SERVER_DUMP = 'dump.dat'
SERVER_PORT = 35303

# Maximum runtime per cmd in seconds. Tests are considered failed if the
# execution took longer than this.
CMD_TIMEOUT = 20

# Maximum time a socket operation (e.g., recv) may take. Normally 1 second is
# plenty of time, but on codegrade we get random slowness and contention, so
# let's try to increase it
SOCKET_TIMEOUT = 5


# Global state - set by one (or more) test and used later to subtract points
g_compiler_warnings = None

# C files added by student - we need these during compilation
g_additional_sources = ''

# Print full traceback of any TestError, and then exit.
g_debug_testerror = False

# On server exit, dump its stdout/stderr
g_debug_print_server_out = False

# PID of an existing server to use (instead of launching a new one every time)
g_debug_server_pid = 0


# flake8: noqa: E128
def server_tests():
    return [
        TestGroup('Valid submission', 'compile', 1.0,
            Test('Make', check_compile),
            Test('Connect and ping', test_connect),
            stop_if_fail=True
        ),
        TestGroup('Compiler warnings', '', -1,
            Test('No warnings', check_warnings),
        ),
        TestGroup('SET command', 'set', 2,
            Test('Simple', test_set_simple),
            Test('Overwrite', test_set_overwrite),
            Test('Big value', test_set_bigval),
            Test('Big key', test_set_bigkey),
            Test('Binary', test_set_binary),
            Test('Many', test_set_many),
            Test('Abort', test_set_abort),
            threshold=1.5
        ),
        TestGroup('GET command', 'get', 1,
            Test('Simple', test_get_simple),
            Test('Non-existing', test_get_nonexisting),
            Test('Big value', test_get_bigval),
            Test('Big key', test_get_bigkey),
            Test('Binary', test_get_binary),
            Test('Many', test_get_many),
            Test('Abort', test_get_abort),
            threshold=0.5
        ),
        TestGroup('DEL command', 'del', 1,
            Test('Simple', test_del_simple),
            Test('Non-existing', test_del_nonexisting),
            Test('Big key', test_del_bigkey),
            Test('Many', test_del_many),
            threshold=0.5
        ),
        TestGroup('Basic parallelism', 'parallel', 0.5,
            Test('Has threads', test_has_threads),
            Test('Threads cleanup', test_threads_cleanup),
            Test('Parallel commands', test_parallel_ping),
            stop_if_fail=True
        ),
        TestGroup('Concurrent SET', 'concset', 1,
            Test('Parallel', test_concset_parallel),
            Test('Lock', test_concset_lock),
            Test('Lock bucket', test_concset_lock_bucket),
        ),
        TestGroup('Concurrent GET', 'concget', 1,
            Test('Parallel', test_concget_parallel),
            Test('Non-blocking', test_concget_non_blocking),
            Test('Lock', test_concget_lock),
            Test('Lock bucket', test_concget_lock_bucket),
        ),
        TestGroup('R/W lock', 'rwlock', 1,
            Test('Concurrent GETs', test_rwlock_get),
        ),
        TestGroup('Thread pool', 'pool', 1,
            Test('Has thread pool', test_threadpool),
        ),
        TestGroup('Stress', 'stress', 2,
            Test('SET random', test_stress_set_random),
            Test('SET contention', test_stress_set_contention),
            Test('GET', test_stress_get),
            Test('DEL', test_stress_del),
            Test('SET+DEL', test_stress_set_del),
            Test('SET+DEL+GET', test_stress_set_del_get),
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

    def __init__(self, fullname, codename, points, *tests, stop_if_fail=False,
                 threshold=None):
        self.fullname = fullname
        self.codename = codename
        self.points = float(points)
        self.tests = tests
        self.stop_if_fail = stop_if_fail
        self.threshold = threshold

    def run_tests(self, output):
        succeeded = 0
        for test in self.tests:
            output.write('\t' + test.name, end=': ')
            try:
                test.func()
            except TestError as e:
                output.write('FAIL', color='red')
                output.write_testerror(e)
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
            output.write(f' ({self.codename})', color='gray', end='')
        output.write()

        succeeded = self.run_tests(output)

        perc = ((1. * succeeded) / len(self.tests))
        if self.points < 0:
            perc = 1 - perc
        points = round(self.points * perc, 2)

        if self.points > 0:
            output.write(f' Passed {succeeded}/{len(self.tests)} tests, '
                         f'{points:.2f}/{self.points:.2f} points')
        else:
            if perc > 0:
                output.write(f' Failed, subtracting {abs(points):.2f} points')

        return points


def test_groups(groups, output):
    points = 0.0
    for group in groups:
        grouppoints = group.run(output)
        points += grouppoints

        if group.stop_if_fail and group.last_run_had_failing_tests:
            output.write(f' You did not pass a critical test, aborting')
            break

        if group.threshold and grouppoints < group.threshold:
            output.write(f' You did not get enough points, need at least '
                         f'{group.threshold} points in this group to continue')
            break

    return points


def full_run(output):
    points = test_groups(server_tests(), output)
    totalpoints = sum(g.points for g in server_tests() if g.points > 0)

    output.write()
    output.write(f'Executed all tests, got {points:.2f}/{totalpoints:.2f} '
                 f'points in total')

    return points


def partial_run(tests, output):
    all_tests = server_tests()
    testmap = {g.codename: g for g in all_tests if g.codename}

    points = 0.0
    for test in tests:
        if test not in testmap:
            output.write('Error: ', color='red', end='')
            output.write(f'Unknown test "{test}".', end=' ')
            output.write(f'Valid options are: {", ".join(testmap.keys())}')
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
                                       underline=underline, blink=blink,
                                       hilight=hilight)

        print(text, end=end, file=self.outfile)

    def write_traceback(self):
        exc_type, exc_value, exc_traceback = sys.exc_info()
        tb = traceback.format_exception(exc_type, exc_value, exc_traceback)
        self.write(''.join(tb), end='')

    def write_testerror(self, err):
        errs = [err]
        while err.__cause__ or (not err.__suppress_context__ and
                                err.__context__):
            err = err.__cause__ or err.__context__
            if isinstance(err, TestError):
                errs.append(err)

        for err in errs[::-1]:
            self.write('\n'.join(err.args))

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
        if bold:
            codes += C_BOLD
        if underline:
            codes += C_UNDERLINE
        if blink:
            codes += C_BLINK
        if hilight:
            codes += C_HILIGHT
        if color:
            codes += {'gray': C_GRAY,
                      'red': C_RED,
                      'green': C_GREEN,
                      'yellow': C_YELLOW,
                      'blue': C_BLUE,
                      'pink': C_PINK,
                      'cyan': C_CYAN}[color]

        return f'{codes}{val}{C_RESET}'


def param_attrs(constructor):
    params = inspect.signature(constructor).parameters
    positional = [p.name for p in params.values()
                  if p.kind == p.POSITIONAL_OR_KEYWORD]
    assert positional.pop(0) == 'self'

    @functools.wraps(constructor)
    def wrapper(self, *args, **kwargs):
        for name, param in params.items():
            if name in kwargs:
                setattr(self, name, kwargs[name])
            elif param.default != param.empty:
                setattr(self, name, param.default)

        for name, value in zip(positional, args):
            setattr(self, name, value)

        constructor(self, *args, **kwargs)

    return wrapper


class ServerError(Exception):
    @param_attrs
    def __init__(self, err_num, err_code, payload=None, cmd=None, key=None,
                 value=None):
        pass

    def __str__(self):
        return f'{self.err_num} {self.err_code} ' \
               f'payload={self.payload} cmd={self.cmd} key={self.key} ' \
               f'value={self.value}'


class MockProc:
    def __init__(self, pid):
        self.pid = pid
        self.running = True

    def communicate(self, timeout=None):
        if self.running:
            raise subprocess.TimeoutExpired('', timeout)
        return '', ''

    def poll(self):
        return not self.running

    def wait(self, timeout=None):
        if self.running:
            raise subprocess.TimeoutExpired('', timeout)

    def terminate(self):
        self.running = False

    def kill(self):
        self.running = False


class Server:
    def __init__(self):
        self.proc = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.stop()

    def start(self):
        if g_debug_server_pid:
            self.proc = MockProc(g_debug_server_pid)
        else:
            self.proc = subprocess.Popen([SERVER_BIN], stdout=subprocess.PIPE,
                                         stderr=subprocess.PIPE,
                                         universal_newlines=True)

        try:
            stdout, stderr = self.proc.communicate(timeout=0.1)
        except subprocess.TimeoutExpired:
            pass
        else:
            raise TestError(f'Server exited immediately after starting.\n'
                            f'stdout: {stdout}\n'
                            f'stderr: {stderr}')
        try:
            self.reset()
        except ServerError as e:
            self.stop()
            raise e

    def stop(self):
        if self.proc.poll():
            stdout, stderr = self.proc.communicate()
            raise TestError(f'Server was already stopped.\n'
                            f'stdout: {stdout}\n'
                            f'stderr: {stderr}')

        self.proc.terminate()
        try:
            stdout, stderr = self.proc.communicate(timeout=1)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            stdout, stderr = self.proc.communicate()

        if g_debug_print_server_out:
            print(f'Server exited normally.\n'
                  f'stdout: {stdout}\n'
                  f'stderr: {stderr}')
        else:
            if stdout or stderr:
                raise TestError(f'Your server produced output to stdout or '
                                f'stderr, which is not allowed.\n'
                                f'For debugging, use pr_info/pr_debug and the '
                                f'-v/-d flags respectively.\n'
                                f'Additionally, you can allow output '
                                f'temporarily by passing the '
                                f'--debug-print-server-out flag to check.py.\n'
                                f'Finally, you can instead run the server '
                                f'manually and tell check.py to use that, by '
                                f'passing "--debug-server-pid `pgrep kvstore`" '
                                f'to check.py.\n'
                                f'stdout: {stdout}\n'
                                f'stderr: {stderr}')

    def get_threads(self):
        server_pid = self.proc.pid
        pids, _ = check_cmd(f'ps -T -o tid --no-header --pid {server_pid}')
        pids = [int(pid) for pid in pids.splitlines()]
        pids.remove(server_pid)
        return pids

    def reset(self):
        with Client() as client:
            client.cmd('RESET')

    def dump(self, include_buckets=False):
        server_pid = self.proc.pid

        with Client() as client:
            client.cmd('DUMP')

        state = {}
        with open(f'/proc/{server_pid}/cwd/{SERVER_DUMP}', 'rb') as f:
            while True:
                line = f.readline().decode('utf-8')
                if not line:
                    break
                ltype, lval = line.split(maxsplit=1)
                if ltype == 'B':
                    curbucket = int(lval)
                    if include_buckets:
                        state[curbucket] = {}
                elif ltype == 'K':
                    key, valsize = lval.split()
                    value = f.read(int(valsize))
                    try:
                        value = value.decode('utf-8')
                    except UnicodeDecodeError as e:
                        raise TestError(f'Error parsing {SERVER_DUMP}: '
                                        f'Value for {key} was not valid '
                                        f'unicode: {get_printable(value)}.\n'
                                        f'Make sure your keys are '
                                        f'null-terminated and value and '
                                        f'value_size are set correctly') \
                                                from e
                    if f.read(1) != b'\n':
                        raise TestError(f'Error parsing {SERVER_DUMP}: '
                                        f'Expected newline at {f.tell()}')

                    correct_bucket = serverhash(key)
                    if correct_bucket != curbucket:
                        raise TestError(f'Error parsing {SERVER_DUMP}: '
                                        f'Key "{key}" should be in bucket '
                                        f'{correct_bucket} but was found in '
                                        f'{curbucket}.')

                    kvmap = state[curbucket] if include_buckets else state
                    if key in kvmap:
                        raise TestError(f'Error parsing _server_dump: '
                                        f'Duplicate key {key}')
                    kvmap[key] = value
                else:
                    raise TestError(f'Error parsing _server_dump: '
                                    f'Unexpected line type {ltype} at offset '
                                    f'{f.tell()}')
        return state


class Client:
    def __init__(self, host='localhost', port=SERVER_PORT):
        self.host = host
        self.port = port
        self.socket = None
        self.last_cmd = None

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        try:
            self.disconnect()
        except Exception as e:
            if not exc_val:
                exc_val = e
            else:
                raise e

        # Transform uncaught ServerErrors into TestErrors.
        if isinstance(exc_val, ServerError):
            msg = f'Server sent unexpected error {exc_val.err_num}: ' \
                    f'{exc_val.err_code}.'
            if exc_val.cmd:
                keyfmt = get_printable(exc_val.key or '', 32, '...')
                valfmt = get_printable(exc_val.value or '', 10, '...') \
                    if exc_val.value else ''
                msg += f'\nCommand: {exc_val.cmd} {keyfmt} {valfmt}'
            if exc_val.payload:
                msg += f'\nServer payload: {exc_val.payload}'
            raise TestError(msg)

    def connect(self, timeout=SOCKET_TIMEOUT):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(timeout)
        self.socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        starttime = time.time()
        while True:
            try:
                self.socket.connect((self.host, self.port))
                break
            except (ConnectionRefusedError,
                    ConnectionAbortedError,
                    socket.timeout) as e:
                if timeout is None or time.time() - starttime > timeout:
                    self.socket = None
                    raise TestError('Could not connect to server') from e
                time.sleep(0.1)

        self._recvbuf = b''

    def disconnect(self):
        if self.socket is not None:
            with suppress(OSError):
                self.socket.shutdown(socket.SHUT_RDWR)
                self.socket.close()
            self.socket = None

    def send(self, data):
        if isinstance(data, str):
            data = data.encode('utf-8')
        try:
            self.socket.sendall(data)
        except socket.timeout as e:
            raise TestError('Server did not want to receive our data.') from e

    def recvline(self):
        while b'\n' not in self._recvbuf:
            try:
                try:
                    recv = self.socket.recv(1024)
                except ConnectionResetError as e:
                    raise TestError(f'Connection to server was closed.\n'
                                    f'Receive buffer: {self._recvbuf}') from e
                if not recv:
                    raise TestError(f'Connection to server was closed.\n'
                                    f'Receive buffer: {self._recvbuf}')

                self._recvbuf += recv
            except socket.timeout as e:
                extra = ''
                if self.last_cmd == 'DUMP':
                    extra = '\nMake sure to support concurrent connections, ' \
                            'by creating a new thread per connection or ' \
                            'using a thread pool'
                if self._recvbuf:
                    extra += f'\nReceive buffer: {self._recvbuf}'

                raise TestError(f'Timeout, did not receive (full) reply '
                                f'from server.\n'
                                f'Last cmd: {self.last_cmd} {extra}') from e

        ret, self._recvbuf = self._recvbuf.split(b'\n', 1)
        return ret.decode('utf-8')

    def recvbuf(self, bufsize):
        while len(self._recvbuf) < bufsize:
            try:
                recv = self.socket.recv(4096)
                if not recv:
                    raise TestError(f'Connection to server was closed while '
                                    f'trying to read {bufsize} bytes, got '
                                    f'only {len(self._recvbuf)} bytes '
                                    f'({self._recvbuf}).')

                self._recvbuf += recv
            except socket.timeout as e:
                buffmt = get_printable(self._recvbuf, 128, b'...')
                raise TestError(f'Timeout, did not receive (full) reply from '
                                f'server. Expected {bufsize} bytes, got '
                                f'{len(self._recvbuf)} bytes\n'
                                f'Receive buffer: {buffmt}') from e

        ret, self._recvbuf = self._recvbuf[:bufsize], self._recvbuf[bufsize:]
        try:
            value = ret.decode('utf-8')
        except UnicodeDecodeError as e:
            raise TestError(f'Error parsing response: '
                            f'unicode: {get_printable(ret)}') \
                                    from e
        return value

    def send_cmd(self, cmd, key=None, value=None):
        self.last_cmd = cmd
        cmdline = cmd
        if key:
            cmdline = f'{cmdline} {key}'
        if value:
            cmdline = f'{cmdline} {len(value.encode("utf-8"))}'

        self.send(cmdline + '\n')
        if value:
            self.send(value + '\n')

    def recv_status(self):
        resp = self.recvline()
        try:
            err_num, err_code, payload_len = resp.split()
            err_num, payload_len = int(err_num), int(payload_len)
        except Exception as e:
            raise TestError(f'Error parsing server response.\n'
                            f'Server response: "{resp}"') from e
        return err_num, err_code, payload_len

    def recv_payload(self, payload_len):
        if payload_len == 0:
            return ''

        payload = self.recvbuf(payload_len)
        self.recvline()  # Eat the \n after payload
        return payload

    def recv_resp(self, dbg_cmd=None, dbg_key=None, dbg_value=None):
        err_num, err_code, payload_len = self.recv_status()
        payload = self.recv_payload(payload_len)
        if err_num:
            raise ServerError(err_num, err_code, payload=payload, cmd=dbg_cmd,
                              key=dbg_key, value=dbg_value)
        return payload

    def recv_resp_stalled(self, dbg_cmd=None, dbg_key=None, dbg_value=None):
        err_num, err_code, payload_len = self.recv_status()
        if err_num:
            payload = self.recv_payload(payload_len)
            raise ServerError(err_num, err_code, payload=payload, cmd=dbg_cmd,
                              key=dbg_key, value=dbg_value)
        return payload_len


    def cmd(self, cmd, key=None, value=None):
        self.send_cmd(cmd, key, value)
        return self.recv_resp(dbg_cmd=cmd, dbg_key=key, dbg_value=value)

    def cmd_stalled(self, cmd, key=None, value=None, resp=False):
        self.last_cmd = cmd
        cmdline = cmd
        if key:
            cmdline = f'{cmdline} {key}'
        if value:
            cmdline = f'{cmdline} {len(value.encode("utf-8"))}'
        self.send(cmdline + '\n')

        if resp:
            return self.recv_resp_stalled(dbg_cmd=cmd, dbg_key=key,
                                          dbg_value=value)

    def set_sock_buffer_sizes(self, size, server=True):
        # Sabotage TCP send/recv buffers so we can get write() to block on the
        # server.

        if server:
            remotesize = int(self.cmd(f'SETOPT SNDBUF {size}'))
        else:
            remotesize = 0

        self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, size)
        localsize = self.socket.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)
        # Weird, even if getsockopt reports ~2K, the actual recvbuf is 32K.
        localsize = max(localsize, 32 * 1024)

        # Sometimes, exceeding the buffers causes sadness and then recv() needs
        # a few seconds to realize there's actually more data coming.
        self.socket.settimeout(5)

        return remotesize + localsize


class TestSetup:
    # Token that indicates in the kvstate that a key was deleted
    class DeletedKeyToken:
        def __str__(self):
            return '<DELETED>'
        def __eq__(self, other):
            return isinstance(other, self.__class__)
    DELETED = DeletedKeyToken()

    def __init__(self, nclients=1):
        self.server = Server()
        self.clients = [Client() for _ in range(nclients)]
        self.multithreaded_kvstate = False
        self.global_kvstate = {}

    def __enter__(self):
        self.stack = ExitStack()
        try:
            self.stack.enter_context(self.server)
            for client in self.clients:
                self.stack.enter_context(client)
        except Exception as e:
            self.stack.close()
            raise e
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        if exc_type:
            # If there was already an exception, just clean up asap.
            try:
                self.stack.close()
            except Exception as e:
                raise e from exc_val

            # Transform uncaught ServerErrors into TestErrors.
            if isinstance(exc_val, ServerError):
                msg = f'Server sent unexpected error {exc_val.err_num}: ' \
                        f'{exc_val.err_code}.'
                if exc_val.cmd:
                    keyfmt = get_printable(exc_val.key or '', 32, '...')
                    valfmt = get_printable(exc_val.value or '', 10, '...') \
                        if exc_val.value else ''
                    msg += f'\nCommand: {exc_val.cmd} {keyfmt} {valfmt}'
                if exc_val.payload:
                    msg += f'\nServer payload: {exc_val.payload}'
                raise TestError(msg)

        else:
            try:
                self.verify()
            finally:
                self.stack.close()

    def kvstate_get(self):
        kvstate = {}
        if self.multithreaded_kvstate:
            # Merge per-thread last-stored values
            for client_kvstate in self.client_kvstate:
                for k, v in client_kvstate.items():
                    if k not in kvstate:
                        kvstate[k] = []
                    kvstate[k].append(v)
        else:
            for k, v in self.global_kvstate.items():
                kvstate[k] = (v,)

        return kvstate

    def kvstate_set(self, client, key, value):
        if isinstance(client, Client):
            client = self.clients.index(client)
        if self.multithreaded_kvstate:
            self.client_kvstate[client][key] = value
        else:
            self.global_kvstate[key] = value

    def verify(self):
        expected_state = self.kvstate_get()
        server_state = self.server.dump()
        for k, server_value in server_state.items():
            if k not in expected_state:
                kfmt = get_printable(k, 128, '...')
                raise TestError(f'Server integrity error: '
                                f'Unexpected key {kfmt} found in server dump')
            if server_value not in expected_state[k]:
                kfmt = get_printable(k, 128, '...')
                expfmt = ', '.join(get_printable(str(v), 32, '...')
                                   for v in expected_state[k])
                srvfmt = get_printable(server_value, 32, '...')
                raise TestError(f'Server integrity error: '
                                f'Value for key {kfmt} incorrect.\n'
                                f'Expected value(s): {expfmt}\n'
                                f'Server dump value: {srvfmt}')
            del expected_state[k]

        for k, v in expected_state.items():
            if self.DELETED not in v:
                kfmt = get_printable(k, 128, '...')
                raise TestError(f'Server integrity error: '
                                f'Expected key {kfmt} not found in server dump')

    def ping(self):
        for client in self.clients:
            client.cmd('PING')

    def set(self, key, value, client=0, allow_error=False):
        client = self.clients[client]
        try:
            client.cmd('SET', key, value)
        except ServerError as e:
            if not allow_error:
                raise e
            return False
        else:
            self.kvstate_set(client, key, value)

        return True

    def get(self, key, client=0, allow_error=False, check_val=True):
        client = self.clients[client]
        err = None
        try:
            value = client.cmd('GET', key)
        except ServerError as e:
            if not allow_error:
                raise e
            else:
                value = None
                err = e

        if check_val:
            global_kvstate = self.kvstate_get()
            if err:
                if err.err_code == 'KEY_ERROR':
                    if key in global_kvstate:
                        raise TestError(f'Server sent KEY_ERROR for GET {key} '
                                        f'that should exist')
                else:
                    raise e
            else:
                if key not in global_kvstate:
                    raise TestError(f'Server sent value for GET {key} while no '
                                    f'such key should exist')

                if value not in global_kvstate[key]:
                    raise TestError(f'Server sent wrong value for key {key}:\n'
                                    f'Expected value(s):'
                                    f' {", ".join(global_kvstate[key])}\n'
                                    f'Received value: {get_printable(value)}')
        return value

    def delkey(self, key, client=0, allow_error=False):
        client = self.clients[client]
        try:
            client.cmd('DEL', key)
        except ServerError as e:
            if not allow_error:
                raise e
        else:
            self.kvstate_set(client, key, self.DELETED)

    def stress_test(self, func, *args, duration=3, nclients=None,
                    expected_ops_per_worker=100):
        nclients = nclients or len(self.clients)
        if nclients < 1 or nclients > len(self.clients):
            raise ValueError(f'nclients must be between 1 and '
                             f'{len(self.clients)}')


        def _thread(num, barrier, queue):
            try:
                barrier.wait()
                ops = 0
                starttime = time.time()
                while time.time() - starttime < duration:
                    func(num, self, *args)
                    ops += 1

                kvstate = self.client_kvstate[num] \
                          if self.multithreaded_kvstate else \
                          self.global_kvstate
                queue.put({'num': num,
                           'ops': ops,
                           'kvstate': kvstate})
            except Exception as e:
                queue.put({'num': num,
                           'error': e})
                barrier.abort()

        barrier = multiprocessing.Barrier(nclients)
        queue = multiprocessing.Queue()
        threads = [multiprocessing.Process(target=_thread,
                                           args=(i, barrier, queue))
                   for i in range(nclients)]

        [t.start() for t in threads]

        # Aggregate per-process data
        if not self.multithreaded_kvstate:
            self.multithreaded_kvstate = True
            self.client_kvstate = [{} for _ in range(len(self.clients))]
            del self.global_kvstate
        ops = []
        thread_errors = []
        for _ in range(nclients):
            thread_data = queue.get()
            num = thread_data['num']
            if 'error' in thread_data:
                thread_errors.append(thread_data['error'])
            else:
                ops.append(thread_data['ops'])
                self.client_kvstate[num] = thread_data['kvstate']

        # Collect threads, and error out if any of threads had error
        [t.join() for t in threads]
        if thread_errors:
            msgs = '\n'.join(f'{e.__class__.__name__}: {e}'
                             for e in thread_errors)
            root_error = thread_errors[0]
            for e in thread_errors:
                if isinstance(e, TestError):
                    root_error = e
            raise TestError(f'One or more threads encountered an error:\n'
                            f'{msgs}') from e

        if any(num < expected_ops_per_worker * duration for num in ops):
            raise TestError(f'At least one worker did (almost) nothing.\n'
                            f'Number of operations for each worker: '
                            f'{", ".join(str(n) for n in ops)}\n'
                            f'Minimum ops per worker: '
                            f'{expected_ops_per_worker * duration}')


def serverhash(val):
    """djb2 truncated to 8 bit"""
    ret = 5381
    for c in val:
        ret = ((ret << 5) + ret + ord(c)) & 0xffffffff
    return ret & 0xff


@contextmanager
def expect_error(err):
    try:
        yield None
    except Exception as e:
        if isinstance(err, type) and isinstance(e, err):
            pass
        elif (isinstance(e, ServerError) and
              isinstance(err, str) and
              e.err_code == err):
            pass
        else:
            raise e
    else:
        raise TestError(f'Code was expected to throw {err} but did not')


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


def randstr(minlength, maxlength=None):
    length = random.randrange(minlength, maxlength or minlength + 1)
    return ''.join(random.choice(string.ascii_lowercase) for i in range(length))


def randbin(minlength, maxlength=None):
    length = random.randrange(minlength, maxlength or minlength + 1)
    return ''.join(chr(random.randrange(0, 256)) for i in range(length))


def randkeys_same_bucket(num_keys, keylen=4):
    """Returns `num_keys` keys that will fall in same bucket. Birthday paradox
    tells us this is pretty efficient."""
    assert num_keys > 0
    buckets = {}
    while True:
        key = randstr(keylen)
        keybucket = serverhash(key)
        bucket = buckets.get(keybucket, [])
        bucket.append(key)
        if len(bucket) == num_keys:
            return bucket
        buckets[keybucket] = bucket


def check_cmd(cmd, add_env=None, timeout=None):
    timeout = timeout or CMD_TIMEOUT
    args = shlex.split(cmd) if isinstance(cmd, str) else [str(c) for c in cmd]
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
        err += f'Timeout of {timeout} seconds expired - test took too long.'

    if proc.returncode:
        raise TestError(f'Command returned non-zero value.\n'
                        f'Command: {cmd}\n'
                        f'Return code: {proc.returncode}\n'
                        f'stdout: {out}\n'
                        f'stderr: {err}')
    return out, err


#
# Basic tests
#
def check_compile():
    check_cmd(f'make clean ADDITIONAL_SOURCES="{g_additional_sources}"')

    out, err = check_cmd(f'make ADDITIONAL_SOURCES="{g_additional_sources}"')
    err = '\n'.join([l for l in err.split('\n') if not l.startswith('make:')])
    if 'warning' in err:
        global g_compiler_warnings
        g_compiler_warnings = err

    check_cmd(f'{SERVER_BIN} -h')


def check_warnings():
    if g_compiler_warnings is not None:
        raise TestError(f'Got compiler warnings:\n{g_compiler_warnings}')


def test_connect():
    with Server(), Client() as client:
        client.cmd('PING')


def test_has_threads():
    with TestSetup(nclients=5) as ts:
        ts.ping()
        threads = ts.server.get_threads()
        if len(threads) < 5:
            raise TestError(f'Expected at least 5 threads with 5 concurrent '
                            f'connections open, but the server only has '
                            f'{len(threads)} threads.')

        ts.ping()
        threads2 = ts.server.get_threads()
        if len(threads) != len(threads2):
            raise TestError(f'After a PING from all clients the number of '
                            f'threads in the server changed from '
                            f'{len(threads)} to {len(threads2)}')


def test_threads_cleanup():
    # If student implements threadpool, we don't need this test
    try:
        test_threadpool()
    except TestError as e:
        threadpool_error = e
        pass  # No threadpool, so do this test
    else:
        return # Otherwise, auto-pass this test

    NCLIENTS = 64

    with Server() as server:
        basethreads = len(server.get_threads())

        if basethreads > 0:
            raise TestError(f'Server has {basethreads} threads at start, '
                            f'while none were expected (and no threadpool was '
                            f'detected, because: {threadpool_error})')

        with ExitStack() as stack:
            for _ in range(NCLIENTS):
                client = Client()
                stack.enter_context(client)
                client.cmd('PING')

            loadthreads = len(server.get_threads())
            if loadthreads != NCLIENTS:
                raise TestError(f'After launching {NCLIENTS} clients, we '
                                f'expect {NCLIENTS} threads, but only '
                                f'{loadthreads} threads were found (and no '
                                f'threadpool was detected either, because: '
                                f'{threadpool_error})')

        postthreads = len(server.get_threads())
        if postthreads > 0:
            raise TestError(f'After closing all {NCLIENTS} client connections, '
                            f'we expect no threads to remain (since no '
                            f'threadpool was detected), but server still had '
                            f'{postthreads} threads alive.\n'
                            f'No threadpool was detected because: '
                            f'{threadpool_error}')


def test_parallel_ping():
    with TestSetup(nclients=2) as ts:
        ts.clients[0].send('PI')
        ts.clients[1].cmd('PING')
        ts.clients[0].cmd('NG')


#
# SET command tests
#
def test_set_simple():
    with TestSetup() as ts:
        ts.set('hello', 'world')
        ts.set('foo', 'bar')


def test_set_overwrite():
    with TestSetup() as ts:
        key1, key2 = randstr(6), randstr(6)

        ts.set(key1, randstr(16, 128))
        ts.set(key2, randstr(16, 128))
        ts.verify()

        ts.set(key1, randstr(1, 15))
        ts.set(key2, randstr(256, 512))


def test_set_bigval():
    with TestSetup() as ts:
        ts.set('book1', randstr(4096 * 16, 4096 * 32))
        ts.set('book2', randstr(4096 * 16, 4096 * 32))
        ts.set('book3', randstr(4096 * 16, 4096 * 32))


def test_set_bigkey():
    with TestSetup() as ts:
        ts.set('longkey1' + randstr(3000, 4000), randstr(4096 * 16, 4096 * 32))
        ts.set('longkey2' + randstr(3000, 4000), randstr(4096 * 16, 4096 * 32))
        ts.set('longkey3' + randstr(3000, 4000), randstr(4096 * 16, 4096 * 32))


def test_set_binary():
    with TestSetup() as ts:
        ts.set('blob1', randbin(4096 * 1, 4096 * 4))
        ts.set('blob2', randbin(4096 * 1, 4096 * 4))


def test_set_many():
    with TestSetup() as ts:
        for _ in range(1000):
            key = randstr(4, 10)
            value = randstr(8, 4096)
            ts.set(key, value)


def test_set_abort():
    with Server() as server:
        key = randstr(4)
        with Client() as client:
            client.cmd_stalled('SET', key, randstr(8, 64))

        server_state = server.dump()
        if server_state:
            raise TestError(f'Server should have no keys stored after only an '
                            f'aborted SET {key}, but server had key-values '
                            f'stored.\n'
                            f'Server dump: {server_state}')

        val = randstr(8, 64)
        with Client() as client:
            with expect_error('KEY_ERROR'):
                client.cmd('GET', key)
            client.cmd('SET', key, val)

        server_state = server.dump()
        if key not in server_state:
            raise TestError(f'Key {key} not found in server after SET.\n'
                            f'Server dump: {server_state}')

        if server_state[key] != val:
            raise TestError(f'Key {key} has wrong value in server.\n'
                            f'Server dump: {server_state}\n'
                            f'Expected value: {val}')

        if len(server_state) != 1:
            raise TestError(f'Server contains more keys than we SET.\n'
                            f'Key we set: {key}\n'
                            f'Server dump: {server_state}')


#
# GET command tests
#
def test_get_simple():
    with TestSetup() as ts:
        ts.set('hello', 'world')
        ts.set('foo', 'bar')

        ts.get('hello')
        ts.get('foo')


def test_get_nonexisting():
    with TestSetup() as ts:
        ts.set('hello', 'world')
        ts.set('foo', 'bar')

        ts.get('hello')
        with expect_error('KEY_ERROR'):
            ts.get('baz')


def test_get_bigval():
    with TestSetup() as ts:
        ts.set('book1', randstr(4096 * 16, 4096 * 32))
        ts.set('book2', randstr(4096 * 16, 4096 * 32))
        ts.set('book3', randstr(4096 * 16, 4096 * 32))

        ts.get('book1')
        ts.get('book2')
        ts.get('book3')


def test_get_bigkey():
    with TestSetup() as ts:
        key1 = 'longkey1' + randstr(3000, 4000)
        key2 = 'longkey2' + randstr(3000, 4000)
        key3 = 'longkey3' + randstr(3000, 4000)

        ts.set(key1, randstr(4096 * 16, 4096 * 32))
        ts.set(key2, randstr(4096 * 16, 4096 * 32))
        ts.set(key3, randstr(4096 * 16, 4096 * 32))

        ts.get(key1)
        ts.get(key2)
        ts.get(key3)


def test_get_binary():
    with TestSetup() as ts:
        ts.set('blob1', randbin(4096 * 1, 4096 * 4))
        ts.set('blob2', randbin(4096 * 1, 4096 * 4))

        ts.get('blob1')
        ts.get('blob2')


def test_get_many():
    with TestSetup() as ts:
        keys = set()
        for _ in range(300):
            key = randstr(4, 10)
            value = randstr(8, 4096)
            ts.set(key, value)
            keys.add(key)

        keys = tuple(keys)  # random doesn't like sets
        for _ in range(1000):
            ts.get(random.choice(keys))


def test_get_abort():
    with TestSetup() as ts:
        key, val = randstr(4), randstr(8, 64)
        ts.set(key, val)

        with Client() as client:
            client.set_sock_buffer_sizes(0)
            client.cmd_stalled('GET', key)

        ts.get(key)

        with Client() as client:
            client.set_sock_buffer_sizes(0)
            client.cmd_stalled('GET', key, resp=True)

        ts.get(key)
        ts.set(key, randstr(8, 64))


#
# DEL command tests
#
def test_del_simple():
    with TestSetup() as ts:
        ts.set('hello', 'world')
        ts.set('foo', 'bar')

        ts.delkey('hello')
        ts.delkey('foo')


def test_del_nonexisting():
    with TestSetup() as ts:
        ts.set('hello', 'world')
        ts.set('foo', 'bar')

        ts.delkey('hello')
        with expect_error('KEY_ERROR'):
            ts.delkey('baz')


def test_del_bigkey():
    with TestSetup() as ts:
        key1 = 'longkey1' + randstr(3000, 4000)
        key2 = 'longkey2' + randstr(3000, 4000)
        key3 = 'longkey3' + randstr(3000, 4000)

        ts.set(key1, randstr(4096 * 16, 4096 * 32))
        ts.set(key2, randstr(4096 * 16, 4096 * 32))
        ts.set(key3, randstr(4096 * 16, 4096 * 32))

        ts.delkey(key1)
        ts.delkey(key2)
        ts.delkey(key3)


def test_del_many():
    with TestSetup() as ts:
        keys = set()
        for _ in range(1000):
            key = randstr(4, 10)
            value = randstr(8, 1024)
            ts.set(key, value)
            keys.add(key)
        ts.verify()

        keys = tuple(keys)
        start = random.randrange(4, 32)
        end = 900 + random.randrange(4, 32)
        for key in keys[start:end]:
            ts.delkey(key)


#
# Concurrent SET tests
#
def test_concset_parallel():
    with TestSetup(nclients=3) as ts:
        # Set some different keys from multiple clients
        ts.set(randstr(4), randstr(8), client=0)
        ts.set(randstr(4), randstr(8), client=1)
        ts.set(randstr(4), randstr(8), client=2)
        ts.verify()

        # Set the same key from multiple clients
        key = randstr(4)
        ts.set(key, randstr(8), client=0)
        ts.verify()
        ts.set(key, randstr(8), client=1)
        ts.verify()
        ts.set(key, randstr(8), client=2)
        ts.verify()


def test_concset_lock():
    with TestSetup(nclients=3) as ts:
        # This locks the key (waiting for the payload)
        key, val = randstr(4), randstr(128)
        ts.clients[0].cmd_stalled('SET', key, val)
        ts.kvstate_set(0, key, '')

        # Touching *this* key should fail
        with expect_error('KEY_ERROR'):
            ts.set(key, randstr(8), client=1)
        with expect_error('KEY_ERROR'):
            ts.set(key, randstr(8), client=2)
        ts.verify()

        # But touching another key should be fine
        ts.set(randstr(4), randstr(8), client=1)
        ts.set(randstr(4), randstr(8), client=2)
        ts.verify()

        # Finish our pending store
        ts.clients[0].send(f'{val}\n')
        ts.clients[0].recv_resp()
        ts.kvstate_set(0, key, val)
        ts.verify()

        # And now others can touch again
        ts.set(key, randstr(8), client=1)
        ts.set(key, randstr(8), client=2)
        ts.verify()


def test_concset_lock_bucket():
    with TestSetup(nclients=3) as ts:
        bkey1, bkey2, bkey3 = randkeys_same_bucket(num_keys=3, keylen=4)
        # This locks the key (waiting for the payload)
        val = randstr(8)
        ts.clients[0].cmd_stalled('SET', bkey1, val)
        ts.kvstate_set(0, bkey1, '')

        # Touching *this* key should fail
        with expect_error('KEY_ERROR'):
            ts.set(bkey1, randstr(8), client=1)
        with expect_error('KEY_ERROR'):
            ts.set(bkey1, randstr(8), client=2)
        ts.verify()

        # But touching another key *in the same bucket* should be fine
        ts.set(bkey2, randstr(8), client=1)
        ts.set(bkey3, randstr(8), client=2)
        ts.verify()

        # Finish our pending store
        ts.clients[0].send(f'{val}\n')
        ts.clients[0].recv_resp()
        ts.kvstate_set(0, bkey1, val)
        ts.verify()


#
# Concurrent GET tests
#
def test_concget_parallel():
    with TestSetup(nclients=3) as ts:
        # Create controlled situation where write() blocks serverside
        bufsize = ts.clients[1].set_sock_buffer_sizes(0)
        bufsize = ts.clients[2].set_sock_buffer_sizes(0)

        key0, val0 = randstr(4), randstr(bufsize * 3)
        key1, val1 = randstr(4), randstr(bufsize * 3)
        key2, val2 = randstr(4), randstr(bufsize * 3)
        ts.set(key0, val0)
        ts.set(key1, val1)
        ts.set(key2, val2)

        # Start some delayed GET requests
        payload1_len = ts.clients[1].cmd_stalled(f'GET {key1}', resp=True)
        payload2_len = ts.clients[2].cmd_stalled(f'GET {key2}', resp=True)

        # The other thread should be fine to do whatever
        ts.get(key0)
        ts.set(randstr(5), randstr(8))

        # Finish the two stalled reads
        ts.clients[2].set_sock_buffer_sizes(128*1024, server=False)
        payload2 = ts.clients[2].recv_payload(payload2_len)
        if payload2 != val2:
            raise TestError(f'Value returned for GET {key2} incorrect.\n'
                            f'Expected: {get_printable(val2, 128, "...")}\n'
                            f'Received: {get_printable(payload2, 128, "...")}')

        ts.clients[1].set_sock_buffer_sizes(128*1024, server=False)
        payload1 = ts.clients[1].recv_payload(payload1_len)
        if payload1 != val1:
            raise TestError(f'Value returned for GET {key1} incorrect.\n'
                            f'Expected: {get_printable(val1, 128, "...")}\n'
                            f'Received: {get_printable(payload1, 128, "...")}')




def test_concget_non_blocking():
    with TestSetup(nclients=2) as ts:
        # This locks the key (waiting for the payload)
        key, val = randstr(4), randstr(8)
        ts.clients[0].cmd_stalled('SET', key, val)
        ts.kvstate_set(0, key, '')

        # Touching this key should fail
        with expect_error('KEY_ERROR'):
            ts.get(key, client=1)

        # Finish our pending store
        ts.clients[0].send(f'{val}\n')
        ts.clients[0].recv_resp()
        ts.kvstate_set(0, key, val)
        ts.verify()

        # Now our get should succeed
        ts.get(key)


def test_concget_lock():
    with TestSetup(nclients=2) as ts:
        # Create controlled situation where write() blocks serverside
        bufsize = ts.clients[1].set_sock_buffer_sizes(0)

        key, val = randstr(4), randstr(bufsize * 3)
        ts.set(key, val)

        # This locks the key (waiting for the payload)
        payload_len = ts.clients[1].cmd_stalled(f'GET {key}', resp=True)

        with expect_error('KEY_ERROR'):
            ts.set(key, randstr(8))

        with expect_error('KEY_ERROR'):
            ts.delkey(key)

        # Set RCVBUF to something reasonable again and start reading response
        # (we cannot restore server SNDBUF because server is stuck writing)
        ts.clients[1].set_sock_buffer_sizes(128*1024, server=False)
        payload = ts.clients[1].recv_payload(payload_len)

        if payload != val:
            raise TestError(f'Value returned for GET {key} incorrect.\n'
                            f'Expected: {get_printable(val, 128, "...")}\n'
                            f'Received: {get_printable(payload, 128, "...")}')

        # Restore everything back now, and verify entry is not locked anymore
        ts.clients[1].set_sock_buffer_sizes(128*1024)
        ts.set(key, randstr(8))
        ts.verify()
        ts.delkey(key)


def test_concget_lock_bucket():
    with TestSetup(nclients=2) as ts:
        bkey1, bkey2, bkey3 = randkeys_same_bucket(num_keys=3, keylen=4)

        # This locks the key (waiting for the payload)
        val = randstr(8)
        ts.clients[0].cmd_stalled('SET', bkey1, val)
        ts.kvstate_set(0, bkey1, '')

        # Touching this key should fail
        with expect_error('KEY_ERROR'):
            ts.get(bkey1, client=1)

        # But other keys in same bucket should be fine
        ts.set(bkey2, randstr(8), client=1)
        ts.set(bkey3, randstr(8), client=1)
        ts.get(bkey2, client=1)
        ts.get(bkey3, client=1)

        # Finish our pending store
        ts.clients[0].send(f'{val}\n')
        ts.clients[0].recv_resp()
        ts.kvstate_set(0, bkey1, val)
        ts.verify()

        # Now get should succeed
        ts.get(bkey1, client=1)


#
# Read/Write lock tests
#
def test_rwlock_get():
    with TestSetup(nclients=3) as ts:
        # Create controlled situation where write() blocks serverside
        bufsize = ts.clients[1].set_sock_buffer_sizes(0)

        key, val = randstr(4), randstr(bufsize * 3)
        ts.set(key, val)

        # This locks the key (waiting for the payload)
        payload1_len = ts.clients[1].cmd_stalled(f'GET {key}', resp=True)

        # Writes should be disallowed while we're doing the read
        with expect_error('KEY_ERROR'):
            ts.set(key, randstr(8))

        # But other reads should be fine
        ts.get(key)
        ts.get(key, client=2)

        # Start another delayed read
        bufsize = ts.clients[2].set_sock_buffer_sizes(0)
        payload2_len = ts.clients[2].cmd_stalled(f'GET {key}', resp=True)

        # And complete the first delayed read
        ts.clients[1].set_sock_buffer_sizes(128*1024, server=False)
        payload1 = ts.clients[1].recv_payload(payload1_len)
        ts.clients[1].set_sock_buffer_sizes(128*1024)
        if payload1 != val:
            raise TestError(f'Value returned for GET {key} incorrect.\n'
                            f'Expected: {get_printable(val, 128, "...")}\n'
                            f'Received: {get_printable(payload1, 128, "...")}')

        # Writes should still be disallowed
        with expect_error('KEY_ERROR'):
            ts.set(key, randstr(8))

        # But other reads should be fine
        ts.get(key)
        ts.get(key, client=1)

        # Complete the second delayed read, this should release the lock
        ts.clients[2].set_sock_buffer_sizes(128*1024, server=False)
        payload2 = ts.clients[2].recv_payload(payload1_len)
        ts.clients[2].set_sock_buffer_sizes(128*1024)
        if payload2 != val:
            raise TestError(f'Value returned for GET {key} incorrect.\n'
                            f'Expected: {get_printable(val, 128, "...")}\n'
                            f'Received: {get_printable(payload2, 128, "...")}')

        ts.get(key, client=0)
        ts.get(key, client=1)
        ts.get(key, client=2)

        ts.set(key, randstr(8))


#
# Threadpool tests
#
def test_threadpool():
    with Server() as server:
        basethreads = set(server.get_threads())

        if len(basethreads) == 0:
            raise TestError('No threads found after starting server')

        if len(basethreads) < 5:
            raise TestError(f'Insufficient threads found for thread pool. '
                            f'Found {len(basethreads)} threads.')

        with Client() as c1, Client() as c2, Client():
            c1.cmd('PING')
            c2.cmd('PING')
            c1.cmd('PING')
            threads = set(server.get_threads())
            if basethreads ^ threads:
                raise TestError(f'Different threads found after creating '
                                f'client connections.\n'
                                f'Original threads: {basethreads}\n'
                                f'Current threads:  {threads}')

        with Client() as c1, Client() as c2, Client():
            c1.cmd('PING')
            c2.cmd('PING')
            c1.cmd('PING')
            threads = set(server.get_threads())
            if basethreads ^ threads:
                raise TestError(f'Different threads found after closing and '
                                f'creating more client connections.\n'
                                f'Original threads: {basethreads}\n'
                                f'Current threads:  {threads}')

        threads = set(server.get_threads())
        if basethreads ^ threads:
            raise TestError(f'Different threads found after closing all '
                            f'client connections.\n'
                            f'Original threads: {basethreads}\n'
                            f'Current threads:  {threads}')


#
# Stress tests
#
def test_stress_set_random():
    def op(client_id, ts):
        key = randstr(4, 10)
        value = randstr(8, 4096)
        ts.set(key, value, client=client_id, allow_error=True)

    with TestSetup(nclients=10) as ts:
        ts.stress_test(op)


def test_stress_set_contention():
    def op(client_id, ts, keys):
        if random.random() < 0.1:
            # Sometimes pick another key, to cause potential races in buckets
            key = randstr(4)
        else:
            key = random.choice(keys)
        value = randstr(8)
        ts.set(key, value, client=client_id, allow_error=True)

    with TestSetup(nclients=10) as ts:
        keys = tuple(randstr(4, 6) for _ in range(10))
        ts.stress_test(op, keys)


def test_stress_get():
    def op(client_id, ts, keys):
        key = random.choice(keys)
        ts.get(key, client=client_id, allow_error=True)

    with TestSetup(nclients=10) as ts:
        validkeys = set()
        for _ in range(50):
            key, value = randstr(4, 10), randstr(8, 4 * 4096)
            ts.set(key, value)
            validkeys.add(key)
        invalidkeys = set(randstr(4, 10) for _ in range(10))
        keys = tuple(validkeys.union(invalidkeys))

        ts.stress_test(op, keys)


def test_stress_del():
    def op(client_id, ts, keys):
        key = random.choice(keys)
        ts.delkey(key, client=client_id, allow_error=True)

    with TestSetup(nclients=10) as ts:
        for _ in range(10):
            ts.set(randstr(4, 10), randstr(8, 4 * 4096))
        validkeys = set()
        for _ in range(1000):
            key, value = randstr(4, 10), randstr(8, 4 * 4096)
            ts.set(key, value)
            validkeys.add(key)
        invalidkeys = set(randstr(4, 10) for _ in range(10))
        keys = tuple(validkeys.union(invalidkeys))

        ts.stress_test(op, keys)


def test_stress_set_del():
    def op(client_id, ts, keys):
        if random.random() < 0.1:
            key = randstr(4)
        else:
            key = random.choice(keys)

        if random.random() < 0.5:
            ts.set(key, randstr(8), client=client_id, allow_error=True)
        else:
            ts.delkey(key, client=client_id, allow_error=True)

    with TestSetup(nclients=10) as ts:
        keys = tuple(set(randstr(4, 6) for _ in range(100)))
        ts.stress_test(op, keys)


def test_stress_set_del_get():
    def op(client_id, ts):
        action = random.choice(('set', 'del', 'get'))
        key = randstr(3) if random.random() < 0.5 else random.choice(keys)
        if action == 'set':
            ts.set(key, randstr(8), client=client_id, allow_error=True)
        elif action == 'del':
            ts.delkey(key, client=client_id, allow_error=True)
        else:
            ts.get(key, client=client_id, allow_error=True, check_val=False)

    with TestSetup(nclients=10) as ts:
        keys = tuple(set(randstr(4, 6) for _ in range(10)))
        ts.stress_test(op)


def do_additional_params(lst, name, suffix=''):
    for f in lst:
        if not f.endswith(suffix):
            raise TestError(f'File does not end with {suffix} in {name}: '
                            f'"{f}"')
        if '"' in f:
            raise TestError(f'No quotes allowed in {name}: "{f}"')
        if '/' in f:
            raise TestError(f'No slashes allowed in {name}: "{f}"')
        if '$' in f:
            raise TestError(f'No $ allowed in {name}: "{f}"')
        if f.startswith('-'):
            raise TestError(f'No flags allowed in {name}: "{f}"')


def fix_makefiles():
    with open('Makefile', 'r') as f:
        addsrc, addhdr = [], []
        for line in f:
            line = line.strip()
            if line.startswith('ADDITIONAL_SOURCES = '):
                addsrc = list(filter(bool, line.split(' ')[2:]))
            if line.startswith('ADDITIONAL_HEADERS = '):
                addhdr = list(filter(bool, line.split(' ')[2:]))
    do_additional_params(addsrc, 'ADDITIONAL_SOURCES', '.c')
    do_additional_params(addhdr, 'ADDITIONAL_HEADERS', '.h')

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
        '--debug-print-server-out',
        action='store_true',
        help='print stdout/stderr of server (after every test)',
    )
    parser.add_argument(
        '--debug-server-pid',
        type=int,
        help='specify PID of an already running server (skip starting server)',
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

    global g_debug_print_server_out
    g_debug_print_server_out = args.debug_print_server_out

    global g_debug_server_pid
    g_debug_server_pid = args.debug_server_pid

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
