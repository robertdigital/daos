#!/usr/bin/python3

"""Test code for named KVs within a container"""

import os
import sys
import json
import time
import pickle
import dbm.gnu

def load_conf():
    """Load the build config file"""

    file_self = os.path.dirname(os.path.abspath(__file__))
    json_file = None
    while True:
        new_file = os.path.join(file_self, '.build_vars.json')
        if os.path.exists(new_file):
            json_file = new_file
            break
        file_self = os.path.dirname(file_self)
        if file_self == '/':
            raise Exception('build file not found')
    ofh = open(json_file, 'r')
    conf = json.load(ofh)
    ofh.close()
    return conf

class daos_named_kv():
    """Named KV generator"""

    def __init__(self, transport, interface, puid, cuid):
        try:
            conf = load_conf()
            if sys.version_info.major < 3:
                pydir = 'python{}.{}'.format(sys.version_info.major, sys.version_info.minor)
            else:
                pydir = 'python{}'.format(sys.version_info.major)

                sys.path.append(os.path.join(conf['PREFIX'],
                                             'lib64',
                                             pydir,
                                             'site-packages'))
        except Exception:
            pass

        os.environ['CRT_PHY_ADDR_STR'] = transport
        os.environ['OFI_INTERFACE'] = interface
        os.environ['DAOS_SINGLETON_CLI'] = '1'

        self.daos = __import__('pydaos')

        self.container = self.daos.Cont(puid, cuid)
        self.root_kv = self.container.rootkv()

    def get_kv_by_name(self, name):
        """Return KV by name, create it if it doesn't exist"""

        if name in self.root_kv:
            return self.container.kv(pickle.loads(self.root_kv[name]))

        new_kv = self.container.newkv()
        self.root_kv[name] = pickle.dumps(new_kv.oid)
        return new_kv

    def get_kv_list(self):
        """Return a list of KVs"""

        for kv in self.root_kv:
            yield kv

def main():
    """Migrate data from dbm file to named DAOS KV"""

    if len(sys.argv) < 3:
        print('Need pool/container UUIDs')
        return
    PUID = sys.argv[1]
    CUID = sys.argv[2]

    my_kv = daos_named_kv('ofi+sockets',
                          'lo',
                          PUID,
                          CUID)

    print('Kvs are {}'.format(','.join(sorted(my_kv.get_kv_list()))))

    if len(sys.argv) != 5:
        print('Also need directory/file to import')
        return

    src_dir = sys.argv[3]
    src_file = sys.argv[4]

    filename = os.path.join(src_dir, src_file)
    if not os.path.exists(filename):
        print('Input file {} does not exist'.format(filename))
        return

    db = dbm.gnu.open(filename, 'c')

    kv = my_kv.get_kv_by_name(src_file)

    LENGTH_KEY = '__length'

    count = pickle.loads(db[LENGTH_KEY])

    try:
        start_count = pickle.loads(kv[LENGTH_KEY])
        start_count += 1
    except KeyError:
        start_count = 0

    start_count = 0

    print('Copying records from {} to {}'.format(start_count, count))

    start_time = time.perf_counter()

    copy_count = 0
    last_report_time = start_time
    to_copy = count - start_count + 1
    # CHANGE THIS TO MODIFY REPORTING FREQUENCY
    report_freq = 0.1

    bytes_total = 0

    try:
        for idx in range(start_count, count + 1):
            key = str(idx)
            value = db[key]
            value_len = len(value)
            bytes_total += value_len
            kv[key] = value
            kv[LENGTH_KEY] = pickle.dumps(idx)

            copy_count += 1
            now = time.perf_counter()

            if now - last_report_time > report_freq:

                time_per_record = (now - start_time) / copy_count

                # Make the remaining time in minutes so it's easier to parse
                remaining_time = int((time_per_record * (to_copy - copy_count)/60))

                print('Copied {}/{} records in {:.2f} seconds {} minutes remaining.'.format(copy_count,
                                                                                            to_copy,
                                                                                            now - start_time,
                                                                                            remaining_time))
                last_report_time = now
    except KeyboardInterrupt:
        pass

    now = time.perf_counter()
    print('Completed, Copied {} records in {:.2f} seconds.'.format(copy_count,
                                                                   now - start_time))
    print('Copied {} bytes'.format(bytes_total))

if __name__ == '__main__':
    main()
