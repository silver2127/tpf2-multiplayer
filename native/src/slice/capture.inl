// capture.inl -- part of tpf2_slice.dll: included by slice_hook.cpp in this order, one translation unit.
// the vehicle/line command writers, speed and calendar buttons, CaptureFactory (the factory hook's dispatcher)


// Returns true when a line was written.
static bool WriteInjectVehicleCmd(int fid, uint64_t r8, uint64_t r9, uint64_t st0)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    bool shipped = true;
    if (fid == 3) {
        // the whole list: a bulk sell of a big fleet is ONE command with every
        // vehicle in it (256 was the old cut, and the rest stayed unsold on the peers)
        std::vector<int32_t> ids;
        if (ReadIntVec(r8, &ids, "VSELL") && !ids.empty()) {
            fprintf(f, "VSELL %d", (int)ids.size());
            for (size_t i = 0; i < ids.size(); i++) fprintf(f, " %d", ids[i]);
            fprintf(f, "\n");
            Log("[slice] VSELL shipped: %d vehicle(s)\n", (int)ids.size());
        } else {
            Log("[slice] VSELL: vehicle list unreadable or empty -- not shipped\n");
            shipped = false;
        }
    } else if (fid == 4) {
        // ReplaceVehicle: r8 = the vehicle being replaced, r9 = the new
        // TransportVehicleConfig (a pointer, unlike BuyVehicle's by-value copy
        // on the caller's stack). A config that fails validation writes NOTHING
        // -- the line is only opened, never begun, so the file stays parseable.
        uint64_t ub = 0;
        int units = VCfgParts(r9, &ub, "VREPL");
        if (units >= 0) {
            fprintf(f, "VREPL %d", (int)(int32_t)r8);
            WriteVehicleConfig(f, r9, ub, units);
            fprintf(f, "\n");
            Log("[slice] VREPL shipped: vehicle=%d parts=%d\n", (int)(int32_t)r8, units);
        } else {
            shipped = false;
        }
    } else if (fid == 5) {
        fprintf(f, "VDEPOT %d %d\n", (int)(int32_t)r8, (int)(r9 & 1));
        Log("[slice] VDEPOT shipped: vehicle=%d sell=%d\n", (int)(int32_t)r8, (int)(r9 & 1));
    } else if (fid == 6) {
        fprintf(f, "VLINE %d %d %d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
        Log("[slice] VLINE shipped: vehicle=%d line=%d stop=%d\n", (int)(int32_t)r8, (int)(int32_t)r9, (int)(int32_t)st0);
    } else if (fid == 7) {
        if (g_lcDecodeOk) {
            // LCREATEX <r> <g> <b> <wait> <n> {<sg> <station> <terminal> <loadMode> <min> <max> <nAlt> {<st> <term>}*nAlt}*n name=<enc>
            // The colour goes out EXACT: %.9g round-trips a float. The line editor gives a
            // new line the least-used lineColors entry (bright ones first), counting the
            // existing lines' colours by exact float equality (FUN_14215da30). Rounded to
            // %.4f, 127/255 came back as 0.4980, matched no palette entry, and every new
            // line was the same orange (2026-09-12).
            const LineDecode& d = g_lcDecode.line;
            fprintf(f, "LCREATEX %.9g %.9g %.9g %.9g %d", g_lcDecode.rgb[0], g_lcDecode.rgb[1], g_lcDecode.rgb[2], d.wait, d.n);
            for (int i = 0; i < d.n; i++) {
                fprintf(f, " %d %d %d %d %.9g %.9g %d", d.st[i].sg, d.st[i].station, d.st[i].terminal,
                        d.st[i].loadMode, d.st[i].minWait, d.st[i].maxWait, d.st[i].nAlt);
                for (int a = 0; a < d.st[i].nAlt; a++)
                    fprintf(f, " %d %d", d.st[i].alt[a].station, d.st[i].alt[a].terminal);
            }
            WriteLineWaypoints(f, d);
            const int32_t spare = (int32_t)InterlockedCompareExchange(&g_lcSpareId, 0, 0);
            if (spare) fprintf(f, " spare=%d", spare);
            fprintf(f, " name=%s\n", g_lcDecode.nameEnc.c_str());
            Log("[slice] LCREATEX shipped: name=%.200s stops=%d%s\n", g_lcDecode.nameEnc.c_str(), d.n, spare ? " (the spare line opens the editor)" : "");
        } else {
            // not decoded: the new line's content is read back from the entity by
            // the Lua side once it exists; only the EVENT ships from here.
            fprintf(f, "LCREATE\n");
            Log("[slice] LCREATE shipped (event only)\n");
        }
    } else if (fid == 8) {
        if (g_lineDecodeOk) {
            // LUPDATE <line> <wait> <n> {<sg> <station> <terminal> <loadMode> <min> <max> <nAlt> {<st> <term>}*nAlt}*n
            const LineDecode& d = g_lineDecode;
            fprintf(f, "LUPDATE %d %.9g %d", (int)(int32_t)r8, d.wait, d.n);
            for (int i = 0; i < d.n; i++) {
                fprintf(f, " %d %d %d %d %.9g %.9g %d", d.st[i].sg, d.st[i].station, d.st[i].terminal,
                        d.st[i].loadMode, d.st[i].minWait, d.st[i].maxWait, d.st[i].nAlt);
                for (int a = 0; a < d.st[i].nAlt; a++)
                    fprintf(f, " %d %d", d.st[i].alt[a].station, d.st[i].alt[a].terminal);
            }
            WriteLineWaypoints(f, d);
            if (g_lineAsgTag >= 0) fprintf(f, " asg=%d", g_lineAsgTag);
            fprintf(f, "\n");
            if (d.n > 0)
                Log("[slice] LUPDATE shipped DECODED: line=%d wait=%g stops=%d (first: sg=%d st=%d term=%d lm=%d wait=%g..%g)\n",
                    (int)(int32_t)r8, d.wait, d.n, d.st[0].sg, d.st[0].station, d.st[0].terminal,
                    d.st[0].loadMode, d.st[0].minWait, d.st[0].maxWait);
            else
                Log("[slice] LUPDATE shipped DECODED: line=%d wait=%g stops=0 (last stop removed)\n",
                    (int)(int32_t)r8, d.wait);
        } else {
            fprintf(f, "LUPDATE %d\n", (int)(int32_t)r8);
            Log("[slice] LUPDATE shipped (event only): line=%d\n", (int)(int32_t)r8);
        }
    } else if (fid == 9) {
        fprintf(f, "LDELETE %d\n", (int)(int32_t)r8);
        Log("[slice] LDELETE shipped: line=%d\n", (int)(int32_t)r8);
    } else if (fid == 10) {
        fprintf(f, "VREV %d\n", (int)(int32_t)r8);
        Log("[slice] VREV shipped: vehicle=%d\n", (int)(int32_t)r8);
    } else if (fid == 13) {
        // SetColor(entity, Vec3f const&): r9 points at three floats, 0..1 each.
        float col[3] = { -1.0f, -1.0f, -1.0f };
        if (Readable((void*)r9, 12)) memcpy(col, (void*)r9, 12);
        if (col[0] >= 0.0f) {
            fprintf(f, "VCOLOR %d %.9g %.9g %.9g\n", (int)(int32_t)r8, col[0], col[1], col[2]);   // exact, like LCREATEX's colour
            Log("[slice] VCOLOR shipped: entity=%d rgb=%.3f,%.3f,%.3f\n",
                (int)(int32_t)r8, col[0], col[1], col[2]);
        } else {
            Log("[slice] VCOLOR: colour at %llx unreadable -- not shipped\n", (unsigned long long)r9);
            shipped = false;
        }
    } else if (fid == 14) {
        // SetName(entity, std::string const&): the name at any length
        // (ReadStdString), percent-encoded because the wire is split on
        // whitespace and a player names things "Coal Line 2". An EMPTY name
        // cannot travel: the Lua's VNAME parser needs a third token.
        std::string name;
        if (!ReadStdString(r9, &name, "VNAME")) name.clear();
        if (!name.empty()) {
            const std::string enc = PercentEncode(name);
            fprintf(f, "VNAME %d %s\n", (int)(int32_t)r8, enc.c_str());
            Log("[slice] VNAME shipped: entity=%d name='%.200s'%s\n", (int)(int32_t)r8, name.c_str(),
                name.size() > 200 ? "..." : "");
        } else {
            Log("[slice] VNAME: name at %llx unreadable or empty -- not shipped\n", (unsigned long long)r9);
            shipped = false;
        }
    }
    fclose(f);
    return shipped;
}

