"""Commit N GiB of memory and hold it, to test how the game and Big Maps behave
on a fuller machine (the rig has 94 GiB and no page file, so commit == RAM).

    python tools\\mem_hog.py 62          # leave ~32 GiB of the rig's 94 free
    python tools\\mem_hog.py 62 --touch  # also write every page (real RAM use,
                                        # not just commit charge; slower to start)

Ends on Enter or Ctrl-C; the memory goes back the moment the process exits.
Watch the plugin's lines in %LOCALAPPDATA%\\tpf2mp\\data\\tpf2mp_host.log
("resident target", "commit_tight") to see the pager policy react.
"""
import sys
import time


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    gib = float(argv[1])
    touch = "--touch" in argv
    chunk = 1 << 30
    held = []
    t0 = time.time()
    for i in range(int(gib)):
        b = bytearray(chunk)                     # committed (charged) at once; resident only when touched
        if touch:
            for off in range(0, chunk, 4096):
                b[off] = 1
        held.append(b)
        print(f"\rholding {i + 1:.0f} GiB{' (touched)' if touch else ''}...", end="", flush=True)
    print(f"\nholding {gib:.0f} GiB after {time.time() - t0:.1f} s -- press Enter to release")
    try:
        input()
    except (KeyboardInterrupt, EOFError):
        pass
    print("released")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
