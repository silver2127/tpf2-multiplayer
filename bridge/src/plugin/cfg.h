// cfg.h -- the ONE config parser.
//
// Replaces four ad-hoc readers (bridge_main's sscanf ladder, slice_hook's
// ReadCfg and CfgHas, the menu flags file), each of which had its own idea of
// what a line means. The audit found four separate defects that all came from
// that duplication:
//
//   * substring matching -- CfgHas built "<key>=1" and ran strstr over the
//     line, so "cancel_line=1" also matched anything ending in that text, and
//     no key could ever be turned off with =0 (only by being absent).
//   * asymmetric no-file defaults -- ReadCfg treated a missing file as
//     suppress=1 while CfgHas returned true only for two hardcoded keys, so a
//     peer with no cfg silently ran a different protocol from one with a cfg.
//   * fixed 128-byte lines -- fgets split a longer line, and the TAIL was then
//     parsed as a fresh line, so a long comment containing "suppress=1" flipped
//     a switch it was supposed to document.
//   * no sections -- every DLL needed its own file to avoid key collisions.
//
// Format (INI):
//   # comment            -- '#' or ';' as the first non-blank character
//   [section]            -- everything after this belongs to that section
//   key = value          -- whitespace around key and value is trimmed
//
// Keys are matched EXACTLY and case-insensitively. Lines are read whole, at any
// length. A value may contain '=', '#' and ';' -- only a line whose first
// non-blank character is a comment marker is a comment, so inline "# ..." after
// a value is part of the value. That is deliberate: paths and format strings
// contain '#', and a trailing-comment rule silently truncates them.
#pragma once
#include <string>

namespace tpf2mp {

// Load (or reload) the config. Looks for `tpf2mp.cfg` in cfgDir first (the
// directory the host DLL lives in, where the installer puts it), then dataDir
// (a user override that survives reinstalls). Both may be empty. Returns the
// path actually loaded, or "" when no file was found -- in which case every
// lookup returns its caller's default, which is the documented behaviour rather
// than a special "no file" mode.
const std::string& CfgLoad(const wchar_t* cfgDir, const wchar_t* dataDir);

// Path of the file in use ("" if none). For logging.
const std::string& CfgPath();

// Merge one more INI file into the loaded table. Keys it sets override keys
// already present; keys it does not mention are left alone. Returns false if
// the file is absent or unreadable (which is not an error: it is optional).
//
// This is how a plugin shipped by a DIFFERENT installer gets its settings: it
// drops <plugins>\<name>.cfg beside its DLL instead of editing tpf2mp.cfg,
// which some other product may own. Two installers then never fight over one
// file, and uninstalling either leaves the other's settings intact.
bool CfgMergeFile(const wchar_t* path);

// Lookups. `section` may be null or "" for keys before any [section] header.
const char* CfgStr(const char* section, const char* key, const char* def);
int         CfgInt(const char* section, const char* key, int def);

// Accepts 1/0, true/false, yes/no, on/off (any case). Anything else is `def`.
// NOTE the contrast with the old CfgHas: "key=0" really means false here, and
// an absent key means `def` -- not false.
bool        CfgBool(const char* section, const char* key, bool def);

// True if the key is present at all, whatever its value.
bool        CfgHasKey(const char* section, const char* key);

}  // namespace tpf2mp
