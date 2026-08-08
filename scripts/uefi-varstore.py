#!/usr/bin/env python3
"""Reads and edits a UEFI variable store, of the kind a firmware build
keeps in its writable flash region.

This exists so that the guest's boot path can be *stated* rather than
discovered. Left to itself the firmware enumerates whatever it finds
bootable and boots the first thing that answers, which on a machine
carrying several boot loaders means the choice is made by a menu, a
timeout, or the order two devices happened to enumerate in. None of
those are things a test can hold still. One boot option, written here,
removes the question.

The format is the one EDK2 writes: a firmware volume header, then a
variable store header, then a run of variable records each of which is
a fixed header followed by a UCS-2 name and its data, aligned to four
bytes. A record is deleted in place by clearing bits in its state byte
rather than by moving what follows it, which is why deleting never has
to rewrite the store.
"""
import argparse
import struct
import sys
import uuid

# The firmware volume that holds the variable store, and the two store
# signatures EDK2 uses - one for a build that authenticates variables
# and one for a build that does not. Only the header shape differs.
NV_DATA_FV_GUID = uuid.UUID('fff12b8d-7696-4c8b-a985-2747075b4f50')
AUTH_VARIABLE_GUID = uuid.UUID('aaf32c78-947b-439a-a180-2e144ec37792')
VARIABLE_GUID = uuid.UUID('ddcf3616-3275-4164-98b6-fe85707ffe7d')

GLOBAL_VARIABLE_GUID = uuid.UUID('8be4df61-93ca-11d2-aa0d-00e098032b8c')

VAR_START_ID = 0x55AA
VAR_ADDED = 0x3F
VAR_DELETED = 0xFD

# Non volatile, boot service and runtime accessible: what a boot option
# has to be to survive a power cycle and still be visible to an OS.
BOOT_OPTION_ATTRIBUTES = 0x07

AUTH_HEADER = '<HBBIQ16sIII16s'
AUTH_HEADER_SIZE = struct.calcsize(AUTH_HEADER)
PLAIN_HEADER = '<HBBIII16s'
PLAIN_HEADER_SIZE = struct.calcsize(PLAIN_HEADER)


