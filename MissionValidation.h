#pragma once

// The checks Rasa.NET runs on the World database's mission content when the Game server starts (Rasa.Game, upstream development
// 5047942: MissionContentCatalog -> MissionContentLoader + MissionContentValidator, with MissionObjectiveRuntimeAnalyzer and
// MissionProgressRuleAuthoring), ported so the editor can show what the server would do with the content before it is saved:
//   - a start-up error: the server throws and does not start (no enabled mission, two enabled revisions of one mission, a JSON
//     column it cannot read, an aggregate counter that conflicts, an object conversation on a class that cannot talk);
//   - a diagnostic on a mission: the server leaves that mission out (MissionDefinitionCatalog skips missions with diagnostics);
//     on a required mission (requirement 1) it also never reports ready (Server.LogMissionValidationAndCheckReadiness), and
//     required missions that need an inactive one are reported too ("required-chain-inactive").
// The diagnostic codes and texts are the server's. Only enabled rows are checked (the server reads one enabled revision per
// mission). NOT ported: the scene-binding document checks the catalog runs afterwards (MissionSceneValidation, scene items,
// credit, public spawns - mission_scene_binding is read-only in the editor) and MissionSceneValidation.ValidateProgressMetadata;
// which scene scripts the server's registry resolves is approximated (a binding whose script key and state version agree).

#include "WorldDatabase.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

struct MissionDiagnostic {
    std::string code;                    // the server's code, e.g. "missing-area"
    std::string message;
    int64_t missionId = -1;              // -1: none (start-up errors about the whole content)
    std::string revision;
    int64_t objectiveId = -1, transitionId = -1, triggerId = -1, actionId = -1, scenarioId = -1, stepId = -1;
    bool startup = false;                // the server does not start at all
};

// The World tables the checks look ids up in.
struct MissionReferences {
    std::set<int64_t> npcPackageIds;                 // npc_package.package_id
    std::map<int64_t, int64_t> itemTemplateClasses;  // itemtemplate_itemclass: template -> item class
    std::set<int64_t> entityClassIds;                // entityclass.id
    std::map<int64_t, std::string> entityClassAugs;  // entityclass.aug_list
    std::map<int64_t, int64_t> creatureClasses;      // creature: id -> class_id
    std::set<int64_t> mapContextIds;                 // map_info.map_context_id
    std::set<int64_t> creatureFlagIds;               // creature_class_flag.flag_id (every flag some class carries)
    static MissionReferences load(const WorldDatabase& database);   // throws std::runtime_error
};

struct MissionValidationResult {
    std::vector<MissionDiagnostic> diagnostics;      // start-up errors first, then by mission
    std::set<int64_t> inactiveMissions;              // missions the server would leave out
    bool startupFails = false;
    bool blocksReadiness = false;                    // a required mission has a diagnostic
    size_t enabledMissions = 0;
};

// tables: the mission content tables by name (mission_content_definition and the children the editor loads).
MissionValidationResult validateMissionContent(const std::map<std::string, const DbTable*>& tables, const MissionReferences& references);
