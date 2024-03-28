#! /usr/bin/python3

import json
from machine import PDP1170
from pdptraps import PDPTrap, PDPTraps
import random
import sys


class PDP1170_wrapper(PDP1170):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        self.reset_mem_transactions_dict()

    def reset_mem_transactions_dict(self):
        self.mem_transactions = dict()
        self.before = dict()

    def get_mem_before(self):
        return self.before

    def get_mem_transactions_dict(self):
        return self.mem_transactions

    def put(self, physaddr, value):
        self.before[physaddr] = value
        super().physRW(physaddr, value)

    def physRW(self, physaddr, value=None):
        if value == None:  # read
            if not physaddr in self.mem_transactions and not physaddr in self.before:
                self.before[physaddr] = random.randint(0, 65536)
            return super().physRW(physaddr, self.before[physaddr])

        self.mem_transactions[physaddr] = value
        return super().physRW(physaddr, value)

    def physRW_N(self, physaddr, nwords, words=None):
        temp_addr = physaddr
        if words == None:
            for i in range(nwords):
                self.physRW(temp_addr, random.randint(0, 65536))
                temp_addr += 2
            return super().physRW_N(physaddr, nwords)

        for w in words:
            self.mem_transactions[temp_addr] = w
            temp_addr += 2

        return super().physRW_N(physaddr, nwords, words=words)

class test_generator:
    def _invoke_bp(self, a, i):
        return True

    def put_registers(self, p, target, tag):
        target[tag] = dict()
        target[tag][0] = dict()
        target[tag][1] = dict()
        for set_ in range(0, 2):
            for reg_ in range(0, 6):
                target[tag][set_][reg_] = p.registerfiles[set_][reg_]

        target[tag]['sp'] = p.stackpointers
        target[tag]['pc'] = p.PC

    def create_test(self):
        out = { }

        p = PDP1170_wrapper(loglevel='DEBUG')

        addr = random.randint(0, 65536) & ~3

        # TODO what is the maximum size of an instruction?
        mem_kv = []
        mem_kv.append((addr + 0, random.randint(0, 65536)))
        mem_kv.append((addr + 2, random.randint(0, 65536)))
        mem_kv.append((addr + 4, random.randint(0, 65536)))
        mem_kv.append((addr + 6, random.randint(0, 65536)))
        out['memory-before'] = dict()
        for a, v in mem_kv:
            p.put(a, v)

        try:
            # generate & set PSW
            while True:
                try:
                    p.psw = random.randint(0, 65536)
                    break
                except PDPTraps.ReservedInstruction as ri:
                    pass

            # generate other registers
            reg_kv = []
            for i in range(7):
                reg_kv.append((i, random.randint(0, 65536)))
            reg_kv.append((7, addr))

            # set registers 
            set_ = (p.psw >> 11) & 1
            for r, v in reg_kv:
                p.registerfiles[set_][r] = v
                p.registerfiles[1 - set_][r] = (~v) & 65535  # make sure it triggers errors
                assert p.registerfiles[set_][r] == p.r[r]
            p.r[6] = p.registerfiles[set_][6]
            p._syncregs()

            self.put_registers(p, out, 'registers-before')
            out['registers-before']['psw'] = p.psw

            p.run_steps(pc=addr, steps=1)

            self.put_registers(p, out, 'registers-after')
            out['registers-after']['psw'] = p.psw

            mb = p.get_mem_before()
            for a in mb:
                out['memory-before'][a] = mb[a]

            out['memory-after'] = dict()
            mem_transactions = p.get_mem_transactions_dict()
            for a in mem_transactions:
                out['memory-after'][a] = mem_transactions[a]
            # TODO originele geheugeninhouden checken

            # TODO check if mem_transactions affects I/O, then return None

            return out

        except PDPTraps.ReservedInstruction as pri:
            return None

        except Exception as e:
            # handle PDP11 traps; store them
            print('test failed', e)
            return None

fh = open(sys.argv[1], 'w')

t = test_generator()

tests = []
for i in range(0, 4096):
    test = t.create_test()
    if test != None:
        tests.append(test)

fh.write(json.dumps(tests, indent=4))
fh.close()