"""The platform-assignment tag on a line update (2026-09-20), offline on Lua 5.2.

The slice appends " asg=<0|1>" to an LUPDATE that came out of the line editor's
assignment pass (a station or waypoint added). inject.lua carries it onto the
scheduled command and the sent-list note; a click merged onto a waiting update
inherits the waiting update's tag; a record without the tag ships none. At the
replay lines.lua names the line for the slice in lockstep_lassign_<x>.txt right
before api.cmd.make.updateLine and blanks the file after.

Reuses the rapid-edit harness (real inject.lua + lines.lua, stub engine).

    python tools/line_assign_tag_test.py
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import line_rapid_edit_test as R   # noqa: E402

LID = R.LID
fails = []


def check(name, cond, extra=""):
    print(("ok   " if cond else "FAIL ") + name + (f"  ({extra})" if extra else ""))
    if not cond:
        fails.append(name)


def main():
    d = tempfile.mkdtemp()
    os.chdir(d)                                      # K.BASE is "": the assign file lands here
    path = os.path.join(d, "lockstep_inject_b.txt")
    open(path, "wb").close()
    H = R.runtime(path)

    def click(line):
        with open(path, "ab") as f:
            f.write(("ARMED 1\n" + line + "\n").encode())
        H.poll()

    a, b = 107158, 107161

    # 1. the tag off the record, onto the command and the sent note
    click(R.click_line([a]) + " asg=1")
    s = H.lastSched()
    check("a tagged click schedules asg=1", s is not None and s.asg == 1, str(s and s.asg))
    check("...and the sent-list note keeps it", H.CM.lineSent["b:6"].asg == 1)

    # 2. a click without the tag, merged onto the waiting tagged one, inherits it
    click(R.click_line([b]))                         # built from the stale empty list, no tag of its own
    s = H.lastSched()
    check("an untagged click merged onto a tagged update inherits asg=1", s.asg == 1, str(s.asg))
    check("...with both stops", s.stops == f"{R.stop_str(a)};{R.stop_str(b)}", str(s.stops))

    # 3. everything applied and confirmed: a plain click ships no tag
    while H.applyNext():
        pass
    H.confirm()
    click(R.click_line([a, b]) + " asg=0")
    check("asg=0 is a tag too (the pass ran with false)", H.lastSched().asg == 0, str(H.lastSched().asg))
    while H.applyNext():
        pass
    H.confirm()
    click(R.click_line([a]))
    check("a record without the tag schedules none", H.lastSched().asg is None, str(H.lastSched().asg))

    # 4. the replay: the file names the line while the command is MADE, and is blank after
    H.lua(r'''
      CM.lineReadWaypoints = function() return {} end
      api.type = { Line = { new = function() return { stops = {} } end, Stop = { new = function() return {} end } },
                   StationTerminal = { new = function() return {} end } }
      game.interface = { getEntities = function() return { 107158, 107161 } end }
      api.cmd = { make = {}, sendCommand = function(cmd, cb) end }
      SEEN = {}
      api.cmd.make.updateLine = function(lid, obj)
        local f = io.open("lockstep_lassign_b.txt", "r")
        SEEN[#SEEN + 1] = f and f:read("*a") or "(no file)"
        if f then f:close() end
        return {}
      end
    ''')
    stop = R.stop_str(a)
    H.lua(f'CM.execLine({{ op = "LUPDATE", key = "b:6", origin = "a", seq = 77, at = 100.8, armed = 1, '
          f'wait = 180, stops = "{stop}", alts = "", asg = 1 }})')
    seen = list(H.lua("return SEEN").values())
    check("the replay named the line for the slice while the command was made",
          len(seen) == 1 and seen[0].startswith(f"{LID} 1 "), str(seen) + " | log: " + H.logs()[-700:])
    after = open(os.path.join(d, "lockstep_lassign_b.txt")).read() if os.path.exists(os.path.join(d, "lockstep_lassign_b.txt")) else "(no file)"
    check("...and blanked the file after", after == "", repr(after))

    H.lua(f'CM.execLine({{ op = "LUPDATE", key = "b:6", origin = "a", seq = 78, at = 101.0, armed = 1, '
          f'wait = 180, stops = "{stop}", alts = "" }})')
    seen = list(H.lua("return SEEN").values())
    check("an update without the tag makes its command with the file still blank",
          len(seen) == 2 and seen[1] == "", str(seen))

    print("ALL PASS" if not fails else f"{len(fails)} FAILED: {fails}")
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