// A cancel is only honest when the command's whole payload reaches the wire:
// BuyVehicle's and ReplaceVehicle's new config, SellVehicle's vehicle list.
// When that does not read, nothing can ship, so the command must run natively
// rather than be cancelled into nothing (never cancel on a failed decode). The
// other vehicle and line commands carry plain values.
static bool VehiclePayloadReadable(int fid, uint64_t r8, uint64_t r9, uint64_t st0)
{
    uint64_t b = 0;
    if (fid == 2) return VCfgParts(st0, &b, "VBUY") >= 0;
    if (fid == 3) { std::vector<int32_t> ids; return ReadIntVec(r8, &ids, "VSELL") && !ids.empty(); }
    if (fid == 4) return VCfgParts(r9, &b, "VREPL") >= 0;
    return true;
}

// A click on the clock's speed controls while a session is live: cancelled
// fire-and-forget (the clock reads the speed back every frame; nothing waits on
// the command) and written as SPEEDBTN <speed> <toggle|button>. The mod makes a
// speed button this player's vote for the session speed, which every instance
// counts at its stamp, and the host's pause toggle a pause or a resume, so a
// lever only moves through pacing. Not live, not a clock caller, or a value out
// of range: the click runs natively, as in a stock game.
static bool WriteInjectSpeedButton(int speed, const char* kind)
{
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] no instance letter -- cannot inject\n"); return false; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] cannot open %s\n", p); return false; }
    fprintf(f, "SPEEDBTN %d %s\n", speed, kind);
    fclose(f);
    return true;
}