class store:
    """A variable store opened over the bytes of a firmware image."""

    def __init__(self, data):
        self.data = bytearray(data)

        # The firmware volume header says how long it is, which is where
        # the variable store begins. Reading it rather than assuming a
        # fixed offset is what makes this work across firmware builds.
        fv_guid = uuid.UUID(bytes_le=bytes(self.data[16:32]))
        if fv_guid != NV_DATA_FV_GUID:
            raise ValueError(f'not a variable firmware volume: {fv_guid}')

        fv_length, = struct.unpack_from('<Q', self.data, 32)
        header_length, = struct.unpack_from('<H', self.data, 48)

        self.store_offset = header_length
        signature = uuid.UUID(
            bytes_le=bytes(self.data[header_length:header_length + 16]))

        if signature == AUTH_VARIABLE_GUID:
            self.authenticated = True
        elif signature == VARIABLE_GUID:
            self.authenticated = False
        else:
            raise ValueError(f'not a variable store: {signature}')

        self.store_size, = struct.unpack_from(
            '<I', self.data, header_length + 16)
        self.fv_length = fv_length
        self.first = header_length + 28

    def header_size(self):
        return AUTH_HEADER_SIZE if self.authenticated else PLAIN_HEADER_SIZE

    def parse_header(self, at):
        if self.authenticated:
            (start, state, _, attributes, _, _, _,
             name_size, data_size, vendor) = struct.unpack_from(
                AUTH_HEADER, self.data, at)
        else:
            (start, state, _, attributes,
             name_size, data_size, vendor) = struct.unpack_from(
                PLAIN_HEADER, self.data, at)

        return start, state, attributes, name_size, data_size, vendor

    def variables(self):
        """Every record in the store, deleted ones included.

        Deleted records are yielded too, because walking past one needs
        its sizes, and because a caller asking what is in the store
        wants to see that a name is present but dead rather than
        absent.
        """
        at = self.first

        while at + self.header_size() <= self.store_size + self.store_offset:
            (start, state, attributes, name_size, data_size,
             vendor) = self.parse_header(at)

            # 0xffff is erased flash, which is where the used part of
            # the store ends.
            if start != VAR_START_ID:
                break

            name_at = at + self.header_size()
            data_at = name_at + name_size
            name = bytes(
                self.data[name_at:data_at]).decode('utf-16-le').rstrip('\0')

            yield {
                'offset': at,
                'state': state,
                'attributes': attributes,
                'name': name,
                'guid': uuid.UUID(bytes_le=vendor),
                'data_offset': data_at,
                'data': bytes(self.data[data_at:data_at + data_size]),
                'live': state == VAR_ADDED,
            }

            at = (data_at + data_size + 3) & ~3

        self.free = at

    def free_offset(self):
        list(self.variables())
        return self.free

    def delete(self, offset):
        """Marks a record dead without moving anything after it."""
        state = self.data[offset + 2]
        self.data[offset + 2] = state & VAR_DELETED

    def append(self, name, guid, attributes, payload):
        at = self.free_offset()
        encoded = name.encode('utf-16-le') + b'\0\0'
        size = self.header_size() + len(encoded) + len(payload)

        if at + size > self.store_offset + self.store_size:
            raise ValueError('no room left in the variable store')

        if self.authenticated:
            struct.pack_into(
                AUTH_HEADER, self.data, at, VAR_START_ID, VAR_ADDED, 0,
                attributes, 0, b'\0' * 16, 0, len(encoded), len(payload),
                guid.bytes_le)
        else:
            struct.pack_into(
                PLAIN_HEADER, self.data, at, VAR_START_ID, VAR_ADDED, 0,
                attributes, len(encoded), len(payload), guid.bytes_le)

        body = at + self.header_size()
        self.data[body:body + len(encoded)] = encoded
        self.data[body + len(encoded):body + len(encoded) + len(payload)] = \
            payload


def hard_drive_path(number, first_lba, sectors, signature):
    """The device path node naming a GPT partition by its own identity.

    A boot option may start at this node rather than at the root of the
    bus, and the firmware then matches it against every partition it
    can see. That is what makes the option survive the disk being
    plugged somewhere else - and here, what lets it be written without
    knowing the topology the disk will appear under.
    """
    return struct.pack(
        '<BBHIQQ16sBB',
        0x04,                   # media device path
        0x01,                   # hard drive
        42,
        number,
        first_lba,
        sectors,
        signature.bytes_le,
        0x02,                   # GPT
        0x02)                   # signature is a GUID


def file_path(path):
    encoded = path.encode('utf-16-le') + b'\0\0'
    return struct.pack('<BBH', 0x04, 0x04, 4 + len(encoded)) + encoded


def end_path():
    return struct.pack('<BBH', 0x7F, 0xFF, 4)


def load_option(description, device_path, optional=b''):
    return (struct.pack('<IH', 1, len(device_path)) +
            description.encode('utf-16-le') + b'\0\0' +
            device_path + optional)


