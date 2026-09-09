"""trace_summary.py -- first-pass analysis of an ETW trace from trace_load.ps1.

Windows Performance Analyzer is not installed on this machine, so this leans on
tracerpt.exe (which ships with Windows) and answers the one question RIP
sampling could not:

    the load's busiest thread spends 73% of its time inside
    RtlSleepConditionVariableSRW -- WHO is it waiting for?

ETW's ReadyThread event records exactly that: "thread A made thread B runnable".
Chase the readier of the blocked thread and you have the lock holder, rather
than an inference about one.

    python tools/trace_summary.py <trace.etl>            summary only (fast)
    python tools/trace_summary.py <trace.etl> --dump     also convert to XML and
                                                         build the ready-chain

The XML conversion is the expensive part -- tracerpt cannot slice by time, so it
expands the WHOLE trace. Keep captures to ~60 s (trace_load.ps1 defaults to that)
or this will produce tens of GB.
"""
import argparse
import collections
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET


def run(cmd):
    print('  $ ' + ' '.join(cmd), flush=True)
    return subprocess.run(cmd, capture_output=True, text=True)


def summarize(etl, outdir):
    """tracerpt -summary: event counts per provider. Cheap, and it tells you
    whether the capture actually contains what you wanted before you spend
    minutes expanding it."""
    summ = os.path.join(outdir, 'summary.txt')
    rep = os.path.join(outdir, 'report.html')
    r = run(['tracerpt', etl, '-summary', summ, '-report', rep, '-f', 'HTML', '-y'])
    if r.returncode != 0:
        print('tracerpt failed:', (r.stderr or r.stdout)[:400])
        return
    if os.path.exists(summ):
        text = open(summ, encoding='utf-8', errors='replace').read()
        print('\n--- event counts ---')
        for line in text.splitlines():
            if re.search(r'(CSwitch|ReadyThread|PageFault|VirtualAlloc|Total|Events)', line, re.I):
                print('   ' + line.strip())
        print(f'\nfull summary: {summ}\nhtml report : {rep}')


def ready_chain(etl, outdir, top=15):
    """Convert to XML and count ReadyThread edges: who unblocks whom.

    The pair we care about is (readying thread -> readied thread). A thread that
    appears overwhelmingly as the READIER of the blocked worker is the one
    holding whatever the worker waits on.
    """
    xml = os.path.join(outdir, 'trace.xml')
    if not os.path.exists(xml):
        r = run(['tracerpt', etl, '-o', xml, '-of', 'XML', '-y'])
        if r.returncode != 0:
            print('tracerpt XML failed:', (r.stderr or r.stdout)[:400])
            return
    size_mb = os.path.getsize(xml) / 1e6
    print(f'\nparsing {xml} ({size_mb:.0f} MB)...')

    edges = collections.Counter()      # (readier_tid, readied_tid) -> count
    cswitch = collections.Counter()    # tid -> context switches
    seen = 0
    # iterparse: the XML is far too large to hold in memory.
    for _, el in ET.iterparse(xml, events=('end',)):
        tag = el.tag.rsplit('}', 1)[-1]
        if tag != 'Event':
            continue
        seen += 1
        name = ''
        rendering = el.find('.//{*}RenderingInfo')
        if rendering is not None:
            op = rendering.find('.//{*}Opcode')
            if op is not None and op.text:
                name = op.text
        data = {d.get('Name'): (d.text or '') for d in el.iter() if d.tag.endswith('Data')}
        if 'Ready' in name:
            sysd = el.find('.//{*}Execution')
            readier = sysd.get('ThreadID') if sysd is not None else '?'
            readied = data.get('TThreadId') or data.get('ThreadId') or '?'
            edges[(readier, readied)] += 1
        elif 'CSwitch' in name or 'Context' in name:
            sysd = el.find('.//{*}Execution')
            if sysd is not None:
                cswitch[sysd.get('ThreadID')] += 1
        el.clear()

    print(f'{seen} events parsed')
    if edges:
        print('\n--- ReadyThread edges (readier -> readied), top %d ---' % top)
        for (a, b), c in edges.most_common(top):
            print(f'   {c:>8}  tid {a}  ->  tid {b}')
        print('\nRead this as: the thread on the LEFT is repeatedly unblocking the one on')
        print('the RIGHT. If your stalled worker is on the right, its readier is holding')
        print('whatever it waits on -- that is the lock holder RIP sampling could not name.')
    else:
        print('\nNo ReadyThread events found. Either the CPU profile was not captured,')
        print('or tracerpt rendered the opcode differently -- inspect trace.xml directly.')
    if cswitch:
        print('\n--- context switches per thread, top 10 ---')
        for tid, c in cswitch.most_common(10):
            print(f'   {c:>8}  tid {tid}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('etl')
    ap.add_argument('--dump', action='store_true',
                    help='also expand to XML and build the ready-chain (slow, large)')
    a = ap.parse_args()
    if not os.path.exists(a.etl):
        print('no such file:', a.etl)
        return 1
    outdir = os.path.splitext(a.etl)[0] + '_analysis'
    os.makedirs(outdir, exist_ok=True)
    print(f'trace   : {a.etl}  ({os.path.getsize(a.etl)/1e6:.0f} MB)')
    print(f'analysis: {outdir}')
    summarize(a.etl, outdir)
    if a.dump:
        ready_chain(a.etl, outdir)
    else:
        print('\n(add --dump for the ReadyThread chain -- that is the lock-holder answer)')
    return 0


if __name__ == '__main__':
    sys.exit(main())
