#!/usr/bin/python3

# Copyright (c) 2024 Oracle.  All rights reserved.
# SPDX-License-Identifier: GPL-2.0
#
# cat the regular files passed in, or the child files in a directory

import sys
import argparse
from pathlib import Path

CHUNK_SIZE = 65536

def dump_path(path, dump_dirs, end):
	'''Dump a specific file to stdout.'''
	global CHUNK_SIZE
	ret = 0

	try:
		last_newline = end == '\n'
		with open(path, 'rb') as fd:
			first_line = True
			while True:
				chunk = fd.read(CHUNK_SIZE)
				if chunk == b'':
					break
				if first_line:
					print('%s:' % path, end = end)
					sys.stdout.flush()
					first_line = False
				last_newline = chunk.endswith(b'\n')
				sys.stdout.buffer.write(chunk)
			if not last_newline:
				print()
			sys.stdout.flush()
	except IsADirectoryError:
		if dump_dirs:
			p = Path(path)
			for child in p.iterdir():
				ret |= dump_path(child, False, end)
	except Exception as e:
		print(e)
		ret |= 1

	return ret

def main():
	ret = 0
	end = ''

	p = argparse.ArgumentParser(
			description = 'Print the passed in files on standard output.')
	p.add_argument('-n', dest = 'newlines', action = 'store_true',
			help = 'Print a newline after printing each path.')
	p.add_argument(
		dest = 'paths', type = str, nargs = '+',
		help = 'Paths to print.')
	args = p.parse_args()
	if args.newlines:
		end = '\n'

	for path in args.paths:
		ret |= dump_path(path, True, end)

	return ret

if __name__ == '__main__':
	sys.exit(main())
