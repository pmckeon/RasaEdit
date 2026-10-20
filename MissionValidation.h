#pragma once

// The checks Rasa.NET runs on the World database's mission content when the Game server starts (Rasa.Game, upstream development
// 5047942: MissionContentCatalog -> MissionContentLoader + MissionContentValidator, with MissionObjectiveRuntimeAnalyzer and
// MissionProgressRuleAuthoring), ported so the editor can show what the server would do with the content before it is saved:
//   - a start-up error: the server throws and does not start (no enabled mission, two enabled revisions of one mission, a JSON
//     column it cannot read, an aggregate counter that conflicts, an object conversation on a class that cannot talk);
//   - a diagnostic on a mission: the server leaves that mission out (MissionDefinitionCatalog skips missions with diagnostics);
//     on a required mission (requirement 1) it also never reports ready (Server.LogMissionValidationAndCheckReadiness), and
//     required missions that need an inactive one are reported too ("required-chain-inactive").
//   - the catalog's scene phase (MissionContentCatalog.Load after the validator): each enabled revision's mission_scene_binding
//     document (MissionSceneValidation.Validate / ValidateDialogueActions / ValidateSharing, script registry, public spawns, credit,
//     mission items and their drops, dialogue topics, object conversations, SceneRuntime on every sequence), the enabled
//     mission_experience_binding rows, radio offers, shared actors, actor gameplay policies, and every operational mission's
//     requirements and item operations - all start-up errors (the server throws).
// The diagnostic codes and texts are the server's. Only enabled rows are checked (the server reads one enabled revision per
// mission). The scene documents are first read against the server's own System.Text.Json contract (MissionSceneSchema.h, dumped
// by tools/missioncheck --schema; missionSceneReadError). Not ported: the
// built-in recovered mission definitions (MissionDefinitionCatalog.CreateRecoveredInactiveDefinitions), and the GetTypedScenarioIds
// test for scripted scenarios still accepts any binding whose script key and state version agree with its row.

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
    std::set<std::pair<int64_t, int64_t>> npcPackageRows;   // npc_package: (id = creature id, package_id)
    std::set<int64_t> spawnPoolIds;                  // spawnpool.id (scene public spawns)
    std::set<int64_t> itemTemplateIds;               // itemtemplate.id
    std::map<int64_t, int64_t> itemClassStackSizes;  // itemclass: id -> stack_size
    struct Experience {                              // enabled mission_experience_binding rows (read-only in the editor)
        std::string key;
        int64_t mapContextId = 0;
        std::string bindings;
    };
    std::vector<Experience> experiences;
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

struct JsonValue;
// Why the server's deserialiser (System.Text.Json with MissionContentCodec.Options) cannot read `document` as a MissionSceneDefinition
// (experience: a MissionExperienceDefinition) - unmapped members, wrong value types, discriminators, required members, the
// constructors that throw; "" when it can. MissionContentLoader / MissionContentCatalog stop the server on such a document.
std::string missionSceneReadError(const JsonValue& document, bool experience);