static void CaptureSpeedButton(uint64_t rcx, uint64_t rdx, uint64_t caller)
{
    const int speed = (int)(int32_t)(uint32_t)rdx;   // no Engine argument: the speed is the low 32 bits of rdx
    bool button = false, toggle = false;
    for (uintptr_t c : CALLER_SPEED_BUTTONS) if (caller == c) button = true;
    for (uintptr_t c : CALLER_PAUSE_TOGGLE) if (caller == c) toggle = true;
    if (!button) {
        static uint64_t seen[8] = {};
        for (int i = 0; i < 8; i++) {
            if (seen[i] == caller) break;
            if (!seen[i]) {
                seen[i] = caller;
                Log("[slice] SetGameSpeed(%d) from caller_rva=%llx -- not a speed button, left alone (logged once per caller)\n",
                    speed, (unsigned long long)caller);
                break;
            }
        }
        return;
    }
    if (speed < 0 || speed > 64) {
        Log("[slice] speed button value %d out of range -- left alone\n", speed);
        return;
    }
    if (!SessionLive()) {
        Log("[slice] speed button %d: no live session -- left alone\n", speed);
        return;
    }
    const char* kind = toggle ? "toggle" : "button";
    if (!WriteInjectSpeedButton(speed, kind)) {
        Log("[slice] speed button %d: not shipped -- left alone\n", speed);
        return;
    }
    InterlockedExchange(&g_pendingNoCb, 1);
    InterlockedExchange(&g_pendingHonour, 0);
    InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
    Log("[slice] armed cancel: speed %s %d (caller_rva=%llx) -- the mod applies it\n",
        kind, speed, (unsigned long long)caller);
}

