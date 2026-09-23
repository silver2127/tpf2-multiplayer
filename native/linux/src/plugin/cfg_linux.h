// cfg_linux.h -- the Linux build of plugin/cfg.h: the ONE config parser.
//
// Same format, same rules and the same lookups as cfg.h, whose comment lists
// the four defects this parser replaced. What differs is the path arguments:
// UTF-8 char paths instead of wchar_t, and CfgLoad takes the whole candidate
// list because the Linux host looks in up to three folders, not two
// (host_linux.cpp says why).
//
// Format (INI):
//   # comment            -- '#' or ';' as the first non-blank character
//   [section]            -- everything after this belongs to that section
//   key = value          -- whitespace around key and value is trimmed
// Sections and keys are matched exactly and case-insensitively (ASCII). A
// value may contain '=', '#' and ';': only a line whose first non-blank
// character is a comment marker is a comment.
//
// One deliberate difference: a pointer CfgStr returned stays valid for the life
// of the process, even after a later CfgMergeFile or CfgLoad replaces that key.
// tpf2mp_plugin.h promises plugins that the host keeps every pointer it hands
// out alive, and a plugin's own .cfg is merged after the plugins before it have
// already read their settings. cfg.cpp overwrote the string in place.
//
// Also new: a file that exists but cannot be read (no permission, a directory,
// an I/O error) is skipped as on Windows, but the caller is told, so the host
// can log it instead of silently dropping its settings.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tpf2mp {

// Load (or reload) the config: `tpf2mp.cfg` from the first directory in `dirs`
// that has a readable one. The rest are not read: the first file found wins,
// with no merging. A trailing '/' on a directory is optional; empty entries are
// skipped. Returns the path actually loaded, or "" when none was found, in
// which case every lookup returns its caller's default.
// `unreadable`, when given, receives each tpf2mp.cfg passed over because it
// exists but could not be read, with the errno (anything but ENOENT).
const std::string& CfgLoad(const std::vector<std::string>& dirs,
                           std::vector<std::pair<std::string, int> >* unreadable = nullptr);

// Path of the file in use ("" if none). For logging.
const std::string& CfgPath();

// Merge one more INI file into the loaded table: keys it sets override keys
// already present, keys it does not mention stay. Returns false when the file
// is absent or unreadable; that is not an error in itself (the file is
// optional). `err`, when given, is 0 after a merge, ENOENT when there is no such
// file, EINVAL for a null or empty path, and otherwise the errno of the failed
// open or read. The caller should report those: the file's settings, enabled=0
// included, are then missing.
bool CfgMergeFile(const char* path, int* err = nullptr);

// Lookups. `section` may be null or "" for keys before any [section] header.
const char* CfgStr(const char* section, const char* key, const char* def);

// strtol with base 0 (0x.. hex, and a leading 0 means octal); the whole value
// must parse, else `def`. Clamped to the int range, which is what the Windows
// build's 32-bit long gives for an out-of-range value.
int         CfgInt(const char* section, const char* key, int def);

// Accepts 1/0, true/false, yes/no, on/off (any case). Anything else is `def`,
// and so is an absent key.
bool        CfgBool(const char* section, const char* key, bool def);

// True if the key is present at all, whatever its value.
bool        CfgHasKey(const char* section, const char* key);

}  // namespace tpf2mp
