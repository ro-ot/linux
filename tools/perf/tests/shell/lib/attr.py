# SPDX-License-Identifier: GPL-2.0

import configparser
import os
import sys
import glob
import optparse
import platform
import tempfile
import logging
import re
import shutil
import subprocess

def data_equal(a, b):
    # Allow multiple values in assignment separated by '|'
    a_list = a.split('|')
    b_list = b.split('|')

    for a_item in a_list:
        for b_item in b_list:
            if (a_item == b_item):
                return True
            elif (a_item == '*') or (b_item == '*'):
                return True

    return False

class Fail(Exception):
    def __init__(self, test, msg):
        self.msg = msg
        self.test = test
    def getMsg(self):
        return '\'%s\' - %s' % (self.test.path, self.msg)

class Notest(Exception):
    def __init__(self, test, arch):
        self.arch = arch
        self.test = test
    def getMsg(self):
        return '[%s] \'%s\'' % (self.arch, self.test.path)

class Unsup(Exception):
    def __init__(self, test):
        self.test = test
    def getMsg(self):
        return '\'%s\'' % self.test.path

class Event(dict):
    terms = [
        'cpu',
        'flags',
        'type',
        'size',
        'config',
        'sample_period',
        'sample_type',
        'read_format',
        'disabled',
        'inherit',
        'pinned',
        'exclusive',
        'exclude_user',
        'exclude_kernel',
        'exclude_hv',
        'exclude_idle',
        'mmap',
        'comm',
        'freq',
        'inherit_stat',
        'enable_on_exec',
        'task',
        'watermark',
        'precise_ip',
        'mmap_data',
        'sample_id_all',
        'exclude_host',
        'exclude_guest',
        'exclude_callchain_kernel',
        'exclude_callchain_user',
        'wakeup_events',
        'bp_type',
        'config1',
        'config2',
        'branch_sample_type',
        'sample_regs_user',
        'sample_stack_user',
    ]

    def add(self, data):
        for key, val in data:
            log.debug("      %s = %s" % (key, val))
            self[key] = val

    def __init__(self, name, data, base):
        log.debug("    Event %s" % name);
        self.name  = name;
        self.group = ''
        self.add(base)
        self.add(data)

    def equal(self, other):
        for t in Event.terms:
            log.debug("      [%s] %s %s" % (t, self[t], other[t]));
            if t not in self or t not in other:
                return False
            if not data_equal(self[t], other[t]):
                return False
        return True

    def optional(self):
        if 'optional' in self and self['optional'] == '1':
            return True
        return False

    def diff(self, other):
        for t in Event.terms:
            if t not in self or t not in other:
                continue
            if not data_equal(self[t], other[t]):
                log.warning("expected %s=%s, got %s" % (t, self[t], other[t]))

def parse_version(version):
    if not version:
        return None
    return [int(v) for v in version.split(".")[0:2]]