// The editor's date picker and date speed slider while a session is live:
// cancelled fire-and-forget (the clock reads the date and calendar speed back
// every frame; nothing waits on the command) and written as SETDATE <julian day>
// or CALSPEED <ms per day>. Every instance, the originator included, applies it
// at the stamp (CM.execCalendar), so the calendar moves on the same sim step
// everywhere. Not live, not the editor, or a value out of range: it runs
// natively, as in a stock game.
static void CaptureCalendar(uint64_t id, uint64_t rcx, uint64_t rdx, uint64_t caller)
{
    const bool isDate = (id == (uint64_t)ID_SETDATE);
    const char* what = isDate ? "SetDate" : "SetCalendarSpeed";
    const int value = (int)(int32_t)(uint32_t)rdx;
    if (caller != (isDate ? CALLER_SET_DATE : CALLER_CALENDAR_SPEED)) {
        static uint64_t seen[8] = {};
        for (int i = 0; i < 8; i++) {
            if (seen[i] == caller) break;
            if (!seen[i]) {
                seen[i] = caller;
                Log("[slice] %s(%d) from caller_rva=%llx -- not the editor, left alone (logged once per caller)\n",
                    what, value, (unsigned long long)caller);
                break;
            }
        }
        return;
    }
    // 1721426 = 0001-01-01 and 5373484 = 9999-12-31 in Julian days. The slider
    // (0x4f29f0, a value-changed handler taking a stop index) sends
    // default-ms-per-day / the stop's multiplier, and 0 for a multiplier of 0 or
    // less -- the stopped calendar -- so 0 is a real setting and must replicate;
    // a day longer than ~2.8 hours is not a stop the slider offers.
    const bool inRange = isDate ? (value >= 1721426 && value <= 5373484) : (value >= 0 && value <= 10000000);
    if (!inRange) {
        Log("[slice] %s value %d out of range -- left alone\n", what, value);
        return;
    }
    if (!SessionLive()) {
        Log("[slice] %s %d: no live session -- left alone\n", what, value);
        return;
    }
    ReadInstance();   // NOT cached: the lobby can rename this peer after attach
    if (!g_instance[0]) { Log("[slice] %s %d: no instance letter -- left alone\n", what, value); return; }
    char p[MAX_PATH];
    snprintf(p, sizeof(p), "%slockstep_inject_%s.txt", g_dataDir, g_instance);
    FILE* f = _fsopen(p, "a", _SH_DENYNO);
    if (!f) { Log("[slice] %s %d: cannot open %s -- left alone\n", what, value, p); return; }
    fprintf(f, "%s %d\n", isDate ? "SETDATE" : "CALSPEED", value);
    fclose(f);
    InterlockedExchange(&g_pendingNoCb, 1);
    InterlockedExchange(&g_pendingHonour, 0);
    InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
    Log("[slice] armed cancel: %s %d (caller_rva=%llx) -- every instance applies it at the stamp\n",
        what, value, (unsigned long long)caller);
}

