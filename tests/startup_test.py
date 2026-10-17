# SPDX-License-Identifier: MIT
"""§4.1 regressions in the independent compiled-entry checker."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from check_build import check_startup

BASE = 0x1c02000
symbols = {'boot_normal': BASE+0x70, '__exception_stack_top': BASE+0x6000,
           '__main_stack_top': BASE+0x7000, '__payload_end': BASE+0x3800}
pre = ['r1 = 0', 'rete = r1', f'r1 = {symbols["boot_normal"]}', 'reti = r1', 'rti']
post = [f'r1 = {symbols["__exception_stack_top"]}', 'ssp = r1',
        f'r1 = {symbols["__main_stack_top"]}', 'sp = r1', 'goto 512 <boot_main : 1c02270 >']


def listing(before, after):
    return [(BASE+i*2, text) for i, text in enumerate(before)] + [
        (BASE+0x70+i*2, text) for i, text in enumerate(after)]


def refuses(before, after):
    try:
        check_startup(listing(before, after), symbols, BASE)
    except ValueError:
        return
    raise AssertionError('invalid entry accepted')


check_startup(listing(pre, post), symbols, BASE)
for position, invalid in ((0, 'r1 = 1'), (1, 'retx = r1'), (2, f'r1 = {BASE+0x72}'),
                          (3, 'rete = r1'), (4, 'rts')):
    changed = pre.copy()
    changed[position] = invalid
    refuses(changed, post)
refuses(['ssp = r1']+pre, post)
refuses(['r0 = 0']+pre, post)
for position, invalid in ((0, 'r1 = 0'), (1, 'usp = r1'), (2, 'r1 = 0'),
                          (3, 'ssp = r1'), (4, 'goto 0 <other : 1c02000 >')):
    changed = post.copy()
    changed[position] = invalid
    refuses(pre, changed)
refuses(pre, ['cli']+post)
print('PASS: §4.1 checker rejects wrong return register, premature stacks, lost r0 and invalid continuation')