# Test file description needs to have following sections:
# [config]
#   - just single instance in file
#   - needs to specify:
#     'command' - perf command name
#     'args'    - special command arguments
#     'ret'     - Skip test if Perf doesn't exit with this value (0 by default)
#     'test_ret'- If set to 'true', fail test instead of skipping for 'ret' argument
#     'arch'    - architecture specific test (optional)
#                 comma separated list, ! at the beginning
#                 negates it.
#     'auxv'    - Truthy statement that is evaled in the scope of the auxv map. When false,
#                 the test is skipped. For example 'auxv["AT_HWCAP"] == 10'. (optional)
#     'kernel_since' - Inclusive kernel version from which the test will start running. Only the
#                      first two values are supported, for example "6.1" (optional)
#     'kernel_until' - Exclusive kernel version from which the test will stop running. (optional)
# [eventX:base]
#   - one or multiple instances in file
#   - expected values assignments
class Test(object):
    def __init__(self, path, options):
        parser = configparser.ConfigParser()
        parser.read(path)

        log.warning("running '%s'" % path)

        self.path     = path
        self.test_dir = options.test_dir
        self.perf     = options.perf
        self.command  = parser.get('config', 'command')
        self.args     = parser.get('config', 'args')

        try:
            self.ret  = parser.get('config', 'ret')
        except:
            self.ret  = 0

        self.test_ret = parser.getboolean('config', 'test_ret', fallback=False)

        try:
            self.arch  = parser.get('config', 'arch')
            log.warning("test limitation '%s'" % self.arch)
        except:
            self.arch  = ''

        self.auxv = parser.get('config', 'auxv', fallback=None)
        self.kernel_since = parse_version(parser.get('config', 'kernel_since', fallback=None))
        self.kernel_until = parse_version(parser.get('config', 'kernel_until', fallback=None))
        self.expect   = {}
        self.result   = {}
        log.debug("  loading expected events");
        self.load_events(path, self.expect)

    def is_event(self, name):
        if name.find("event") == -1:
            return False
        else:
            return True

    def skip_test_kernel_since(self):
        if not self.kernel_since:
            return False
        return not self.kernel_since <= parse_version(platform.release())

    def skip_test_kernel_until(self):
        if not self.kernel_until:
            return False
        return not parse_version(platform.release()) < self.kernel_until

    def skip_test_auxv(self):
        def new_auxv(a, pattern):
            items = list(filter(None, pattern.split(a)))
            # AT_HWCAP is hex but doesn't have a prefix, so special case it
            if items[0] == "AT_HWCAP":
                value = int(items[-1], 16)
            else:
                try:
                    value = int(items[-1], 0)
                except:
                    value = items[-1]
            return (items[0], value)

        if not self.auxv:
            return False
        auxv = subprocess.check_output("LD_SHOW_AUXV=1 sleep 0", shell=True) \
               .decode(sys.stdout.encoding)
        pattern = re.compile(r"[: ]+")
        auxv = dict([new_auxv(a, pattern) for a in auxv.splitlines()])
        return not eval(self.auxv)

    def skip_test_arch(self, myarch):
        # If architecture not set always run test
        if self.arch == '':
            # log.warning("test for arch %s is ok" % myarch)
            return False

        # Allow multiple values in assignment separated by ','
        arch_list = self.arch.split(',')

        # Handle negated list such as !s390x,ppc
        if arch_list[0][0] == '!':
            arch_list[0] = arch_list[0][1:]
            log.warning("excluded architecture list %s" % arch_list)
            for arch_item in arch_list:
                # log.warning("test for %s arch is %s" % (arch_item, myarch))
                if arch_item == myarch:
                    return True
            return False

        for arch_item in arch_list:
            # log.warning("test for architecture '%s' current '%s'" % (arch_item, myarch))
            if arch_item == myarch:
                return False
        return True

    def restore_sample_rate(self, value=10000):
        try:
            # Check value of sample_rate
            with open("/proc/sys/kernel/perf_event_max_sample_rate", "r") as fIn:
                curr_value = fIn.readline()
            # If too low restore to reasonable value
            if not curr_value or int(curr_value) < int(value):
                with open("/proc/sys/kernel/perf_event_max_sample_rate", "w") as fOut:
                    fOut.write(str(value))

        except IOError as e:
            log.warning("couldn't restore sample_rate value: I/O error %s" % e)
        except ValueError as e:
            log.warning("couldn't restore sample_rate value: Value error %s" % e)
        except TypeError as e:
            log.warning("couldn't restore sample_rate value: Type error %s" % e)

    def load_events(self, path, events):
        parser_event = configparser.ConfigParser()
        parser_event.read(path)

        # The event record section header contains 'event' word,
        # optionaly followed by ':' allowing to load 'parent
        # event' first as a base
        for section in filter(self.is_event, parser_event.sections()):

            parser_items = parser_event.items(section);
            base_items   = {}

            # Read parent event if there's any
            if (':' in section):
                base = section[section.index(':') + 1:]
                parser_base = configparser.ConfigParser()
                parser_base.read(self.test_dir + '/' + base)
                base_items = parser_base.items('event')

            e = Event(section, parser_items, base_items)
            events[section] = e

    def run_cmd(self, tempdir):
        junk1, junk2, junk3, junk4, myarch = (os.uname())

        if self.skip_test_arch(myarch):
            raise Notest(self, myarch)

        if self.skip_test_auxv():
            raise Notest(self, "auxv skip")

        if self.skip_test_kernel_since():
            raise Notest(self, "old kernel skip")

        if self.skip_test_kernel_until():
            raise Notest(self, "new kernel skip")

        self.restore_sample_rate()
        cmd = "PERF_TEST_ATTR=%s %s %s -o %s/perf.data %s" % (tempdir,
              self.perf, self.command, tempdir, self.args)
        ret = os.WEXITSTATUS(os.system(cmd))

        log.info("  '%s' ret '%s', expected '%s'" % (cmd, str(ret), str(self.ret)))

        if not data_equal(str(ret), str(self.ret)):
            if self.test_ret:
                raise Fail(self, "Perf exit code failure")
            else:
                raise Unsup(self)

    def compare(self, expect, result):
        match = {}

        log.debug("  compare");

        # For each expected event find all matching
        # events in result. Fail if there's not any.
        for exp_name, exp_event in expect.items():
            exp_list = []
            res_event = {}
            log.debug("    matching [%s]" % exp_name)
            for res_name, res_event in result.items():
                log.debug("      to [%s]" % res_name)
                if (exp_event.equal(res_event)):
                    exp_list.append(res_name)
                    log.debug("    ->OK")
                else:
                    log.debug("    ->FAIL");

            log.debug("    match: [%s] matches %s" % (exp_name, str(exp_list)))

            # we did not any matching event - fail
            if not exp_list:
                if exp_event.optional():
                    log.debug("    %s does not match, but is optional" % exp_name)
                else:
                    if not res_event:
                        log.debug("    res_event is empty");
                    else:
                        exp_event.diff(res_event)
                    raise Fail(self, 'match failure');

            match[exp_name] = exp_list

        # For each defined group in the expected events
        # check we match the same group in the result.
        for exp_name, exp_event in expect.items():
            group = exp_event.group

            if (group == ''):
                continue

            for res_name in match[exp_name]:
                res_group = result[res_name].group
                if res_group not in match[group]:
                    raise Fail(self, 'group failure')

                log.debug("    group: [%s] matches group leader %s" %
                         (exp_name, str(match[group])))

        log.debug("  matched")

    def resolve_groups(self, events):
        for name, event in events.items():
            group_fd = event['group_fd'];
            if group_fd == '-1':
                continue;

            for iname, ievent in events.items():
                if (ievent['fd'] == group_fd):
                    event.group = iname
                    log.debug('[%s] has group leader [%s]' % (name, iname))
                    break;

    def run(self):
        tempdir = tempfile.mkdtemp();

        try:
            # run the test script
            self.run_cmd(tempdir);

            # load events expectation for the test
            log.debug("  loading result events");
            for f in glob.glob(tempdir + '/event*'):
                self.load_events(f, self.result);

            # resolve group_fd to event names
            self.resolve_groups(self.expect);
            self.resolve_groups(self.result);

            # do the expectation - results matching - both ways
            self.compare(self.expect, self.result)
            self.compare(self.result, self.expect)

        finally:
            # cleanup
            shutil.rmtree(tempdir)