static void CaptureFactory(const Factory& f, uint64_t rcx, uint64_t rdx, uint64_t r8,
                           uint64_t r9, uint64_t calleeRsp, uint64_t caller, bool cancel)
{
    uint64_t st[6] = { 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 6; i++)
        if (Readable((void*)(calleeRsp + 0x28 + 8 * i), 8))
            memcpy(&st[i], (void*)(calleeRsp + 0x28 + 8 * i), 8);

    Log("[cap] %s caller=%llx cmd=%llx rdx=%llx r8=%llx r9=%llx st=%llx %llx %llx %llx %llx %llx\n",
        f.name, (unsigned long long)caller, (unsigned long long)rcx,
        (unsigned long long)rdx, (unsigned long long)r8, (unsigned long long)r9,
        (unsigned long long)st[0], (unsigned long long)st[1], (unsigned long long)st[2],
        (unsigned long long)st[3], (unsigned long long)st[4], (unsigned long long)st[5]);

    // Sell / Replace / SendToDepot / SetLine. The scripting layer's wrappers (our
    // own replays on the peer) live in one block, 0xcec000..0xcf2000 (ced378 =
    // buildProposal, cee710 = SetVehicleManualDeparture, ceefae = buyVehicle);
    // anything else is the UI. ReplaceVehicle (4) is in the list: without it a
    // player's "replace with this model" reached the wire nowhere and the peer
    // kept the old vehicle.
    // 13/14 (SetColor/SetName) ship through the same writer. Leaving them out
    // of this list meant the hook CAPTURED a rename -- '[cap] SetName' is in
    // the log -- and then wrote nothing, so renaming a line looked like a
    // replication failure when it never reached the wire at all.
    if ((f.id >= 3 && f.id <= 10) || f.id == 13 || f.id == 14) {
        bool luaPath = IsScriptCaller(caller);
        if (luaPath) {
            Log("[slice] %s from the Lua path (caller=%llx) -- a replay, not shipped\n",
                f.name, (unsigned long long)caller);
            if (f.id == 7) {
                __try { ClaimLineCreateCarrier(rcx); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] CreateLine: claim fault -- ignored\n"); }
            }
            if (f.id == 8) {
                // the platform assignment the click ran, re-run here on the rebuilt list (LINE PLATFORM ASSIGNMENT AT REPLAY)
                __try { ApplyLineAssignAtReplay(rdx, (int32_t)r8, r9); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[lineassign] LUPDATE replay: fault in the assignment -- the list is applied as shipped\n"); }
            }
        } else {
            // UpdateLine: decode the Line FIRST. A cancel is only honest when
            // the whole new stop list is on the wire; otherwise ship the event
            // as before and let it run natively (never cancel on a failed decode).
            g_lineDecodeOk = false;
            if (f.id == 8) {
                __try { g_lineDecodeOk = DecodeLine(r9, &g_lineDecode); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_lineDecodeOk = false; }
                if (!g_lineDecodeOk && cancel) {
                    Log("[slice] UpdateLine: Line decode failed (%s) -- NOT cancelled; event ships, peers read back. "
                        "LOCKSTEP AT RISK: this edit runs on this game first and the read-back may not carry it\n", g_lineDecodeWhy);
                    cancel = false;
                }
                g_lineAsgTag = (g_lineDecodeOk && cancel) ? LineAssignTagForCapture(rdx, (int32_t)r8) : -1;
            }
            if (f.id == 7) {
                // CreateLine: strict only when the whole create -- name, colour, line --
                // is on the wire (never cancel on a failed decode)
                __try { DecodeLineCreate(rdx, r8, st[0]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { g_lcDecodeOk = false; }
                if (!g_lcDecodeOk && cancel) {
                    Log("[slice] CreateLine: decode failed -- NOT cancelled; the event ships as before\n");
                    cancel = false;
                }
            }
            bool readable = false;
            __try { readable = VehiclePayloadReadable(f.id, r8, r9, st[0]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }
            if (!readable) {
                Log("[slice] %s: arguments not readable -- NOT cancelled, runs natively, not shipped\n", f.name);
                cancel = false;
            } else {
                const bool armed = cancel && SessionLive();
                WriteArmed(armed);
                // the spare line (ReadAndConsumeSpareLine): only for a create that IS
                // cancelled and decoded, so the Lua sees spare= exactly when the editor
                // was given that line
                InterlockedExchange(&g_lcSpareId, (f.id == 7 && armed && g_lcDecodeOk) ? ReadAndConsumeSpareLine() : 0);
                bool shipped = false;
                __try { shipped = WriteInjectVehicleCmd(f.id, r8, r9, st[0]); }
                __except (EXCEPTION_EXECUTE_HANDLER) { Log("[slice] %s decode fault -- not shipped\n", f.name); }
                if (!shipped && cancel) {
                    // ARMED 1 is on disk with no line behind it: take it back so the
                    // next capture cannot inherit it, and let this command run.
                    if (armed) WriteArmed(false);
                    Log("[slice] %s: nothing shipped -- NOT cancelled, runs natively\n", f.name);
                    cancel = false;
                }
            }
        }
    }

    if (f.id == 2 && caller == CALLER_LUA_VEHICLE) {
        Log("[slice] VBUY from the Lua path (caller=%llx) -- a replay, not shipped\n",
            (unsigned long long)caller);
    } else if (f.id == 2) {
        // A player's buy: r9 = depot entity (value), st[0] = pointer to the
        // by-value config copy on the caller's stack. WriteArmed runs at
        // CAPTURE time, before the Add hook decides whether the cancel can be
        // honoured, so it states an INTENTION. For the buy that intention is
        // not refused: its Add fires the depot window's callback and, if the
        // fire fails, honours the armed cancel anyway (g_pendingHonour).
        // So the config must read BEFORE anything is armed: a buy cancelled
        // with no VBUY behind it is a purchase that never happens.
        bool readable = false;
        __try { readable = VehiclePayloadReadable(2, r8, r9, st[0]); }
        __except (EXCEPTION_EXECUTE_HANDLER) { readable = false; }
        if (!readable) {
            Log("[slice] BuyVehicle: config not readable -- NOT cancelled, the buy runs natively, not shipped\n");
            cancel = false;
        } else {
            const bool armed = cancel && SessionLive();
            WriteArmed(armed);
            bool shipped = false;
            __try { shipped = WriteInjectVBuy(r9, st[0]); }
            __except (EXCEPTION_EXECUTE_HANDLER) { shipped = false; }
            if (!shipped) {
                if (armed) WriteArmed(false);   // take back the ARMED 1 that has no line behind it
                Log("[slice] VBUY not shipped -- NOT cancelled, the buy runs natively\n");
                cancel = false;
            }
        }
    }

    if (cancel && SessionLive()) {
        InterlockedExchange64(&g_pendingCmd, (LONG64)rcx);
        // BuyVehicle is the one vehicle command whose UI WAITS: the depot window
        // expects the new entity back, and cancelling it fire-and-forget crashed
        // the client on an assert (2026-08-28, caller 74fda9). That is why the
        // buy was left optimistic and why it is the last host-only asymmetry in
        // a vehicle's life. So it takes the BUILD TOOL's route instead --
        // g_pendingNoCb = 0, meaning the Add hook fires the completion callback
        // before cancelling, and if it cannot fire it lets the buy run rather
        // than wedge the window. Every other vehicle/line command genuinely has
        // nothing waiting and stays fire-and-forget.
        // ReplaceVehicle (4) waits too: the vehicle window reads the
        // replacement's result entity. Its callback is a
        // heap-allocated std::function, which the Add hook now resolves
        // through the _Getimpl slot at r9+0x38 (docs/re/COMMANDS.md).
        const bool waitsForResult = (f.id == 2 || f.id == 4);
        InterlockedExchange(&g_pendingNoCb, waitsForResult ? 0 : 1);
        // CreateLine: suppressed without firing (its callback asserts on an empty
        // result) -- the Add hook MOVES the callback into the stash for our replay
        InterlockedExchange(&g_pendingStashCb, f.id == 7 ? 1 : 0);
        InterlockedExchange(&g_pendingHonour, 1);
        Log("[slice] armed cancel: %s cmd=%llx (%s)\n", f.name,
            (unsigned long long)rcx,
            waitsForResult ? "callback WILL be fired -- the depot window waits on it"
                           : "no-callback");
    } else if (cancel) {
        Log("[slice] %s: no live session -- left alone, the game handles it\n", f.name);
    }
}

