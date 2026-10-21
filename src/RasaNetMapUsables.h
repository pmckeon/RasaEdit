#pragma once

// The use states Rasa.NET gives the usables the client's own .map files place (Rasa.Game/Managers/MapUsables.cs, read 2026-10-20).
// The client makes every .map entity itself, and a usable among them has no state - no state effect, no state animation - until
// the server sends one (usable.py: Usable.__init__ leaves _curStateId None; only Recv_UsableInfo / Recv_ForceState / Recv_Use set
// it). Rasa.NET keeps those states in code, not in the World database: the table `MapUsables.All` of (map context id, entity id,
// class, position, state, ...), written through a few helpers (DissectionTable, Running, Monitor, ...). The editor reads that source
// file - so it follows upstream changes - rather than copying it: Rasa.Game/Data/UseObjectState.cs for the state names,
// MapUsables.cs for the map constants, the helpers and the table.
#include <cstdint>
#include <map>
#include <string>

namespace rasanet {

struct MapUsableStates {
    std::map<uint64_t, int32_t> states;     // entity id -> use state, for one map context
    std::string error;                       // why nothing was read ("" = read, or no Rasa.NET source found)
    std::string source;                      // the MapUsables.cs that was read
};

// The Rasa.NET source folder (the one holding src/Rasa.Game/Managers/MapUsables.cs, or src itself, or Rasa.Game): `configured`
// when set, else found walking up from the World database file's folder (the server's bin folder sits inside the source tree).
std::string findSource(const std::string& configured, const std::string& databasePath);

// The states Rasa.NET sends for the usables of map context `mapContextId`, read from the source at `sourceRoot`.
MapUsableStates mapUsableStates(const std::string& sourceRoot, uint32_t mapContextId);

}  // namespace rasanet
