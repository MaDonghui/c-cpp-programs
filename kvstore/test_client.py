import argparse

from check import Client, ServerError, SERVER_PORT


def run_oneshot(client, cmd, key, value=None):
    try:
        resp = client.cmd(cmd.upper(), key, value)
        if resp:
            print(f'OK: {resp}')
        else:
            print('OK')
    except ServerError as err:
        print(f'ERROR: {err}')


def run_interactive(client):
    try:
        while True:
            cmd, key, value = None, None, None
            while not cmd:
                cmd = input('> ')
            if ' ' in cmd:
                cmd, key = cmd.split(' ', 1)
            if cmd.lower() not in ('set', 'get', 'del', 'ping', 'reset',
                                   'exit'):
                print(f'Unknown command: {cmd}')
                continue

            if not key and cmd in ('set', 'get', 'del'):
                while not key:
                    key = input('Key: ')
            if ' ' in key:
                key, value = key.split(' ', 1)

            if not value and cmd == 'set':
                while not value:
                    value = input('Value: ')

            run_oneshot(client, cmd, key, value)

    except EOFError:
        print()


def main():
    parser = argparse.ArgumentParser(description='Run manual tests on the '
                                                 'server')
    parser.add_argument('--host', default='127.0.0.1',
                        help='host of the server')
    parser.add_argument('-p', '--port', default=SERVER_PORT, type=int,
                        help='port of the server')
    parser.add_argument('-i', '--interactive', action='store_true',
                        help='read multiple commands from stdin')
    subparsers = parser.add_subparsers(dest='cmd', title='commands')

    parser_set = subparsers.add_parser('set')
    parser_set.add_argument('key')
    parser_set.add_argument('value')

    parser_get = subparsers.add_parser('get')
    parser_get.add_argument('key')

    parser_del = subparsers.add_parser('del')
    parser_del.add_argument('key')

    parser_del = subparsers.add_parser('reset')
    parser_del = subparsers.add_parser('ping')
    parser_del = subparsers.add_parser('exit')

    args = parser.parse_args()

    if args.cmd and args.interactive:
        print('Error: Cannot specify both --interactive and a command')
        print()
        parser.print_help()
        exit(1)

    if not args.cmd and not args.interactive:
        print('Error: Must specify either command or --interactive')
        print()
        parser.print_help()
        exit(1)

    with Client(args.host, args.port) as client:
        if args.interactive:
            run_interactive(client)
        else:
            run_oneshot(client, args.cmd,
                        args.key if 'key' in args else None,
                        args.value if 'value' in args else None)


if __name__ == '__main__':
    main()