def describe(option):
    """Renders a load option enough to tell two of them apart."""
    if len(option) < 6:
        return '<truncated>'

    attributes, path_length = struct.unpack_from('<IH', option, 0)
    at = 6

    while at + 1 < len(option) and option[at:at + 2] != b'\0\0':
        at += 2

    description = option[6:at].decode('utf-16-le')
    at += 2

    path = option[at:at + path_length]
    files = []
    walked = 0

    while walked + 4 <= len(path):
        kind, subtype, length = struct.unpack_from('<BBH', path, walked)
        if length < 4 or kind == 0x7F:
            break
        if kind == 0x04 and subtype == 0x04:
            files.append(
                path[walked + 4:walked + length].decode(
                    'utf-16-le').rstrip('\0'))
        walked += length

    active = 'active' if attributes & 1 else 'inactive'
    return f'{description!r} [{active}] {" ".join(files)}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image')
    parser.add_argument('--output')
    parser.add_argument(
        '--only-boot-option', metavar='FILE',
        help='delete every boot option and write one naming this file')
    parser.add_argument('--description', default='zpp')
    parser.add_argument('--partition', type=int, default=2)
    parser.add_argument('--first-lba', type=int)
    parser.add_argument('--sectors', type=int)
    parser.add_argument('--partition-guid')
    parser.add_argument(
        '--timeout', type=int,
        help='seconds the firmware waits before booting the first option')

    arguments = parser.parse_args()
    image = store(open(arguments.image, 'rb').read())

    def is_boot_option(name):
        """Exactly a boot option, its order, or the next-boot override.

        Matched by shape rather than by prefix: there are variables
        named BootOptionSupport, BootDebugPolicyApplied and
        BootCurrent, none of which is an option, and deleting any of
        them would be a silent change to something else.
        """
        if name in ('BootOrder', 'BootNext'):
            return True

        return (len(name) == 8 and name.startswith('Boot') and
                all(c in '0123456789ABCDEF' for c in name[4:]))

    if arguments.only_boot_option:
        for variable in list(image.variables()):
            if not variable['live']:
                continue
            if variable['guid'] != GLOBAL_VARIABLE_GUID:
                continue
            if is_boot_option(variable['name']):
                print(f'  delete {variable["name"]}   '
                      f'{describe(variable["data"]) if len(variable["name"]) == 8 else ""}')
                image.delete(variable['offset'])

        path = (hard_drive_path(
                    arguments.partition, arguments.first_lba,
                    arguments.sectors,
                    uuid.UUID(arguments.partition_guid)) +
                file_path(arguments.only_boot_option) +
                end_path())

        image.append('Boot0000', GLOBAL_VARIABLE_GUID,
                     BOOT_OPTION_ATTRIBUTES,
                     load_option(arguments.description, path))
        image.append('BootOrder', GLOBAL_VARIABLE_GUID,
                     BOOT_OPTION_ATTRIBUTES, struct.pack('<H', 0))
        print(f'  write  Boot0000 -> {arguments.only_boot_option}')
        print('  write  BootOrder = 0000')

    if arguments.timeout is not None:
        for variable in list(image.variables()):
            if variable['live'] and variable['name'] == 'Timeout' and \
                    variable['guid'] == GLOBAL_VARIABLE_GUID:
                image.delete(variable['offset'])

        image.append('Timeout', GLOBAL_VARIABLE_GUID,
                     BOOT_OPTION_ATTRIBUTES,
                     struct.pack('<H', arguments.timeout))
        print(f'  write  Timeout = {arguments.timeout}')

    print(f'{arguments.image}: '
          f'{"authenticated" if image.authenticated else "plain"} store, '
          f'{image.store_size} bytes, free at {image.free_offset():#x}')

    for variable in image.variables():
        if not variable['live']:
            continue

        mark = ''
        if variable['name'].startswith('Boot') and len(
                variable['name']) == 8 and variable['guid'] == \
                GLOBAL_VARIABLE_GUID:
            mark = '  ' + describe(variable['data'])
        elif variable['name'] == 'BootOrder':
            order = struct.unpack(
                f'<{len(variable["data"]) // 2}H', variable['data'])
            mark = '  ' + ' '.join(f'{n:04X}' for n in order)

        print(f'  {variable["name"]:<24} '
              f'{len(variable["data"]):>6} bytes{mark}')

    if arguments.output:
        open(arguments.output, 'wb').write(bytes(image.data))
        print(f'wrote {arguments.output}')

    return 0


if __name__ == '__main__':
    sys.exit(main())