def run_tests(options):
    for f in glob.glob(options.test_dir + '/' + options.test):
        try:
            Test(f, options).run()
        except Unsup as obj:
            log.warning("unsupp  %s" % obj.getMsg())
        except Notest as obj:
            log.warning("skipped %s" % obj.getMsg())

def setup_log(verbose):
    global log
    level = logging.CRITICAL

    if verbose == 1:
        level = logging.WARNING
    if verbose == 2:
        level = logging.INFO
    if verbose >= 3:
        level = logging.DEBUG

    log = logging.getLogger('test')
    log.setLevel(level)
    ch  = logging.StreamHandler()
    ch.setLevel(level)
    formatter = logging.Formatter('%(message)s')
    ch.setFormatter(formatter)
    log.addHandler(ch)

USAGE = '''%s [OPTIONS]
  -d dir  # tests dir
  -p path # perf binary
  -t test # single test
  -v      # verbose level
''' % sys.argv[0]

def main():
    parser = optparse.OptionParser(usage=USAGE)

    parser.add_option("-t", "--test",
                      action="store", type="string", dest="test")
    parser.add_option("-d", "--test-dir",
                      action="store", type="string", dest="test_dir")
    parser.add_option("-p", "--perf",
                      action="store", type="string", dest="perf")
    parser.add_option("-v", "--verbose",
                      default=0, action="count", dest="verbose")

    options, args = parser.parse_args()
    if args:
        parser.error('FAILED wrong arguments %s' %  ' '.join(args))
        return -1

    setup_log(options.verbose)

    if not options.test_dir:
        print('FAILED no -d option specified')
        sys.exit(-1)

    if not options.test:
        options.test = 'test*'

    try:
        run_tests(options)

    except Fail as obj:
        print("FAILED %s" % obj.getMsg())
        sys.exit(-1)

    sys.exit(0)

if __name__ == '__main__':
    main()
