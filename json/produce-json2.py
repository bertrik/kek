#! /usr/bin/python3

import copy
import json
from machine import PDP1170
from mmio import MMIO
from op4 import op4_dispatch_table
import os
from pdptraps import PDPTrap, PDPTraps
import random
import sys


ignore_traps = True

class MMIO_wrapper(MMIO):
    def register(self, iofunc, offsetaddr, nwords, *, byte_writes=False, reset=False):
        pass

    def register_simpleattr(self, obj, attrname, addr, reset=False):
        pass

    def devicereset_register(self, func):
        pass

class PDP1170_wrapper(PDP1170):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

        self.logger.info('')

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
            self.logger.info(f'Read from {physaddr:08o} (phys)')
            if not physaddr in self.mem_transactions and not physaddr in self.before:
                self.before[physaddr] = random.randint(0, 65535)
            return super().physRW(physaddr, self.before[physaddr])

        self.logger.info(f'Write to {physaddr:08o}: {value:06o} (phys)')
        self.mem_transactions[physaddr] = value
        return super().physRW(physaddr, value)

    def physRW_N(self, physaddr, nwords, words=None):
        temp_addr = physaddr
        if words == None:
            self.logger.info(f'Read {nwords} words from {physaddr:08o} (phys)')
            for i in range(nwords):
                self.physRW(temp_addr, random.randint(0, 65535))
                temp_addr += 2
            return super().physRW_N(physaddr, nwords)

        self.logger.info(f'Write {nwords} ({len(words)}) words to {physaddr:08o} (phys)')
        for w in words:
            self.mem_transactions[temp_addr] = w
            temp_addr += 2

        return super().physRW_N(physaddr, nwords, words=words)

    def simple_run_1(self, pc):
        thisPC = pc
        self.r[self.PC] = (thisPC + 2) & 0o177777  # "could" wrap

        try:
            inst = self.mmu.wordRW(thisPC)
            op4_dispatch_table[inst >> 12](self, inst)

            return True

        except PDPTrap as trap:
            return False

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

        target[tag]['sp'] = copy.deepcopy(p.stackpointers)
        target[tag]['pc'] = p.r[p.PC]

    def create_test(self, instr, psw):
        try:
            out = { }

            p = PDP1170_wrapper(loglevel='ERROR')
            p.ub.mmio = MMIO_wrapper(p)

            # TODO what is the maximum size of an instruction?
            # non-mmu thus shall be below device range
            addr = random.randint(0, 0o160000 - 8) & ~1
            mem_kv = []
            if instr == 1:  # TODO ignore 'WAIT' instruction
                return None
            p.logger.info(f'emulating {instr:06o}')
            mem_kv.append((addr + 0, instr))
            mem_kv.append((addr + 2, random.randint(0, 65535 - 8) & ~1))
            mem_kv.append((addr + 4, random.randint(0, 65535 - 8) & ~1))
            mem_kv.append((addr + 6, random.randint(0, 65535 - 8) & ~1))
            out['memory-before'] = dict()
            for a, v in mem_kv:
                p.put(a, v)

            p.psw = psw

            # generate other registers
            reg_kv = []
            for i in range(7):
                reg_kv.append((i, random.randint(0, 65535) & ~1))
            reg_kv.append((7, addr))

            # set registers 
            set_ = (p.psw >> 11) & 1
            for r, v in reg_kv:
                p.registerfiles[set_][r] = v
                p.registerfiles[1 - set_][r] = (~v) & 65535  # make sure it triggers errors
                assert p.registerfiles[set_][r] == p.r[r]
            p.r[6] = p.registerfiles[set_][6]
            p.r[p.PC] = addr
            p._syncregs()

            self.put_registers(p, out, 'registers-before')
            out['registers-before']['psw'] = p.psw

            # run instruction
            p.run_steps(pc=addr, steps=1)
            #if p.simple_run_1(addr) == False:
            #    return None

            #if (p.straps or p.issues) and ignore_traps:
            if p.straps and ignore_traps:
                return None

            p._syncregs()

            self.put_registers(p, out, 'registers-after')
            out['registers-after']['psw'] = p.psw

            mb = p.get_mem_before()
            for a in mb:
                out['memory-before'][a] = mb[a]

            out['memory-after'] = dict()
            mem_transactions = p.get_mem_transactions_dict()
            # verify original values
            for a, v in mem_kv:
                if not a in mem_transactions:
                    mem_transactions[a] = v
            for a in mem_transactions:
                out['memory-after'][a] = mem_transactions[a]

            out['mmr0-after'] = p.mmu.MMR0
            p.mmu._MMR1commit()
            out['mmr1-after'] = p.mmu.MMR1
            out['mmr2-after'] = p.mmu.MMR2
            out['mmr3-after'] = p.mmu.MMR3

            return out

        except PDPTraps.ReservedInstruction as pri:
            return None

        except Exception as e:
            # handle PDP11 traps; store them
            print(f'test failed {e}, line number: {e.__traceback__.tb_lineno}')
            return None

stop = False
while True:
    psw = random.randint(0, 65535) & 0o174377
    name = f'/data/data/temp2/0008-{psw:06o}.json'
    if os.path.isfile(name):
        print(f'skipping {name}')
        continue

    print(name)
    fh = open(name, 'w')

    t = test_generator()

    tests = []
    try:
        for lb in range(65535):
            if (lb & 63) == 0:
                print(f'{lb} {len(tests)}\r', end='')
            test = t.create_test(lb, psw)
            if test != None:
                tests.append(test)

    except KeyboardInterrupt as ki:
        stop = True

    fh.write(json.dumps(tests, indent=4))
    fh.close()

    if stop:
        break
