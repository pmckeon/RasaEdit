#include "MissionValidation.h"

#include "Json.h"
#include "MissionSceneSchema.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <stdexcept>
#include <tuple>

// Ported from Rasa.NET upstream development 5047942 (see MissionValidation.h). Function names follow the C# ones; the
// enum values are Rasa.NET's (MissionTriggerKind 1-5, MissionActionKind 1-13, MissionScenarioStepKind 1-23,
// MissionObjectiveState 0-4, MissionProgressEventKind 0-15, MissionAreaShape 1-4, TutorialId, CharacterQualificationKey).

namespace {

using Opt = std::optional<int64_t>;

// MissionProgressEventKind
enum Event : int64_t {
    WaypointAcquired = 0, LogosAcquired, CreatureKilled, MissionCompleted, ItemAcquired, ItemConsumed, InteractionUsed, AreaEntered, ItemEquipped,
    AbilityHit, ScenarioEvent, DeadlineElapsed, ObjectiveStateReached, ObjectHit, CreatureFlagKilled, CreatureClassKilled
};
const char* eventName(int64_t k) {
    static const char* names[] = {"WaypointAcquired", "LogosAcquired",       "CreatureKilled",  "MissionCompleted", "ItemAcquired",  "ItemConsumed",
                                  "InteractionUsed",  "AreaEntered",         "ItemEquipped",    "AbilityHit",       "ScenarioEvent", "DeadlineElapsed",
                                  "ObjectiveStateReached", "ObjectHit",      "CreatureFlagKilled", "CreatureClassKilled"};
    return k >= 0 && k <= 15 ? names[k] : "?";
}
bool isKill(int64_t k) { return k == CreatureKilled || k == CreatureFlagKilled || k == CreatureClassKilled; }
const char* actionName(int64_t k) {
    static const char* names[] = {"", "RevealObjective", "ActivateObjective", "CompleteObjective", "GrantReward", "StartScenario", "ActivateSpawnGroup",
                                  "ShowIndicator", "SetPlayerFlag", "ShowAmbientConversation", "IssueMissionItem", "ConsumeMissionItem",
                                  "RemoveMissionItems", "FailRelatedMission"};
    return k >= 1 && k <= 13 ? names[k] : "?";
}
const char* triggerName(int64_t k) {
    static const char* names[] = {"", "Conversation", "ProgressEvent", "ObjectiveState", "AreaEntered", "TimerElapsed"};
    return k >= 1 && k <= 5 ? names[k] : "?";
}
bool validState(Opt v) { return v && *v >= 0 && *v <= 4; }
std::string optText(Opt v) { return v ? std::to_string(*v) : std::string("null"); }

// TutorialId (Rasa.Game/Data/TutorialId.cs)
bool validTutorial(int64_t id) {
    if (id >= 1 && id <= 9) return true;
    if (id >= 10000000 && id <= 10000040) return true;
    return id >= 10000043 && id <= 10000048;
}

struct Row {
    const DbTable* t = nullptr;
    const std::vector<DbValue>* r = nullptr;
    const DbValue* v(const char* c) const {
        const int i = t->column(c);
        return i < 0 ? nullptr : &(*r)[size_t(i)];
    }
    Opt o(const char* c) const {
        const DbValue* x = v(c);
        if (!x || x->isNull()) return std::nullopt;
        return x->asInt();
    }
    int64_t i(const char* c) const { return o(c).value_or(0); }
    std::optional<double> od(const char* c) const {
        const DbValue* x = v(c);
        if (!x || x->isNull()) return std::nullopt;
        return x->asReal();
    }
    std::string s(const char* c) const {
        const DbValue* x = v(c);
        return x && x->type == DbValue::Type::Text ? x->t : std::string();
    }
    bool isNull(const char* c) const {
        const DbValue* x = v(c);
        return !x || x->isNull();
    }
};

struct Trigger {
    int64_t id = 0, kind = 0;
    Opt eventKind, subject, counter, initial, target, area, duration, npc, flag, relatedObjective, relatedState;
    std::optional<bool> sourceSpawnResolved;
    bool hasEventKind() const { return eventKind && *eventKind >= 0 && *eventKind <= 15; }
};
struct Action {
    int64_t id = 0, kind = 0;
    Opt targetObjective, reward, spawnGroup, scenario, indicator, flagId, flagValue, npc;
    std::string intentKind;              // "$kind" of item_intent, "" for none
    JsonValue intent;                    // the readable item_intent (Null when none)
};
struct Transition {
    int64_t objective = 0, id = 0, sequence = 0;
    Opt toState;                         // a valid MissionObjectiveState, else none
    std::vector<Trigger> triggers;
    std::vector<Action> actions;
};
struct Counter {
    int64_t id = 0, initial = 0, target = 0;
};
enum RuleType { CompleteExact, CompleteDistinctSet, IncrementExactCounter, IncrementExactItemCounter };
struct Rule {
    int64_t kind = 0;
    RuleType type = CompleteExact;
    std::set<int64_t> subjects;
    Opt counter, initial, target, scope, detail;
    std::optional<bool> sourceSpawnResolved;
};
struct Executable {
    int64_t id = 0, sequence = 0;
    Opt toState;
    size_t conversations = 0;
    std::vector<std::pair<int64_t, int64_t>> conversationKeys;   // (npc package, player flag), all of type Completion
    std::vector<Action> actions;
    std::optional<Rule> rule;
    std::map<int64_t, Counter> counters, itemCounters;
    std::vector<int64_t> revealed, activated;
    bool zeroSubject = false;
};
struct RuntimeDiagnostic {
    std::string code, message;
    int64_t transitionId = -1;
};
struct Aggregation {
    std::vector<int64_t> children;
    int64_t target = 0;
    Opt counter;
};
struct Objective {
    int64_t id = 0, ordinal = 0;
    Opt nameText, bodyText, counterText[3], initialState;
    bool visible = true, recognizeExisting = false;
    std::optional<bool> required;            // is_required
    std::optional<Aggregation> aggregation, history;
    std::vector<RuntimeDiagnostic> runtime;
    bool zeroSubject = false;                // a transition's rule has a zero subject (the server throws)
    std::vector<int64_t> revealed, activated;
    std::optional<Rule> rule;
    std::map<int64_t, Counter> counters, itemCounters;
    std::vector<Executable> executable;
    size_t conversations = 0;
    std::vector<std::pair<int64_t, double>> indicators;     // id, radius
    bool isAggregate() const { return aggregation || history; }
};
struct Reward {
    int64_t id = 0, selection = 0;
    struct Item { int64_t id = 0, templateId = 0, quantity = 0; int64_t module1 = 0, module2 = 0, module3 = 0, module4 = 0; };
    std::vector<Item> fixed, selectable;
};
struct Area {
    int64_t id = 0, map = 0, shape = 0;
    std::optional<double> radius;
};
struct SpawnGroup {
    int64_t id = 0, map = 0, policy = 0;
    Opt area, respawn;
    struct Spawn { int64_t id = 0, creature = 0, quantity = 0; };
    std::vector<Spawn> spawns;
};
struct Step {
    int64_t id = 0, kind = 0;
    Row row;
};
struct Scenario {
    int64_t id = 0;
    std::vector<Step> steps;
};
struct Definition {
    int64_t missionId = 0;
    std::string revision;
    Row row;
    int64_t requirement = 0;
    std::map<int64_t, Objective> objectives;
    std::vector<Transition> transitions;
    std::vector<Row> prerequisites;
    std::map<int64_t, Reward> rewards;
    std::map<int64_t, Area> areas;
    std::map<int64_t, SpawnGroup> spawnGroups;
    std::map<int64_t, Scenario> scenarios;
    // mission_channel_policy (defaults NPC / NPC, no radio sources) and mission_repeat_policy
    int64_t acceptance = 1, completion = 1;
    struct RadioSource { int64_t kind = -1; std::string key; Opt map; bool valid = true; JsonValue requirement; };
    std::vector<RadioSource> radioSources;
    bool hasRepeat = false;
    int64_t repeatKind = 0;
    Opt cooldown, resetSecond;
    std::set<int64_t> objectNpcPackages;
    bool hasTypedScenarios = false;
    std::set<int64_t> typedScenarios;
    // mission_scene_binding of this revision: its document and the row's script_key / state_version.
    bool hasScene = false;
    bool sceneUnreadable = false;        // the loader could not read it (a start-up error already)
    JsonValue scene;
    std::string sceneScriptKey;
    int64_t sceneStateVersion = 0;
};

// ---- MissionProgressRuleAuthoring -------------------------------------------------------------------------------------------
bool monotonic(const char* ruleLabel, const char* identifierLabel, int64_t identifier, int64_t initial, int64_t target, std::string& diagnostic) {
    if (target > initial) return true;
    diagnostic = std::string(ruleLabel) + " " + identifierLabel + " " + std::to_string(identifier) + " has initial_value " + std::to_string(initial) +
                 " and target_value " + std::to_string(target) +
                 "; target_value must be greater than initial_value so runtime monotonic progress can advance.";
    return false;
}

bool buildDistinctSet(const std::vector<const Trigger*>& ts, Rule& rule, std::map<int64_t, Counter>& counters, std::string& diagnostic) {
    const int64_t kind = *ts[0]->eventKind;
    if (kind != WaypointAcquired && kind != LogosAcquired) {
        diagnostic = "multiple progress event triggers only support waypoint or Logos distinct-subject rules, or kill triggers that share one counter.";
        return false;
    }
    for (const Trigger* t : ts)
        if (!t->hasEventKind() || *t->eventKind != kind || !t->subject || *t->subject == 0 || t->sourceSpawnResolved) {
            diagnostic = "distinct progress subject rules require one event_kind, non-zero subject_id values, and no spawn parameters.";
            return false;
        }
    std::set<int64_t> subjects;
    for (const Trigger* t : ts) subjects.insert(*t->subject);
    const Trigger& first = *ts[0];
    bool any = false;
    for (const Trigger* t : ts) any = any || t->counter || t->initial || t->target;
    if (any) {
        bool ok = first.counter.has_value();
        for (const Trigger* t : ts) ok = ok && t->counter == first.counter && t->initial == Opt(0) && t->target == Opt(int64_t(subjects.size()));
        if (!ok) {
            diagnostic = "distinct counter rules require one shared counter_id, initial_value 0, and target_value equal to the distinct subject count.";
            return false;
        }
        counters[*first.counter] = Counter{*first.counter, 0, int64_t(subjects.size())};
    }
    rule = Rule{kind, CompleteDistinctSet, subjects, first.counter};
    return true;
}

bool buildKillCounter(const std::vector<const Trigger*>& ts, Rule& rule, std::map<int64_t, Counter>& counters, std::string& diagnostic) {
    const Trigger& first = *ts[0];
    const int64_t kind = *first.eventKind;
    bool ok = first.counter && first.initial && first.target;
    for (const Trigger* t : ts)
        ok = ok && t->hasEventKind() && *t->eventKind == kind && t->subject && *t->subject != 0 && t->counter == first.counter &&
             t->initial == first.initial && t->target == first.target && !t->sourceSpawnResolved;
    if (!ok) {
        diagnostic = "several kill triggers on one transition must share one event_kind and one counter_id, initial_value and target_value, with non-zero "
                     "subject_id values and no source_spawn_resolved.";
        return false;
    }
    if (!monotonic("counter progress rule", "counter_id", *first.counter, *first.initial, *first.target, diagnostic)) return false;
    counters[*first.counter] = Counter{*first.counter, *first.initial, *first.target};
    rule = Rule{kind, IncrementExactCounter, {}, first.counter, first.initial, first.target};
    for (const Trigger* t : ts) rule.subjects.insert(*t->subject);
    return true;
}

bool buildScopedSceneSet(int64_t missionId, const std::vector<const Trigger*>& ts, Rule& rule, std::map<int64_t, Counter>& counters, std::string& diagnostic) {
    const Trigger& first = *ts[0];
    const int64_t n = int64_t(ts.size());
    bool bad = missionId == 0 || !first.counter || *first.counter == 0 || first.initial != Opt(0) || first.target != Opt(n);
    std::set<Opt> distinct;
    for (const Trigger* t : ts) {
        bad = bad || !t->hasEventKind() || *t->eventKind != ScenarioEvent || t->counter != first.counter || t->initial != Opt(0) ||
              t->target != first.target || !t->subject || *t->subject == 0 || *t->subject > n || t->sourceSpawnResolved;
        distinct.insert(t->subject);
    }
    if (bad || int64_t(distinct.size()) != n) {
        diagnostic = "scoped scene counters require distinct events 1..N in one mission/scenario, initial_value 0 and target_value equal to the authored event count.";
        return false;
    }
    counters[0] = Counter{0, 0, *first.target};
    rule = Rule{ScenarioEvent, CompleteDistinctSet, {}, Opt(0), std::nullopt, std::nullopt, missionId, first.counter};
    for (const Trigger* t : ts) rule.subjects.insert(*t->subject);
    return true;
}

bool tryBuildProgressRule(int64_t missionId, const std::vector<const Trigger*>& ts, Rule& rule, std::map<int64_t, Counter>& counters,
                          std::map<int64_t, Counter>& itemCounters, std::string& diagnostic) {
    if (ts.empty()) return false;
    for (const Trigger* t : ts)
        if (!t->hasEventKind()) {
            diagnostic = "progress event trigger uses an unknown event_kind value.";
            return false;
        }
    if (ts.size() > 1) {
        const int64_t shared = *ts[0]->eventKind;
        if (isKill(shared)) return buildKillCounter(ts, rule, counters, diagnostic);
        if (shared == ScenarioEvent) return buildScopedSceneSet(missionId, ts, rule, counters, diagnostic);
        return buildDistinctSet(ts, rule, counters, diagnostic);
    }
    const Trigger& t = *ts[0];
    const int64_t kind = *t.eventKind;
    if (!t.subject || *t.subject == 0) {
        diagnostic = "progress event trigger must declare a non-zero subject_id.";
        return false;
    }
    const bool hasCounter = t.counter.has_value(), hasInitial = t.initial.has_value(), hasTarget = t.target.has_value(),
               hasSsr = t.sourceSpawnResolved.has_value();
    if (kind == WaypointAcquired || kind == LogosAcquired) return buildDistinctSet(ts, rule, counters, diagnostic);
    if (kind == ItemAcquired || kind == ItemConsumed) {
        if (!hasCounter && hasInitial && hasTarget && !hasSsr) {
            if (!monotonic("item counter progress rule", "subject_id", *t.subject, *t.initial, *t.target, diagnostic)) return false;
            itemCounters[*t.subject] = Counter{*t.subject, *t.initial, *t.target};
            rule = Rule{kind, IncrementExactItemCounter, {*t.subject}, t.subject, t.initial, t.target};
            return true;
        }
        if (hasCounter || hasInitial || hasTarget || hasSsr) {
            diagnostic = std::string(eventName(kind)) +
                         " progress rules must either use subject_id only or use initial_value and target_value without counter_id or source_spawn_resolved.";
            return false;
        }
        rule = Rule{kind, CompleteExact, {*t.subject}};
        return true;
    }
    if (kind == ItemEquipped) {
        if (hasCounter || hasInitial || hasTarget) {
            diagnostic = "item equipped progress rules support subject_id only, with optional source_spawn_resolved=true to match item_template_id.";
            return false;
        }
        rule = Rule{kind, CompleteExact, {*t.subject}};
        rule.sourceSpawnResolved = t.sourceSpawnResolved == std::optional<bool>(true);
        return true;
    }
    if (kind == AbilityHit) {
        if (!hasCounter || *t.counter == 0 || hasInitial || hasTarget || hasSsr) {
            diagnostic = "ability hit progress rules must declare subject_id as action_id and counter_id as target creature_id, without counter ranges or "
                         "source_spawn_resolved.";
            return false;
        }
        rule = Rule{kind, CompleteExact, {*t.subject}};
        rule.detail = t.counter;
        return true;
    }
    if (kind == ScenarioEvent) {
        if (hasInitial || hasTarget) return buildScopedSceneSet(missionId, ts, rule, counters, diagnostic);
        if (!hasCounter || *t.counter == 0 || hasSsr) {
            diagnostic = "scenario event progress rules must declare subject_id as scenario_event_id and counter_id as scenario_id, without counter ranges or "
                         "source_spawn_resolved.";
            return false;
        }
        rule = Rule{kind, CompleteExact, {*t.subject}};
        rule.scope = missionId;
        rule.detail = t.counter;
        return true;
    }
    if (kind == ObjectHit) {
        if (!hasCounter || *t.counter == 0 || hasInitial || hasTarget || hasSsr) {
            diagnostic = "object hit progress rules require subject_id as entity_class_id and counter_id as action_id, without counter ranges or source_spawn_resolved.";
            return false;
        }
        rule = Rule{kind, CompleteExact, {*t.subject}};
        rule.detail = t.counter;
        return true;
    }
    if (hasCounter || hasInitial || hasTarget) {
        if (!hasCounter || !hasInitial || !hasTarget || hasSsr) {
            diagnostic = "counter progress rules must declare counter_id, initial_value, and target_value together, without source_spawn_resolved.";
            return false;
        }
        if (!monotonic("counter progress rule", "counter_id", *t.counter, *t.initial, *t.target, diagnostic)) return false;
        counters[*t.counter] = Counter{*t.counter, *t.initial, *t.target};
        rule = Rule{kind, IncrementExactCounter, {*t.subject}, t.counter, t.initial, t.target};
        return true;
    }
    if (hasSsr && kind != CreatureKilled && kind != ItemEquipped) {
        diagnostic = "source_spawn_resolved is only supported for creature kill rules or item equipped template matching.";
        return false;
    }
    rule = Rule{kind, CompleteExact, {*t.subject}};
    rule.sourceSpawnResolved = t.sourceSpawnResolved;
    return true;
}

// ---- MissionObjectiveRuntimeAnalyzer ----------------------------------------------------------------------------------------
bool tryBuildExecutable(int64_t missionId, const Transition& tr, Executable& out, std::string& diagnostic) {
    out = Executable{tr.id, tr.sequence, tr.toState};
    out.actions = tr.actions;
    for (const Action& a : tr.actions) {
        if (a.kind == 1 && a.targetObjective && std::find(out.revealed.begin(), out.revealed.end(), *a.targetObjective) == out.revealed.end())
            out.revealed.push_back(*a.targetObjective);
        if (a.kind == 2 && a.targetObjective && std::find(out.activated.begin(), out.activated.end(), *a.targetObjective) == out.activated.end())
            out.activated.push_back(*a.targetObjective);
    }
    size_t conversation = 0;
    for (const Trigger& t : tr.triggers) conversation += t.kind == 1 && t.npc && t.flag ? 1 : 0;
    if (conversation > 0) {
        if (conversation != tr.triggers.size()) {
            diagnostic = "transition " + std::to_string(tr.id) +
                         " mixes conversation triggers with other trigger kinds; author one executable trigger shape per transition.";
            return false;
        }
        out.conversations = conversation;
        for (const Trigger& t : tr.triggers) out.conversationKeys.emplace_back(*t.npc, *t.flag);
        return true;
    }
    std::vector<const Trigger*> progress, area, objectiveState, timer;
    for (const Trigger& t : tr.triggers) {
        if (t.kind == 2) progress.push_back(&t);
        if (t.kind == 4) area.push_back(&t);
        if (t.kind == 3) objectiveState.push_back(&t);
        if (t.kind == 5) timer.push_back(&t);
    }
    Rule rule;
    if (!progress.empty()) {
        if (progress.size() != tr.triggers.size()) {
            diagnostic = "transition " + std::to_string(tr.id) +
                         " mixes progress-event triggers with other trigger kinds; author one executable trigger shape per transition.";
            return false;
        }
        if (!tryBuildProgressRule(missionId, progress, rule, out.counters, out.itemCounters, diagnostic)) return false;
    } else if (!area.empty()) {
        if (area.size() != 1 || tr.triggers.size() != 1 || !area[0]->area || *area[0]->area == 0) {
            diagnostic = "transition " + std::to_string(tr.id) + " must use exactly one area trigger with a non-zero area_id.";
            return false;
        }
        rule = Rule{AreaEntered, CompleteExact, {*area[0]->area}};
        rule.scope = missionId;
    } else if (!objectiveState.empty()) {
        const Trigger& t = *objectiveState[0];
        if (objectiveState.size() != 1 || tr.triggers.size() != 1 || !t.relatedObjective || *t.relatedObjective == 0 || !validState(t.relatedState)) {
            diagnostic = "transition " + std::to_string(tr.id) +
                         " must use exactly one objective-state trigger with a non-zero related_objective_id and a valid related_state.";
            return false;
        }
        rule = Rule{ObjectiveStateReached, CompleteExact, {*t.relatedObjective}};
        rule.scope = missionId;
        rule.detail = t.relatedState;
    } else if (!timer.empty()) {
        if (timer.size() != 1 || tr.triggers.size() != 1 || !timer[0]->duration || *timer[0]->duration == 0) {
            diagnostic = "transition " + std::to_string(tr.id) + " must use exactly one timer trigger with a positive duration_seconds value.";
            return false;
        }
        rule = Rule{DeadlineElapsed, CompleteExact, {tr.objective}};
        rule.scope = missionId;
    } else {
        return false;                                // no executable shape (no diagnostic)
    }
    // MissionProgressRule's constructor: a distinct set with a counter counts 0 .. the number of subjects; zero subjects throw
    // (the loader's exception stops the server).
    if (rule.type == CompleteDistinctSet && rule.counter) {
        rule.initial = 0;
        rule.target = int64_t(rule.subjects.size());
    }
    out.zeroSubject = rule.subjects.empty() || rule.subjects.count(0) != 0;
    out.rule = rule;
    return true;
}

void analyzeObjective(int64_t missionId, Objective& o, std::vector<const Transition*> transitions) {
    std::stable_sort(transitions.begin(), transitions.end(), [](const Transition* a, const Transition* b) {
        return a->sequence != b->sequence ? a->sequence < b->sequence : a->id < b->id;
    });
    std::vector<RuntimeDiagnostic> diagnostics;
    std::vector<Executable> executable;
    std::set<int64_t> revealed, activated;
    std::map<int64_t, Counter> counters, itemCounters;
    size_t conversations = 0;
    for (const Transition* t : transitions) {
        Executable e;
        std::string diagnostic;
        if (!tryBuildExecutable(missionId, *t, e, diagnostic)) {
            if (!diagnostic.empty()) diagnostics.push_back(RuntimeDiagnostic{"invalid-trigger-shape", diagnostic, t->id});
            continue;
        }
        conversations += e.conversations;
        revealed.insert(e.revealed.begin(), e.revealed.end());
        activated.insert(e.activated.begin(), e.activated.end());
        const auto merge = [&](const std::map<int64_t, Counter>& source, std::map<int64_t, Counter>& destination, const char* code, const char* what) {
            for (const auto& entry : source) {
                const auto it = destination.find(entry.first);
                if (it == destination.end()) {
                    destination.insert(entry);
                    continue;
                }
                if (it->second.initial == entry.second.initial && it->second.target == entry.second.target) continue;
                diagnostics.push_back(RuntimeDiagnostic{code,
                                                        "objective " + std::to_string(o.id) + " transition " + std::to_string(t->id) + " authors " + what + " " +
                                                            std::to_string(entry.first) + " with a different range than another executable transition.",
                                                        t->id});
            }
        };
        merge(e.counters, counters, "conflicting-progress-counters", "counter");
        merge(e.itemCounters, itemCounters, "conflicting-progress-item-counters", "item counter");
        o.zeroSubject = o.zeroSubject || e.zeroSubject;
        executable.push_back(std::move(e));
    }
    o.runtime = diagnostics;
    if (!diagnostics.empty()) return;            // the analysis keeps nothing else
    o.revealed.assign(revealed.begin(), revealed.end());
    o.activated.assign(activated.begin(), activated.end());
    o.conversations = conversations;
    o.counters = counters;
    o.itemCounters = itemCounters;
    size_t withRule = 0;
    for (const Executable& e : executable) withRule += e.rule ? 1 : 0;
    if (withRule == 1)
        for (const Executable& e : executable)
            if (e.rule) o.rule = e.rule;
    o.executable = std::move(executable);
}

// MissionProgressRule.IsCompatible
bool ruleCompatible(const Rule& r, const Objective& o) {
    if (r.type == IncrementExactItemCounter) {
        if (!r.counter || !r.initial || !r.target) return false;
        const auto it = o.itemCounters.find(*r.counter);
        return it != o.itemCounters.end() && it->second.id == *r.counter && it->second.initial == *r.initial && it->second.target == *r.target;
    }
    if (r.type != IncrementExactCounter && (r.type != CompleteDistinctSet || !r.counter)) return true;
    if (!r.counter || !r.initial || !r.target) return false;
    const auto it = o.counters.find(*r.counter);
    return it != o.counters.end() && it->second.id == *r.counter && it->second.initial == *r.initial && it->second.target == *r.target;
}

// MissionObjectiveDefinition.HasCompleteServerContract (credit policy: the loader passes none, i.e. Personal)
bool completeServerContract(const Objective& o) {
    if (o.visible && (!o.nameText || !o.bodyText)) return false;
    if (!o.initialState) return false;
    for (const auto& c : o.counters)
        if (c.second.id != c.first || (o.visible && (c.first >= 3 || c.first < 0 || !o.counterText[c.first]))) return false;
    if (o.rule && !ruleCompatible(*o.rule, o)) return false;
    if (o.aggregation && o.history) return false;
    if (o.isAggregate()) {
        const Opt aggregateCounter = o.aggregation ? o.aggregation->counter : o.history->counter;
        if (o.rule || !o.executable.empty() || o.conversations || !o.itemCounters.empty() || !o.revealed.empty() || !o.activated.empty() ||
            o.counters.size() != (aggregateCounter ? 1u : 0u))
            return false;
    }
    if (o.recognizeExisting) {
        // HasExistingFactContract
        if (o.executable.size() != 1 || o.executable[0].conversations != 0) return false;
        const Opt to = o.executable[0].toState;
        if (to && *to != 2) return false;
        if (!o.executable[0].rule) return false;
        const Rule& r = *o.executable[0].rule;
        if (r.scope || r.detail) return false;
        const bool ok = ((r.kind == WaypointAcquired || r.kind == LogosAcquired) && (r.type == CompleteDistinctSet || r.type == CompleteExact)) ||
                        (r.kind == MissionCompleted && r.type == CompleteExact) || (r.kind == ItemAcquired && r.type == IncrementExactItemCounter);
        if (!ok) return false;
    }
    return true;
}

// The executable transitions' reveal / activate edges plus aggregate summaries (child -> summary): MissionContentValidator.BuildObjectiveGraph.
std::map<int64_t, std::vector<int64_t>> objectiveGraph(const std::map<int64_t, Objective>& objectives) {
    std::map<int64_t, std::vector<int64_t>> graph;
    for (const auto& o : objectives) {
        std::set<int64_t> edges;
        for (const Executable& e : o.second.executable) {
            edges.insert(e.revealed.begin(), e.revealed.end());
            edges.insert(e.activated.begin(), e.activated.end());
        }
        for (const auto& summary : objectives)
            if (summary.second.aggregation)
                for (int64_t child : summary.second.aggregation->children)
                    if (child == o.first) edges.insert(summary.first);
        graph[o.first].assign(edges.begin(), edges.end());
    }
    return graph;
}

bool detectCycle(int64_t node, const std::map<int64_t, std::vector<int64_t>>& graph, std::set<int64_t>& visiting, std::set<int64_t>& visited) {
    if (visited.count(node)) return false;
    if (!visiting.insert(node).second) return true;
    const auto it = graph.find(node);
    if (it != graph.end())
        for (int64_t edge : it->second)
            if (graph.count(edge) && detectCycle(edge, graph, visiting, visited)) return true;
    visiting.erase(node);
    visited.insert(node);
    return false;
}

struct Context {
    const MissionReferences& refs;
    std::vector<MissionDiagnostic>& out;
    const Definition* def = nullptr;
    void add(const std::string& code, const std::string& message, int64_t objective = -1, int64_t transition = -1, int64_t trigger = -1, int64_t action = -1,
             int64_t scenario = -1, int64_t step = -1) {
        MissionDiagnostic d;
        d.code = code;
        d.message = message;
        d.missionId = def->missionId;
        d.revision = def->revision;
        d.objectiveId = objective;
        d.transitionId = transition;
        d.triggerId = trigger;
        d.actionId = action;
        d.scenarioId = scenario;
        d.stepId = step;
        out.push_back(std::move(d));
    }
};

void validateTransitions(Context& c) {
    const Definition& d = *c.def;
    std::vector<const Transition*> ordered;
    for (const Transition& t : d.transitions) ordered.push_back(&t);
    std::sort(ordered.begin(), ordered.end(), [](const Transition* a, const Transition* b) {
        return a->objective != b->objective ? a->objective < b->objective : a->id < b->id;
    });
    for (const Transition* tp : ordered) {
        const Transition& t = *tp;
        for (const Trigger& g : t.triggers) {
            if (g.kind < 1 || g.kind > 5) {
                c.add("unsupported-trigger", "trigger kind " + std::to_string(g.kind) + " is unknown; use a supported discriminator.", t.objective, t.id, g.id);
                continue;
            }
            if (g.kind == 1 && (!g.npc || (!c.refs.npcPackageIds.count(*g.npc) && !d.objectNpcPackages.count(*g.npc)))) {
                const std::string id = optText(g.npc);
                c.add("missing-npc-package", "conversation trigger references missing npc_package.package_id " + id + "; restore npc_package.package_id " + id +
                                                 " or update mission_trigger.npc_package_id to a valid package.",
                      t.objective, t.id, g.id);
            }
            if (g.kind == 4 && (!g.area || !d.areas.count(*g.area)))
                c.add("missing-area", "area trigger references missing mission_area " + optText(g.area) + ".", t.objective, t.id, g.id);
            if (g.kind == 5 && (!g.duration || *g.duration == 0))
                c.add("invalid-delay", "timer trigger must declare a positive duration_seconds value.", t.objective, t.id, g.id);
            if (g.kind == 3 && (!g.relatedObjective || !d.objectives.count(*g.relatedObjective) || !validState(g.relatedState)))
                c.add("missing-related-objective",
                      "objective-state trigger references missing objective " + optText(g.relatedObjective) + " or an invalid related_state.", t.objective, t.id,
                      g.id);
        }
        std::vector<const Trigger*> progress;
        for (const Trigger& g : t.triggers)
            if (g.kind == 2) progress.push_back(&g);
        if (!progress.empty()) {
            Rule rule;
            std::map<int64_t, Counter> counters, itemCounters;
            std::string diagnostic;
            if (!tryBuildProgressRule(d.missionId, progress, rule, counters, itemCounters, diagnostic))
                c.add("invalid-progress-event", diagnostic, t.objective, t.id);
        }
        for (const Action& a : t.actions) {
            // MissionItemValidation.ActionMatches
            const char* expected = a.kind == 10 ? "issue-mission-item" : a.kind == 11 ? "consume-mission-item" : a.kind == 12 ? "remove-mission-items"
                                 : a.kind == 13 ? "fail-related-mission" : "";
            if (a.intentKind != expected)
                c.add("invalid-item-action", "Mission item action kind does not match its typed item intent.", t.objective, t.id, -1, a.id);
            if (a.kind < 1 || a.kind > 13) {
                c.add("unsupported-action", "action kind " + std::to_string(a.kind) + " is unknown; use a supported discriminator.", t.objective, t.id, -1, a.id);
                continue;
            }
            if ((a.kind == 1 || a.kind == 2 || a.kind == 3) && (!a.targetObjective || !d.objectives.count(*a.targetObjective)))
                c.add("missing-target", "action references missing objective " + optText(a.targetObjective) + ".", t.objective, t.id, -1, a.id);
            if (a.kind == 4 && (!a.reward || !d.rewards.count(*a.reward)))
                c.add("missing-reward", "grant reward action references missing reward " + optText(a.reward) + ".", t.objective, t.id, -1, a.id);
            if (a.kind == 6 && (!a.spawnGroup || !d.spawnGroups.count(*a.spawnGroup)))
                c.add("missing-spawn-group", "activate spawn group action references missing spawn group " + optText(a.spawnGroup) + ".", t.objective, t.id, -1,
                      a.id);
            if (a.kind == 5 && (!a.scenario || !d.scenarios.count(*a.scenario)))
                c.add("missing-scenario", "start scenario action references missing scenario " + optText(a.scenario) + ".", t.objective, t.id, -1, a.id);
            if (a.kind == 9 && (!a.npc || !a.flagId))
                c.add("missing-ambient-conversation-binding", "ambient conversation action is missing its NPC package or player flag.", t.objective, t.id, -1, a.id);
            if (a.kind == 7) {
                bool found = false;
                for (const auto& o : d.objectives)
                    for (const auto& ind : o.second.indicators) found = found || (a.indicator && ind.first == *a.indicator);
                if (!a.indicator || !found)
                    c.add("missing-indicator", "show indicator action references missing mission_indicator " + optText(a.indicator) + ".", t.objective, t.id, -1,
                          a.id);
            }
            if (a.kind == 8 && (!a.flagId || !a.flagValue))
                c.add("missing-player-flag-binding", "set player flag action is missing its player_flag_id or player_flag_value.", t.objective, t.id, -1, a.id);
            else if (a.kind == 8 && !(*a.flagId > 0 && *a.flagId < 0x80000000LL))
                c.add("reserved-player-flag-id", "Mission flags must use IDs 1..2147483647; higher IDs are reserved for named server state.", t.objective, t.id,
                      -1, a.id);
        }
    }
    for (const auto& o : d.objectives)
        for (const RuntimeDiagnostic& r : o.second.runtime) c.add(r.code, r.message, o.first, r.transitionId);
}

void validateReferences(Context& c, const std::map<int64_t, Definition>& all) {
    const Definition& d = *c.def;
    for (const Row& p : d.prerequisites) {
        const Opt required = p.o("required_mission_id");
        if (!required) continue;
        const auto it = all.find(*required);
        if (it != all.end() && it->second.revision != d.revision)
            c.add("cross-revision-reference", "prerequisite mission " + std::to_string(*required) + " resolves to content revision " + it->second.revision +
                                                  "; keep required chains on one revision.");
    }
    for (const auto& o : d.objectives) {
        if (!o.second.rule || o.second.rule->kind != MissionCompleted || o.second.rule->subjects.size() != 1) continue;
        const int64_t target = *o.second.rule->subjects.begin();
        const auto it = all.find(target);
        if (it != all.end() && it->second.revision != d.revision)
            c.add("cross-revision-reference", "mission completed progress rule references mission " + std::to_string(target) + " on revision " +
                                                  it->second.revision + "; keep authored mission references on one revision.",
                  o.first);
    }
}

void validateCounterTextBindings(Context& c) {
    for (const auto& o : c.def->objectives) {
        if (!o.second.visible) continue;
        for (const auto& counter : o.second.counters) {
            if (counter.first < 0 || counter.first >= 3) {
                c.add("invalid-counter-text-binding", "counter " + std::to_string(counter.first) + " has no authored client counter text slot.", o.first);
                continue;
            }
            if (!o.second.counterText[counter.first])
                c.add("invalid-counter-text-binding", "counter " + std::to_string(counter.first) + " is missing its authored client counter text binding.", o.first);
        }
    }
}

void validateRewardReferences(Context& c) {
    const Definition& d = *c.def;
    if (d.rewards.empty()) return;
    std::set<int64_t> referenced;
    for (const Transition& t : d.transitions)
        for (const Action& a : t.actions)
            if (a.kind == 4 && a.reward) referenced.insert(*a.reward);
    if (referenced.empty()) {
        c.add("missing-reward-reference", "mission authors rewards but no GrantReward action references a reward_id.");
        return;
    }
    if (referenced.size() > 1) {
        std::string list;
        for (int64_t r : referenced) list += (list.empty() ? "" : ", ") + std::to_string(r);
        c.add("ambiguous-reward-reference",
              "mission references multiple GrantReward reward_ids (" + list + "); the current runtime supports exactly one turn-in reward.");
    }
}

void validateObjectiveGraph(Context& c) {
    const Definition& d = *c.def;
    std::set<int64_t> revealTargets, activateTargets;
    for (const Transition& t : d.transitions) {
        if (!d.objectives.count(t.objective)) continue;
        for (const Action& a : t.actions) {
            if (a.kind == 1 && a.targetObjective) revealTargets.insert(*a.targetObjective);
            if (a.kind == 2 && a.targetObjective) activateTargets.insert(*a.targetObjective);
        }
    }
    bool anyInitial = false;
    for (const auto& o : d.objectives) anyInitial = anyInitial || o.second.initialState == Opt(1);
    if (!d.objectives.empty() && !anyInitial) c.add("invalid-objective-graph", "mission has no initially incomplete objective; author one entry point.");
    for (const auto& o : d.objectives)
        if (o.second.initialState == Opt(4) && activateTargets.count(o.first) && !revealTargets.count(o.first))
            c.add("invalid-objective-graph", "inactive objectives must be revealed before they are activated.", o.first);
    const auto graph = objectiveGraph(d.objectives);
    std::set<int64_t> visiting, visited;
    for (const auto& node : graph)
        if (detectCycle(node.first, graph, visiting, visited)) {
            c.add("transition-cycle", "reveal/activate actions create a cycle; break the successor loop.", node.first);
            break;
        }
}

// MissionContentValidator.ValidateRewardModules (development b22b4a4d): the crafting station's rules for putting modules into an
// item - each a module_class row, the item's class in modifiable_class, every module of that class set, no two of one variant - and
// the slots filled from module_1.
void validateRewardModules(Context& c, const Reward::Item& item, int64_t itemClassId) {
    const int64_t modules[4] = {item.module1, item.module2, item.module3, item.module4};
    int lastFull = -1;
    for (int slot = 0; slot < 4; ++slot)
        if (modules[slot] != 0) lastFull = slot;
    if (lastFull < 0) return;
    const std::string prefix = "reward item " + std::to_string(item.id) + ": ";
    for (int slot = 0; slot < lastFull; ++slot)
        if (modules[slot] == 0) {
            c.add("reward-module-slot-gap", prefix + "module slot " + std::to_string(slot + 1) + " is empty before a full one; fill the slots from module_1.");
            break;
        }
    const auto set = c.refs.modifiableClasses.find(itemClassId);
    if (set == c.refs.modifiableClasses.end() || set->second == 0) {
        c.add("reward-item-takes-no-modules", prefix + "item template " + std::to_string(item.templateId) + " (class " +
                                                  std::to_string(itemClassId) + ") takes no modules.");
        return;
    }
    std::set<int64_t> variants;
    for (int64_t moduleId : modules) {
        if (moduleId == 0) continue;
        const auto module = c.refs.moduleClasses.find(moduleId);
        if (module == c.refs.moduleClasses.end()) {
            c.add("missing-reward-module", prefix + "module " + std::to_string(moduleId) + " is not in module_class.");
            continue;
        }
        if (module->second.second != set->second)
            c.add("wrong-reward-module-kind", prefix + "module " + std::to_string(moduleId) + " is for class set " + std::to_string(module->second.second) +
                                                  ", not item template " + std::to_string(item.templateId) + "'s " + std::to_string(set->second) + ".");
        if (module->second.first != 0 && !variants.insert(module->second.first).second)
            c.add("duplicate-reward-module-variant", prefix + "module " + std::to_string(moduleId) + " is a second module of variant " +
                                                         std::to_string(module->second.first) + ".");
    }
}

void validateRewards(Context& c) {
    for (const auto& rp : c.def->rewards) {
        const Reward& r = rp.second;
        std::vector<Reward::Item> items = r.fixed;
        items.insert(items.end(), r.selectable.begin(), r.selectable.end());
        for (const Reward::Item& item : items) {
            const auto cls = c.refs.itemTemplateClasses.find(item.templateId);
            if (cls == c.refs.itemTemplateClasses.end()) {
                c.add("missing-item-template",
                      "reward item " + std::to_string(item.id) + " references missing item template " + std::to_string(item.templateId) + ".");
                continue;
            }
            if (!c.refs.entityClassIds.count(cls->second))
                c.add("missing-entity-class",
                      "item template " + std::to_string(item.templateId) + " resolves to missing entity class " + std::to_string(cls->second) + ".");
            if (item.quantity == 0) c.add("invalid-quantity", "reward item " + std::to_string(item.id) + " must have a positive quantity.");
            validateRewardModules(c, item, cls->second);
        }
        const bool selectable = !r.selectable.empty();
        if ((r.selection == 0 && selectable) || (r.selection > 0 && !selectable))
            c.add("invalid-reward-selection", "reward selection_count must match the presence of selectable reward items.");
    }
}

void validateProgressReferences(Context& c) {
    const Definition& d = *c.def;
    for (const auto& op : d.objectives) {
        if (!op.second.rule) continue;
        const Rule& r = *op.second.rule;
        const int64_t oid = op.first;
        for (int64_t subject : r.subjects) {
            switch (r.kind) {
                case CreatureKilled:
                    if (!c.refs.creatureClasses.count(subject))
                        c.add("missing-creature", "creature kill progress rule references missing creature " + std::to_string(subject) + ".", oid);
                    break;
                case CreatureFlagKilled:
                    if (!c.refs.creatureFlagIds.count(subject))
                        c.add("missing-creature-flag",
                              "creature flag kill progress rule names creature flag " + std::to_string(subject) +
                                  ", which no creature class carries (creature_class_flag).",
                              oid);
                    break;
                case CreatureClassKilled:
                    if (!c.refs.entityClassIds.count(subject))
                        c.add("missing-entity-class", "creature class kill progress rule references missing entity class " + std::to_string(subject) + ".", oid);
                    break;
                case ItemAcquired:
                case ItemConsumed:
                case InteractionUsed:
                case ObjectHit:
                    if (!c.refs.entityClassIds.count(subject))
                        c.add("missing-entity-class", "progress rule references missing entity class " + std::to_string(subject) + ".", oid);
                    break;
                case ItemEquipped:
                    if (r.sourceSpawnResolved == std::optional<bool>(true)) {
                        if (!c.refs.itemTemplateClasses.count(subject))
                            c.add("missing-item-template", "item equipped progress rule references missing item template " + std::to_string(subject) + ".", oid);
                    } else if (!c.refs.entityClassIds.count(subject)) {
                        c.add("missing-entity-class", "item equipped progress rule references missing entity class " + std::to_string(subject) + ".", oid);
                    }
                    break;
                case AbilityHit:
                    if (!c.refs.creatureClasses.count(r.detail.value_or(0)))
                        c.add("missing-creature", "ability hit progress rule references missing creature " + optText(r.detail) + ".", oid);
                    break;
                case ScenarioEvent: {
                    const auto sc = d.scenarios.find(r.detail.value_or(0));
                    bool found = false;
                    if (sc != d.scenarios.end())
                        for (const Step& s : sc->second.steps) found = found || s.row.o("scenario_event_id") == Opt(subject);
                    if (!found)
                        c.add("missing-scenario-step",
                              "scenario event progress rule references missing scenario " + optText(r.detail) + " scenario_event_id " + std::to_string(subject) + ".",
                              oid);
                    break;
                }
                default:
                    break;
            }
        }
    }
}

void validateAreas(Context& c) {
    for (const auto& ap : c.def->areas) {
        const Area& a = ap.second;
        if (!c.refs.mapContextIds.count(a.map))
            c.add("missing-map-context", "mission area " + std::to_string(a.id) + " references missing map_context " + std::to_string(a.map) + ".");
        if (a.shape == 1 && (!a.radius || *a.radius <= 0))
            c.add("invalid-radius", "mission area " + std::to_string(a.id) + " must have a positive radius.");
    }
    for (const auto& o : c.def->objectives)
        for (const auto& ind : o.second.indicators)
            if (ind.second <= 0) c.add("invalid-radius", "indicator " + std::to_string(ind.first) + " must have a positive radius.", o.first);
}

void validateSpawnGroups(Context& c) {
    for (const auto& gp : c.def->spawnGroups) {
        const SpawnGroup& g = gp.second;
        const std::string id = std::to_string(g.id);
        if (!c.refs.mapContextIds.count(g.map))
            c.add("missing-map-context", "spawn group " + id + " references missing map_context " + std::to_string(g.map) + ".");
        if (g.area && !c.def->areas.count(*g.area))
            c.add("missing-area", "spawn group " + id + " references missing mission_area " + std::to_string(*g.area) + ".");
        if (g.respawn && *g.respawn == 0) c.add("invalid-delay", "spawn group " + id + " must use a positive respawn_seconds value when present.");
        if (g.policy != 0 && g.policy != 1) c.add("invalid-spawn-policy", "spawn group " + id + " uses unsupported spawn_policy " + std::to_string(g.policy) + ".");
        if (g.policy == 1 && g.respawn) c.add("invalid-spawn-policy", "scenario-controlled spawn group " + id + " must not declare respawn_seconds.");
        for (const SpawnGroup::Spawn& s : g.spawns) {
            if (s.quantity == 0) c.add("invalid-quantity", "spawn " + std::to_string(s.id) + " must have a positive quantity.");
            const auto cls = c.refs.creatureClasses.find(s.creature);
            if (cls != c.refs.creatureClasses.end() && !c.refs.entityClassIds.count(cls->second))
                c.add("missing-entity-class",
                      "spawn creature " + std::to_string(s.creature) + " resolves to missing entity class " + std::to_string(cls->second) + ".");
        }
    }
}

void validateScenarioStep(Context& c, int64_t scenarioId, const Step& step) {
    const Definition& d = *c.def;
    const Row& r = step.row;
    const auto shape = [&](const std::string& message) { c.add("invalid-scenario-step-shape", message, -1, -1, -1, -1, scenarioId, step.id); };
    const auto diag = [&](const char* code, const std::string& message) { c.add(code, message, -1, -1, -1, -1, scenarioId, step.id); };
    const auto validDelay = [](int64_t ms) { return ms >= 1 && ms <= 86400000; };
    const Opt group = r.o("spawn_group_id");
    switch (step.kind) {
        case 1:
        case 2:
            if (!group) return shape("spawn group steps require spawn_group_id.");
            if (!d.spawnGroups.count(*group)) diag("missing-spawn-group", "scenario step references missing spawn group " + std::to_string(*group) + ".");
            return;
        case 22:
            if (!group) return shape("escort spawn group steps require spawn_group_id.");
            if (!d.spawnGroups.count(*group)) return diag("missing-spawn-group", "scenario step references missing spawn group " + std::to_string(*group) + ".");
            if (d.spawnGroups.at(*group).policy != 1)
                diag("invalid-spawn-policy", "escort step requires spawn group " + std::to_string(*group) + " to use spawn_policy ScenarioControlled.");
            return;
        case 20:
            if (r.s("dynamic_object_key").find_first_not_of(" \t\r\n") == std::string::npos || r.isNull("entity_class_id") || r.isNull("pos_x") ||
                r.isNull("pos_y") || r.isNull("pos_z") || r.isNull("orientation"))
                return shape("spawn dynamic object steps require dynamic_object_key, entity_class_id, pos_x, pos_y, pos_z, and orientation.");
            if (!c.refs.entityClassIds.count(r.i("entity_class_id")))
                diag("missing-entity-class", "dynamic object step references missing entity class " + std::to_string(r.i("entity_class_id")) + ".");
            return;
        case 21:
            if (r.s("dynamic_object_key").find_first_not_of(" \t\r\n") == std::string::npos) shape("despawn dynamic object steps require dynamic_object_key.");
            return;
        case 3:
        case 4: {
            if (!r.isNull("entity_class_id")) {
                if (!c.refs.entityClassIds.count(r.i("entity_class_id")))
                    diag("missing-entity-class", "interaction step references missing entity class " + std::to_string(r.i("entity_class_id")) + ".");
                return;
            }
            const Opt spawn = r.o("spawn_id");
            if (!group || !spawn) return shape("interaction steps require either entity_class_id or spawn_group_id plus spawn_id.");
            const auto g = d.spawnGroups.find(*group);
            if (g == d.spawnGroups.end()) return diag("missing-spawn-group", "interaction step references missing spawn group " + std::to_string(*group) + ".");
            bool found = false;
            for (const SpawnGroup::Spawn& s : g->second.spawns) found = found || s.id == *spawn;
            if (!found)
                diag("missing-spawn", "interaction step references missing spawn " + std::to_string(*spawn) + " in spawn group " + std::to_string(*group) + ".");
            return;
        }
        case 5:
        case 6:
        case 7:
        case 8: {
            const Opt target = r.o("target_objective_id");
            if (!target) return shape("objective steps require target_objective_id.");
            if (!d.objectives.count(*target)) diag("missing-target", "scenario step references missing objective " + std::to_string(*target) + ".");
            return;
        }
        case 9: {
            const Opt delay = r.o("delay_milliseconds");
            if (!delay) return shape("deadline start steps require delay_milliseconds.");
            if (!validDelay(*delay))
                diag("invalid-delay", "deadline delay_milliseconds " + std::to_string(*delay) + " must be between 1 and 86400000.");
            return;
        }
        case 10:
        case 23:
            return;
        case 11: {
            const Opt reward = r.o("reward_id");
            if (!reward) return shape("reward steps require reward_id.");
            const auto rw = d.rewards.find(*reward);
            if (rw == d.rewards.end()) return diag("missing-reward", "scenario step references missing reward " + std::to_string(*reward) + ".");
            if (rw->second.selection > 0 || !rw->second.selectable.empty())
                diag("invalid-scenario-reward-selection", "scenario step reward " + std::to_string(*reward) + " must not contain selectable alternatives.");
            return;
        }
        case 12: {
            const Opt skill = r.o("skill_id"), ability = r.o("ability_id"), level = r.o("skill_level"), slot = r.o("ability_slot");
            if (!skill || !ability || !level) return shape("skill grant steps require skill_id, ability_id, and skill_level.");
            if (*skill == 0) shape("skill_id must be greater than zero.");
            if (*ability == 0 || *ability > 2147483647LL) shape("ability_id must be between 1 and 2147483647.");
            if (*level < 1 || *level > 5) shape("skill_level must be between 1 and 5.");
            if (slot && *slot > 24) shape("ability_slot must be between 0 and 24 when it is authored.");
            return;
        }
        case 13: {
            const Opt tutorial = r.o("tutorial_id"), audio = r.o("audio_set_id");
            if (!tutorial) return shape("tutorial steps require tutorial_id.");
            if (!validTutorial(*tutorial)) diag("invalid-tutorial", "tutorial step references unknown tutorial_id " + std::to_string(*tutorial) + ".");
            if (audio && *audio == 0) shape("audio_set_id must be greater than zero when it is authored.");
            return;
        }
        case 14: {
            const Opt target = r.o("target_scenario_id"), delay = r.o("delay_milliseconds");
            if (!target || !delay) return shape("scheduled scenario steps require target_scenario_id and delay_milliseconds.");
            if (!d.scenarios.count(*target)) diag("missing-scenario", "scheduled scenario step references missing scenario " + std::to_string(*target) + ".");
            if (!validDelay(*delay)) diag("invalid-delay", "scenario delay_milliseconds " + std::to_string(*delay) + " must be between 1 and 86400000.");
            return;
        }
        case 15: {
            const Opt target = r.o("target_scenario_id");
            const bool hasKey = r.s("attempt_key").find_first_not_of(" \t\r\n") != std::string::npos;
            if (target.has_value() == hasKey) return shape("reset attempt steps require exactly one of target_scenario_id or attempt_key.");
            if (target && !d.scenarios.count(*target))
                diag("missing-scenario", "reset attempt step references missing scenario " + std::to_string(*target) + ".");
            return;
        }
        case 16: {
            const Opt event = r.o("scenario_event_id");
            if (!event || *event == 0) shape("scenario event steps require scenario_event_id.");
            return;
        }
        case 17:
            if (r.isNull("map_context_id") || r.isNull("pos_x") || r.isNull("pos_y") || r.isNull("pos_z") || r.isNull("orientation"))
                return shape("transfer steps require map_context_id, pos_x, pos_y, pos_z, and orientation.");
            if (!c.refs.mapContextIds.count(r.i("map_context_id")))
                diag("missing-map-context", "transfer step references missing map_context " + std::to_string(r.i("map_context_id")) + ".");
            return;
        case 18: {
            const Opt key = r.o("qualification_key"), value = r.o("qualification_value");
            if (!key || !value) return shape("qualification steps require qualification_key and qualification_value.");
            if (*key != 1) return diag("invalid-qualification", "qualification step requires a defined CharacterQualificationKey.");
            if (*value != 0 && *value != 1) diag("invalid-qualification", "qualification step requires qualification_value 0 (remove) or 1 (set).");
            return;
        }
        case 19:
            if (r.isNull("account_skip_entitlement")) shape("account skip entitlement steps require account_skip_entitlement.");
            return;
        default:
            diag("unsupported-scenario-step", "scenario step kind " + std::to_string(step.kind) + " is unknown; use an approved discriminator.");
            return;
    }
}

void validateScenarios(Context& c) {
    for (const auto& sp : c.def->scenarios) {
        if (sp.second.steps.empty()) {
            if (c.def->typedScenarios.count(sp.first)) continue;
            c.add("missing-scenario", "scenario " + std::to_string(sp.first) + " has no steps; author at least one step.");
            continue;
        }
        for (const Step& s : sp.second.steps) validateScenarioStep(c, sp.first, s);
    }
}

void validateMission(Context& c, const std::map<int64_t, Definition>& all) {
    const Definition& d = *c.def;
    // MissionChannelValidation.Errors
    const auto validChannel = [](int64_t v) { return v >= 1 && v <= 3; };
    if (!validChannel(d.acceptance) || !validChannel(d.completion)) c.add("invalid-mission-channel", "mission acceptance/completion channel is invalid");
    const bool hasGiver = !d.row.isNull("giver_id"), hasReceiver = !d.row.isNull("receiver_id");
    if (((d.acceptance & 1) && !hasGiver) || ((d.completion & 1) && !hasReceiver))
        c.add("invalid-mission-channel", "an NPC channel requires its corresponding giver or receiver ID");
    bool sourcesBad = false;
    std::set<std::pair<int64_t, std::string>> seen;
    for (const Definition::RadioSource& s : d.radioSources) {
        if (!s.valid || !(s.kind == 0 || s.kind == 1) || s.key.find_first_not_of(" \t\r\n") == std::string::npos || s.key.size() > 96 || s.map == Opt(0))
            sourcesBad = true;
        if (!seen.insert({s.kind, s.key}).second) sourcesBad = true;
    }
    if (sourcesBad) c.add("invalid-mission-channel", "radio sources are invalid or duplicated");
    if (!d.radioSources.empty() && !(d.acceptance & 2)) c.add("invalid-mission-channel", "radio sources require a radio acceptance channel");
    if ((d.acceptance & 2) && d.radioSources.empty()) c.add("invalid-mission-channel", "radio acceptance requires an authored source");
    for (const Definition::RadioSource& s : d.radioSources)
        if (s.map && !c.refs.mapContextIds.count(*s.map))
            c.add("missing-radio-source-map", "Radio source " + s.key + " references unavailable map context " + std::to_string(*s.map) + ".");
    // MissionRepeatPolicy.ValidationError
    if (d.hasRepeat) {
        bool ok = false;
        if ((d.repeatKind == 0 || d.repeatKind == 1) && !d.cooldown && !d.resetSecond) ok = true;
        if (d.repeatKind == 2 && d.cooldown && *d.cooldown > 0 && !d.resetSecond) ok = true;
        if (d.repeatKind == 3 && d.resetSecond && *d.resetSecond < 86400 && !d.cooldown) ok = true;
        if (!ok)
            c.add("invalid-repeat-policy",
                  "Repeat policy requires Once/Immediate with no timing, a positive Cooldown, or a Daily UTC reset second (0..86399).");
    }
    if (d.row.i("client_name_text_id") == 0) c.add("missing-client-text", "mission client_name_text_id is missing; author a non-zero binding.");
    if (d.objectives.empty()) c.add("missing-objective-id", "mission has no authored objectives; add at least one objective row.");
    for (const auto& o : d.objectives) {
        if ((o.second.isAggregate() || o.second.recognizeExisting) && !completeServerContract(o.second))
            c.add("invalid-objective-progress-metadata",
                  "progress metadata requires a standalone aggregate or an eligible waypoint, Logos or mission-history rule.", o.first);
        if (o.second.visible && (!o.second.nameText || !o.second.bodyText))
            c.add("missing-client-text", "objective text bindings are incomplete; populate client_name_text_id and client_body_text_id.", o.first);
    }
    validateTransitions(c);
    validateReferences(c, all);
    validateCounterTextBindings(c);
    validateRewardReferences(c);
    validateObjectiveGraph(c);
    validateRewards(c);
    validateAreas(c);
    validateSpawnGroups(c);
    validateScenarios(c);
    validateProgressReferences(c);
}

// Why System.Text.Json (MissionContentCodec.Options: camelCase, unmapped members disallowed, string enums; .NET's polymorphic
// metadata first) cannot read an item intent as a CharacterIntent (Rasa.Missions/Scenes/SceneContracts.cs); "" when it can. The
// loader's exception stops the server.
std::string intentReadError(const JsonValue& v) {
    enum T { Str, UInt, Byte, OptUInt, OptByte, Bool, State, Deadline, Scope };
    struct Member { const char* name; T type; };
    static const std::map<std::string, std::vector<Member>> types = {
        {"reward", {{"missionId", UInt}, {"rewardId", UInt}}},
        {"ability", {{"skillId", UInt}, {"abilityId", UInt}, {"level", Byte}, {"slot", OptByte}}},
        {"qualification", {{"qualification", Byte}, {"present", Bool}}},
        {"character-flag", {{"flagId", UInt}, {"value", OptUInt}}},
        {"entitlement", {{"enabled", Bool}}},
        {"objective", {{"missionId", UInt}, {"objectiveId", UInt}, {"state", State}}},
        {"deadline", {{"missionId", UInt}, {"kind", Deadline}, {"milliseconds", UInt}}},
        {"issue-mission-item", {{"missionId", UInt}, {"itemKey", Str}, {"itemTemplateId", UInt}, {"quantity", UInt}}},
        {"consume-mission-item", {{"missionId", UInt}, {"itemKey", Str}, {"quantity", UInt}, {"scope", Scope}}},
        {"remove-mission-items", {{"missionId", UInt}, {"itemKey", Str}}},
        {"offer-radio-mission", {{"missionId", UInt}, {"forceDialog", Bool}, {"ifEligible", Bool}}},
        {"fail-related-mission", {{"missionId", UInt}}}};
    if (!v.isObject()) return "not a JSON object";
    if (v.members.empty() || v.members.front().first != "$kind") return "the $kind discriminator must be the first property";
    const JsonValue& kind = v.members.front().second;
    const auto type = kind.isString() ? types.find(kind.text) : types.end();
    if (type == types.end()) return "unknown $kind " + (kind.isString() ? kind.text : std::string("(not a string)"));
    const auto enumOk = [](const JsonValue& x, std::initializer_list<const char*> names, int64_t count) {
        int64_t n = 0;
        if (x.integer(n)) return n >= 0 && n < count;
        if (!x.isString()) return false;
        for (const char* name : names) {
            std::string a = x.text, b = name;
            for (char& ch : a) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            for (char& ch : b) ch = char(std::tolower(static_cast<unsigned char>(ch)));
            if (a == b) return true;
        }
        return false;
    };
    for (size_t k = 1; k < v.members.size(); ++k) {
        const std::string& name = v.members[k].first;
        const JsonValue& x = v.members[k].second;
        if (name == "operationKey") {
            if (!x.isString() && !x.isNull()) return "operationKey must be a string";
            continue;
        }
        const Member* m = nullptr;
        for (const Member& candidate : type->second)
            if (name == candidate.name) m = &candidate;
        if (!m) return "the JSON property '" + name + "' could not be mapped to any member of the " + kind.text + " intent";
        int64_t n = 0;
        bool ok = true;
        switch (m->type) {
            case Str: ok = x.isString() || x.isNull(); break;
            case UInt: ok = x.integer(n) && n >= 0 && n <= 0xFFFFFFFFLL; break;
            case Byte: ok = x.integer(n) && n >= 0 && n <= 255; break;
            case OptUInt: ok = x.isNull() || (x.integer(n) && n >= 0 && n <= 0xFFFFFFFFLL); break;
            case OptByte: ok = x.isNull() || (x.integer(n) && n >= 0 && n <= 255); break;
            case Bool: ok = x.type == JsonValue::Type::Bool; break;
            case State: ok = enumOk(x, {"NotAssigned", "Incomplete", "Completed", "Failed", "Inactive"}, 5); break;
            case Deadline: ok = enumOk(x, {"Start", "Satisfy", "Cancel"}, 3); break;
            case Scope: ok = enumOk(x, {"AssignmentIssued", "CharacterOwned"}, 2); break;
        }
        if (!ok) return "the JSON value of '" + name + "' cannot be read as its member's type";
    }
    return "";
}

// ---- MissionContentCatalog.Load: the scene bindings and experiences ---------------------------------------------------------
// MissionSceneValidation (Rasa.Game/Missions/Content), MissionRequirementEvaluator.RequiredFacts, MissionItemValidation,
// MissionDialogueValidation, MissionConversationProjection.ForObject, ActorGameplayPolicy.ValidationError, the loot planner's
// reference checks and SceneRuntime.Evaluate as the catalog runs it on each sequence (script data.sequence). Every failure here is
// an exception: the server does not start. The documents are read as MissionContentCodec writes them (camelCase, string enums);
// radio sources with the default options (PascalCase).

const int64_t kIntMax = 2147483647;

bool blank(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}
const JsonValue* mem(const JsonValue* v, const char* name) { return v && v->isObject() ? v->member(name) : nullptr; }
bool present(const JsonValue* v) { return v && !v->isNull(); }
int64_t num(const JsonValue* v, int64_t fallback = 0) {
    int64_t n = 0;
    return v && v->integer(n) ? n : fallback;
}
Opt optNum(const JsonValue* v) {
    int64_t n = 0;
    if (v && v->integer(n)) return n;
    return std::nullopt;
}
std::string str(const JsonValue* v) { return v && v->isString() ? v->text : std::string(); }
// A string enum (JsonStringEnumConverter: names case-insensitive, numbers allowed): its value, `fallback` when absent, -1 when
// the name is unknown.
int64_t enumValue(const JsonValue* v, std::initializer_list<const char*> names, int64_t fallback) {
    if (!present(v)) return fallback;
    int64_t n = 0;
    if (v->integer(n)) return n;
    if (!v->isString()) return -1;
    int64_t index = 0;
    for (const char* name : names) {
        std::string a = v->text, b = name;
        for (char& ch : a) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (char& ch : b) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        if (a == b) return index;
        ++index;
    }
    return -1;
}
bool defined(int64_t value, int64_t count) { return value >= 0 && value < count; }
std::string join(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts) out += (out.empty() ? "" : separator) + part;
    return out;
}
// The members of an object (empty for anything else).
const std::vector<std::pair<std::string, JsonValue>>& entries(const JsonValue* v) {
    static const std::vector<std::pair<std::string, JsonValue>> none;
    return v && v->isObject() ? v->members : none;
}
const std::vector<JsonValue>& elements(const JsonValue* v) {
    static const std::vector<JsonValue> none;
    return v && v->isArray() ? v->items : none;
}
Opt keyNumber(const std::string& key) {
    if (key.empty() || !std::all_of(key.begin(), key.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; })) return std::nullopt;
    return std::strtoll(key.c_str(), nullptr, 10);
}

// MissionRequirementEvaluator.RequiredFacts (every handler's facts are in MissionRequirementFactsAdapter.Supported, so a requirement
// passes the Supported check exactly when this finds nothing); "" when well formed.
std::string requirementError(const JsonValue* r, bool pascal, int depth = 0) {
    if (depth > 32) return "Mission requirement nesting exceeds 32 levels.";
    if (!present(r)) return "";
    const auto m = [&](const char* camel) {
        std::string name = camel;
        if (pascal) name[0] = char(std::toupper(static_cast<unsigned char>(name[0])));
        return mem(r, name.c_str());
    };
    static const std::set<std::string> handlers = {"example.even-level", "account.starting-experience-entitlement", "character.starting-experience-completed",
                                                   "character.starting-experience-active", "character.soldier-family", "character.specialist-family"};
    const std::string malformed = "Malformed or unsupported mission requirement.";
    const std::string kind = str(mem(r, "$kind"));
    if (kind == "all" || kind == "any") {
        const JsonValue* items = m("items");
        if (!items || !items->isArray()) return malformed;
        for (const JsonValue& item : items->items)
            if (item.isNull()) return malformed;
        for (const JsonValue& item : items->items)
            if (std::string e = requirementError(&item, pascal, depth + 1); !e.empty()) return e;
        return "";
    }
    if (kind == "not") {
        const JsonValue* item = m("item");
        return present(item) ? requirementError(item, pascal, depth + 1) : malformed;
    }
    if (kind == "custom") {
        const std::string key = str(m("key"));
        return !blank(key) && handlers.count(key) ? std::string() : "Missing pure requirement handler " + key + ".";
    }
    if (kind == "assignment-item") {
        const std::string itemKey = str(m("itemKey"));
        const JsonValue* source = m("sourceOfferMissionId");
        return num(m("missionId")) > 0 && num(m("minimumQuantity"), 1) > 0 && !blank(itemKey) && itemKey.size() <= 64 &&
                       (!present(source) || num(source) != 0)
                   ? std::string()
                   : malformed;
    }
    if (kind == "level" || kind == "mission" || kind == "flag") return "";
    if (kind == "map") {
        const JsonValue* maps = m("maps");
        if (!maps || !maps->isArray() || maps->items.empty()) return malformed;
        for (const JsonValue& id : maps->items)
            if (num(&id) == 0) return malformed;
        return "";
    }
    return malformed;
}

// MissionItemValidation.BindingError
std::string bindingError(const JsonValue& b) {
    const std::string invalid = "Invalid mission item binding, scope, quantity or terminal disposition.";
    if (!b.isObject()) return invalid;
    const std::string key = str(mem(&b, "itemKey"));
    const int64_t scope = enumValue(mem(&b, "scope"), {"AssignmentIssued", "CharacterOwned"}, -1);
    const auto disposition = [&](const char* name) { return enumValue(mem(&b, name), {"Remove", "Retain"}, -1); };
    const int64_t completion = disposition("completion"), failure = disposition("failure"), abandonment = disposition("abandonment");
    const int64_t maximum = num(mem(&b, "maximumQuantity"));
    const JsonValue* drop = mem(&b, "drop");
    if (!present(mem(&b, "itemKey")) || blank(key) || key.size() > 64 || num(mem(&b, "itemTemplateId")) == 0 || maximum == 0 ||
        num(mem(&b, "turnInQuantity")) > maximum || (present(drop) && num(mem(drop, "quantity")) > maximum) || !defined(scope, 2) ||
        !defined(completion, 2) || !defined(failure, 2) || !defined(abandonment, 2))
        return invalid;
    if (scope == 1 && (completion != 1 || failure != 1 || abandonment != 1)) return "Character-owned costs cannot have automatic cleanup.";
    if (present(drop) && scope != 1) return "Corpse drops require CharacterOwned scope; assignment-issued items use mission item intents.";
    return "";
}

// ActorGameplayPolicy.ValidationError
bool isTag(const JsonValue* v) {
    if (!v || !v->isString() || v->text.empty()) return false;
    return std::none_of(v->text.begin(), v->text.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
}
std::string policyError(const JsonValue& p) {
    const JsonValue* radiusValue = mem(&p, "defenseRadius");
    const double radius = radiusValue ? radiusValue->number() : 0.0;
    if (radius < 0) return "defense radius must be finite and nonnegative";
    const JsonValue* tags = mem(&p, "tags");
    if (tags) {
        if (!tags->isArray()) return "tags must be nonempty, unique tokens without whitespace";
        std::set<std::string> seen;
        for (const JsonValue& tag : tags->items)
            if (!isTag(&tag) || !seen.insert(tag.text).second) return "tags must be nonempty, unique tokens without whitespace";
    }
    const JsonValue* target = mem(&p, "defenseTargetTag");
    if ((present(target) && !isTag(target)) || (radius > 0 && !present(target))) return "defense requires a valid target tag";
    return "";
}
// The constructors that throw while the document is read (AuthoredLootProfile, MissionCreditPolicy, MissionItemDropDefinition).
std::string lootError(const JsonValue* loot) {
    if (!present(loot)) return "";
    const JsonValue* drops = mem(loot, "drops");
    if (!drops || !drops->isArray()) return "Invalid authored loot distribution.";
    for (const JsonValue& drop : drops->items) {
        const int64_t chance = num(mem(&drop, "chancePercent")), minimum = num(mem(&drop, "minimum")), maximum = num(mem(&drop, "maximum"));
        if (!drop.isObject() || num(mem(&drop, "templateId")) == 0 || chance < 0 || chance > 100 || minimum < 1 || maximum < minimum || maximum == kIntMax)
            return "Invalid authored loot distribution.";
    }
    return "";
}
std::string documentReadError(const JsonValue& scene) {
    for (const auto& credit : entries(mem(&scene, "credit"))) {
        const int64_t mode = enumValue(mem(&credit.second, "mode"), {"Personal", "NearbyParty", "EncounterParticipants"}, 0);
        const JsonValue* r = mem(&credit.second, "radius");
        const double radius = r ? r->number() : 0.0;
        if (!defined(mode, 3) || radius < 0 || (mode != 0 && radius == 0)) return "Group credit needs an explicit positive finite radius.";
    }
    for (const JsonValue& item : elements(mem(&scene, "items"))) {
        const JsonValue* drop = mem(&item, "drop");
        if (!present(drop)) continue;
        const JsonValue* creatures = mem(drop, "creatureIds");
        std::set<int64_t> distinct;
        bool bad = !creatures || !creatures->isArray() || creatures->items.empty();
        for (const JsonValue& c : elements(creatures)) bad = bad || num(&c) == 0 || !distinct.insert(num(&c)).second;
        const int64_t chance = num(mem(drop, "chancePercent"));
        if (bad || num(mem(drop, "mapContextId")) == 0 || chance < 0 || chance > 100 || num(mem(drop, "quantity")) == 0)
            return "Invalid mission item drop source, map, chance or quantity.";
    }
    for (const auto& actor : entries(mem(&scene, "actors")))
        if (std::string e = lootError(mem(mem(&actor.second, "gameplayPolicy"), "loot")); !e.empty()) return e;
    return "";
}

// (below: the deserialiser's reading of a scene / experience document)
std::string sceneReadError(const JsonValue& document, bool experience);

// The scene scripts Rasa.Missions registers ([MissionScript] classes, SceneScriptRegistry.Discover).
bool scriptRegistered(const std::string& key, int64_t version) {
    static const std::set<std::string> scripts = {
        "data.sequence", "bootcamp.experience", "bootcamp.initiation", "bootcamp.gearing-up", "bootcamp.capture-the-flag", "bootcamp.reinforcements",
        "bootcamp.bomb-retry", "example.escort", "wilderness.solis-escort", "wilderness.alia-branch", "wilderness.corman-finale",
        "wilderness.daghdas-rangers", "wilderness.daghdas-herbs", "wilderness.corman-first-batch", "wilderness.corman-replacement",
        "wilderness.eloh-pinhole-survey", "wilderness.escape-velocity", "wilderness.walking-wounded", "wilderness.ranja-egg-clusters",
        "wilderness.supply-delivery", "wilderness.smuggler-deliveries.v1"};
    return version == 1 && scripts.count(key) != 0;
}

const std::set<int64_t>& useObjectStates() {
    static const std::set<int64_t> states = {
        1,   2,   3,   4,   5,   15,  31,  41,  42,  44,  46,  48,  50,  51,  53,  55,  56,  59,  61,  74,  81,  82,  86,  91,  92,  96,  97,  98,  99,  100,
        101, 103, 104, 105, 110, 113, 114, 115, 119, 120, 121, 127, 138, 145, 146, 164, 170, 171, 172, 176, 177, 179, 180, 181, 182, 183, 185, 186, 187, 188,
        189, 190, 191, 192, 193, 194, 196, 197, 198, 199, 200, 201, 202, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
        10000001, 10000002, 10000003, 10000004, 10000005, 10000006, 10000007, 10000008, 10000010, 10000011, 10000012, 10000013, 10000014, 10000015};
    return states;
}

// SceneActorKind: PublicSpawn 0, Creature 1, Object 2, PracticeTarget 3.
int64_t actorKind(const JsonValue* actor) { return enumValue(mem(actor, "kind"), {"PublicSpawn", "Creature", "Object", "PracticeTarget"}, 0); }
bool isItemIntentKind(const std::string& kind) { return kind == "issue-mission-item" || kind == "consume-mission-item" || kind == "remove-mission-items"; }
bool finitePosition(const JsonValue* p) { return p && p->isObject(); }      // JSON numbers are finite

// SceneRuntime.Evaluate on one sequence with DataSequenceScript (the decision = the sequence's intents, signals and timers).
std::string evaluateSequence(const JsonValue& scene, const JsonValue& sequence) {
    const JsonValue* actors = mem(&scene, "actors");
    const JsonValue* routes = mem(&scene, "routes");
    const auto& world = elements(mem(&sequence, "world"));
    const auto& character = elements(mem(&sequence, "character"));
    if (world.size() + character.size() + elements(mem(&sequence, "signals")).size() > 64) return "Scene decision exceeds the 64-operation transition bound.";
    std::set<std::string> keys;
    for (const auto* list : {&world, &character})
        for (const JsonValue& intent : *list) {
            const JsonValue* key = mem(&intent, "operationKey");
            const std::string k = str(key);
            if (!present(key) || blank(k) || k.size() > 96 || !keys.insert(k).second) return "Scene operation key is invalid or duplicated: " + k + ".";
        }
    for (const JsonValue& intent : world) {
        const std::string kind = str(mem(&intent, "$kind")), op = str(mem(&intent, "operationKey"));
        const JsonValue* roleValue = (kind == "presentation" || kind == "transfer") ? nullptr : mem(&intent, "role");
        const std::string role = str(roleValue);
        const JsonValue* actor = present(roleValue) ? mem(actors, role.c_str()) : nullptr;
        if (present(roleValue) && !actor) return "Scene operation " + op + " references missing actor role " + role + ".";
        if (kind == "object-state") {
            const int64_t state = num(mem(&intent, "state")), windup = num(mem(&intent, "windupMilliseconds"));
            const int64_t k = actorKind(actor);
            if (blank(role) || !actor || (k != 2 && k != 3) || state == 0 || state > kIntMax || windup > kIntMax)
                return "Scene operation " + op + " has an invalid object-state transition.";
        }
        if (kind == "route") {
            const JsonValue* route = mem(routes, str(mem(&intent, "route")).c_str());
            const size_t points = elements(mem(route, "points")).size();
            const int64_t start = num(mem(&intent, "startWaypoint"));
            const int64_t k = actorKind(actor);
            if (!route || points == 0 || start < 0 || start >= int64_t(points) || !actor || k == 2 || k == 3)
                return "Scene operation " + op + " references an invalid route or actor.";
        }
        if (kind == "quiet-spawn-pools" || kind == "wake-spawn-pools") {
            // SceneRuntime.Evaluate (development b22b4a4d): a role and at least one spawn pool, none of them 0.
            const std::vector<JsonValue>& pools = elements(mem(&intent, "spawnPools"));
            bool bad = !present(roleValue) || pools.empty();
            for (const JsonValue& pool : pools) bad = bad || num(&pool) == 0;
            if (bad)
                return "Scene operation " + op + (kind == "quiet-spawn-pools" ? " quiets" : " wakes") +
                       " no spawn pools, or has no actor holding them.";
        }
        if (kind == "attack") {
            const JsonValue* targetValue = mem(&intent, "targetRole");
            const int64_t k = actorKind(actor);
            bool bad = !present(roleValue) || !actor || (k != 0 && k != 1);
            if (!bad && present(targetValue)) {
                const JsonValue* target = mem(actors, str(targetValue).c_str());
                const int64_t tk = actorKind(target);
                bad = !target || (tk != 0 && tk != 1) || str(targetValue) == role;
            }
            if (bad) return "Scene operation " + op + " references an invalid combat actor.";
        }
    }
    for (const JsonValue& intent : character) {
        const std::string kind = str(mem(&intent, "$kind"));
        if (kind == "character-flag" && num(mem(&intent, "flagId")) == 0) return "Character flag IDs must be nonzero.";
    }
    for (const JsonValue& intent : character) {
        const std::string kind = str(mem(&intent, "$kind"));
        const int64_t mission = num(mem(&intent, "missionId"));
        if (kind == "offer-radio-mission" && (mission == 0 || mission > kIntMax)) return "Radio offer intents require a native mission ID.";
    }
    std::set<std::string> timers;
    for (const JsonValue& timer : elements(mem(&sequence, "timers"))) {
        const std::string name = str(mem(&timer, "name"));
        if (!present(mem(&timer, "name")) || blank(name) || name.size() > 64 || !timers.insert(name).second)
            return "Scene timer is invalid or duplicated: " + name + ".";
    }
    return "";
}

// MissionSceneValidation.Validate; "" when the document passes.
std::string validateScene(int64_t missionId, const JsonValue& scene, const std::set<int64_t>& objectiveIds) {
    const std::string mission = "Mission " + std::to_string(missionId);
    const auto ids = [&](const JsonValue* list, bool& distinctAndLocal) {
        std::set<int64_t> seen;
        for (const JsonValue& v : elements(list)) {
            const int64_t id = num(&v);
            if (!seen.insert(id).second || !objectiveIds.count(id)) distinctAndLocal = false;
        }
    };
    // ValidateProgressMetadata
    bool metadata = true;
    if (present(mem(&scene, "hiddenObjectiveIds"))) ids(mem(&scene, "hiddenObjectiveIds"), metadata);
    if (present(mem(&scene, "existingFactObjectiveIds"))) ids(mem(&scene, "existingFactObjectiveIds"), metadata);
    for (const auto& entry : entries(mem(&scene, "objectiveAggregations"))) {
        const Opt key = keyNumber(entry.first);
        if (entry.second.isNull() || !key || !objectiveIds.count(*key)) metadata = false;
        for (const JsonValue& child : elements(mem(&entry.second, "childObjectiveIds")))
            if (key && (num(&child) == *key || !objectiveIds.count(num(&child)))) metadata = false;
    }
    for (const auto& entry : entries(mem(&scene, "objectiveHistoryAggregations"))) {
        const Opt key = keyNumber(entry.first);
        if (entry.second.isNull() || !key || !objectiveIds.count(*key)) metadata = false;
        for (const JsonValue& group : elements(mem(&entry.second, "groups")))
            for (const JsonValue& id : elements(&group))
                if (num(&id) == missionId) metadata = false;
    }
    if (!metadata) return mission + ": progress metadata must name distinct local objectives.";

    const JsonValue* sequences = mem(&scene, "sequences");
    std::vector<const JsonValue*> characterIntents;
    for (const auto& seq : entries(sequences))
        for (const JsonValue& intent : elements(mem(&seq.second, "character"))) characterIntents.push_back(&intent);
    for (const JsonValue* intent : characterIntents)
        if (str(mem(intent, "$kind")) == "fail-related-mission") return "Related mission failure belongs to the originating native transition transaction.";
    const JsonValue* items = mem(&scene, "items");
    if (missionId == 0) {
        bool itemIntent = present(items) || present(mem(&scene, "acceptanceItems"));
        for (const JsonValue* intent : characterIntents) itemIntent = itemIntent || isItemIntentKind(str(mem(intent, "$kind")));
        if (itemIntent) return "Assignment item operations require a mission-owned scene, not an experience.";
    }
    if (present(items)) {
        std::set<std::string> keys;
        bool bad = false;
        for (const JsonValue& item : elements(items)) bad = bad || !bindingError(item).empty() || !keys.insert(str(mem(&item, "itemKey"))).second;
        if (bad) return mission + ": invalid or duplicate item bindings.";
    }
    const JsonValue* dialogue = mem(&scene, "dialogue");
    if (present(dialogue)) {
        bool bad = missionId == 0;
        for (const JsonValue& topic : elements(dialogue)) bad = bad || topic.isNull() || !objectiveIds.count(num(mem(&topic, "objectiveId")));
        if (bad) return mission + ": dialogue must name this mission's objectives.";
    }
    const JsonValue* titles = mem(&scene, "titles");
    if (present(titles)) {
        bool bad = missionId == 0;
        for (const auto& title : entries(titles)) {
            const Opt objective = keyNumber(title.first);
            bad = bad || num(&title.second) == 0 || !objective || !objectiveIds.count(*objective);
        }
        if (bad) return mission + ": a title must name a title id and one of this mission's objectives.";
    }
    const JsonValue* category = mem(&scene, "category");
    if ((present(category) && num(category) == 0) || (present(category) && missionId == 0)) return mission + ": a category must be a mission's, and not zero.";
    const JsonValue* audio = mem(&scene, "audio");
    if (present(audio)) {
        const JsonValue* events = mem(audio, "events");
        const JsonValue* announcements = mem(audio, "announcements");
        const JsonValue* offer = mem(audio, "offerAudioSetId");
        bool bad = (events && events->isNull()) || (announcements && announcements->isNull()) ||
                   (present(offer) && (num(offer) == 0 || num(offer) > kIntMax));
        for (const auto& e : entries(events)) {
            JsonValue name;
            name.type = JsonValue::Type::String;
            name.text = e.first;
            const Opt numeric = keyNumber(e.first);
            const int64_t event = numeric ? *numeric : enumValue(&name, {"Accepted", "Completed"}, -1);
            bad = bad || !defined(event, 2) || num(&e.second) == 0 || num(&e.second) > kIntMax;
        }
        for (const auto& a : entries(announcements)) bad = bad || keyNumber(a.first).value_or(0) == 0 || num(&a.second) == 0 || num(&a.second) > kIntMax;
        if (bad) return mission + ": invalid audio cue binding.";
    }
    // MissionRequirementService.Validate(missionId, objectives, requirement, turnInRequirement, objectiveRequirements)
    const JsonValue* objectiveRequirements = mem(&scene, "objectiveRequirements");
    if (objectiveRequirements && objectiveRequirements->isNull()) return mission + " has no objective requirement bindings.";
    for (const auto& r : entries(objectiveRequirements)) {
        const Opt objective = keyNumber(r.first);
        if (!objective || !objectiveIds.count(*objective)) return mission + " binds a requirement to missing objective " + r.first + ".";
    }
    for (const auto& r : entries(objectiveRequirements))
        if (std::string e = requirementError(&r.second, false); !e.empty()) return e;
    for (const char* name : {"requirement", "turnInRequirement"})
        if (std::string e = requirementError(mem(&scene, name), false); !e.empty()) return e;
    const JsonValue* script = mem(&scene, "script");
    const int64_t stateVersion = num(mem(&scene, "stateVersion"), 1);
    if (present(script) && !scriptRegistered(str(script), stateVersion))
        return mission + ": migrated script " + str(script) + "/" + std::to_string(stateVersion) + " is unavailable in this server.";

    const JsonValue* actors = mem(&scene, "actors");
    for (const auto& entry : entries(actors)) {
        const JsonValue& actor = entry.second;
        const int64_t kind = actorKind(&actor);
        const JsonValue* shared = mem(&actor, "sharedKey");
        const JsonValue* position = mem(&actor, "position");
        if (actor.isNull() || blank(entry.first) || entry.first != str(mem(&actor, "role")) || num(mem(&actor, "templateId")) == 0 || !defined(kind, 4) ||
            (kind != 0 && !present(position)) || (present(shared) && blank(str(shared))) || (present(position) && !finitePosition(position)))
            return mission + ": invalid actor role " + entry.first + ".";
        if (const JsonValue* policy = mem(&actor, "gameplayPolicy"); present(policy)) {
            if (kind != 1 && kind != 0) return mission + ", actor " + entry.first + ": gameplay policy requires a creature role.";
            if (std::string e = policyError(*policy); !e.empty()) return mission + ", actor " + entry.first + ": " + e + ".";
        }
        const bool conversation = present(mem(&actor, "conversation"));
        if (const JsonValue* use = mem(&actor, "useAction"); present(use)) {
            const int64_t arg = num(mem(use, "actionArgId"), 1);
            if (missionId == 0 || kind != 2 || conversation || num(mem(use, "missionId")) != missionId || !objectiveIds.count(num(mem(use, "objectiveId"))) ||
                (arg != 1 && arg != 3) || !mem(sequences, std::to_string(num(mem(use, "sequenceId"))).c_str()))
                return mission + ", actor " + entry.first + ": invalid native use action.";
        }
        if (const JsonValue* destruction = mem(&actor, "destruction"); present(destruction)) {
            if (missionId == 0 || kind != 2 || conversation || num(mem(destruction, "missionId")) != missionId ||
                !objectiveIds.count(num(mem(destruction, "objectiveId"))) || num(mem(destruction, "hitPoints"), 100) == 0 ||
                !useObjectStates().count(num(mem(destruction, "destroyedState"), 2)) ||
                !mem(sequences, std::to_string(num(mem(destruction, "sequenceId"))).c_str()))
                return mission + ", actor " + entry.first + ": invalid object destruction action.";
        }
    }
    std::map<std::string, int> sharedKeys;
    for (const auto& entry : entries(actors))
        if (const JsonValue* shared = mem(&entry.second, "sharedKey"); present(shared) && ++sharedKeys[str(shared)] > 1)
            return mission + ": a shared actor cannot have multiple roles in one scene.";
    for (const auto& entry : entries(actors)) {
        const JsonValue* c = mem(&entry.second, "conversation");
        if (!present(c)) continue;
        const int64_t conversationMission = num(mem(c, "missionId")), dialog = num(mem(c, "dialogObjectiveId")), flag = num(mem(c, "playerFlagId"), 1);
        const int64_t kind = enumValue(mem(c, "kind"), {"Completion", "Reminder", "Ambient", "Choice"}, 0);
        if (actorKind(&entry.second) != 2 || conversationMission == 0 || conversationMission > kIntMax || num(mem(c, "objectiveId")) == 0 ||
            num(mem(c, "npcPackageId")) == 0 || dialog == 0 || dialog > kIntMax || flag == 0 || flag > kIntMax || !defined(kind, 4) ||
            (missionId != 0 && (conversationMission != missionId || !objectiveIds.count(num(mem(c, "objectiveId"))))))
            return mission + ": invalid object conversation for " + str(mem(&entry.second, "role")) + ".";
    }
    if (const JsonValue* encounter = mem(&scene, "publicEncounter"); present(encounter)) {
        const JsonValue* actor = mem(actors, str(mem(encounter, "role")).c_str());
        const JsonValue* policyValue = mem(encounter, "ownerLossPolicy");
        const std::string policy = present(policyValue) ? str(policyValue) : std::string("Reset");
        const bool scriptMatches = present(mem(encounter, "scriptKey")) == present(script) && str(mem(encounter, "scriptKey")) == str(script);
        if (num(mem(encounter, "missionId")) != missionId || !scriptMatches || !actor || actorKind(actor) != 0 ||
            num(mem(actor, "templateId")) != num(mem(encounter, "spawnId")) || (policy != "Reset" && policy != "Wait" && policy != "Continue" && policy != "Fail"))
            return mission + ": public encounter does not match its actor/script.";
    }
    for (const auto& route : entries(mem(&scene, "routes"))) {
        const JsonValue* speed = mem(&route.second, "speed");
        bool bad = route.first != str(mem(&route.second, "key")) || elements(mem(&route.second, "points")).empty() || (speed && speed->number() <= 0);
        for (const JsonValue& point : elements(mem(&route.second, "points"))) bad = bad || !present(mem(&point, "position"));
        if (bad) return mission + ": invalid route " + route.first + ".";
    }
    for (const auto& credit : entries(mem(&scene, "credit"))) {
        const Opt objective = keyNumber(credit.first);
        if (!objective || !objectiveIds.count(*objective)) return mission + ": credit policy names an unknown objective.";
    }
    for (const auto& defeat : entries(mem(&scene, "defeatSequences"))) {
        const JsonValue* actor = mem(actors, defeat.first.c_str());
        const int64_t kind = actorKind(actor);
        if (!actor || (kind != 1 && kind != 0) || !mem(sequences, std::to_string(num(&defeat.second)).c_str()))
            return mission + ": invalid actor defeat sequence.";
    }
    std::set<std::string> keys;
    for (const auto& seq : entries(sequences)) {
        if (std::string rejection = evaluateSequence(scene, seq.second); !rejection.empty()) return mission + ", sequence " + seq.first + ": " + rejection;
        for (const char* list : {"world", "character"})
            for (const JsonValue& intent : elements(mem(&seq.second, list)))
                if (!keys.insert(str(mem(&intent, "operationKey"))).second) return mission + ": duplicate operation " + str(mem(&intent, "operationKey")) + ".";
        for (const JsonValue& timer : elements(mem(&seq.second, "timers"))) {
            const JsonValue* cancel = mem(&timer, "cancel");
            const bool cancels = cancel && cancel->type == JsonValue::Type::Bool && cancel->boolean;
            if (!cancels && !mem(sequences, std::to_string(num(mem(&timer, "sequenceId"))).c_str()))
                return mission + ": timer " + str(mem(&timer, "name")) + " has no target sequence.";
        }
    }
    return "";
}

// A mission as the catalog holds it after its scene binding: the definition plus the document's credit, items and dialogue.
struct CatalogMission {
    const Definition* d = nullptr;
    const JsonValue* scene = nullptr;                    // nullptr: no scene binding
    std::map<int64_t, int64_t> credit;                   // objective -> MissionCreditMode (Personal 0 when absent)
    std::vector<const JsonValue*> items;                 // MissionItemBinding (by key: the document's are unique once validated)
    std::vector<const JsonValue*> acceptanceItems;
    std::vector<const JsonValue*> dialogue;              // MissionDialogueTopicDefinition
};

const JsonValue* itemBinding(const CatalogMission& m, const std::string& key) {
    for (const JsonValue* b : m.items)
        if (str(mem(b, "itemKey")) == key) return b;
    return nullptr;
}

// MissionItemValidation.IntentError
std::string intentError(const CatalogMission& m, const JsonValue& intent) {
    const std::string kind = str(mem(&intent, "$kind"));
    const int64_t id = isItemIntentKind(kind) ? num(mem(&intent, "missionId")) : 0;
    const JsonValue* keyValue = isItemIntentKind(kind) ? mem(&intent, "itemKey") : nullptr;
    const JsonValue* binding = present(keyValue) ? itemBinding(m, str(keyValue)) : nullptr;
    const std::string op = str(mem(&intent, "operationKey"));
    if (id != m.d->missionId || id == 0 || !present(keyValue) || !binding || blank(op) || op.size() > 160)
        return "Mission item operation must name its mission, item binding and stable operation key.";
    if (std::string e = bindingError(*binding); !e.empty()) return e;
    const int64_t scope = enumValue(mem(binding, "scope"), {"AssignmentIssued", "CharacterOwned"}, -1);
    const int64_t maximum = num(mem(binding, "maximumQuantity")), quantity = num(mem(&intent, "quantity"));
    if (kind == "issue-mission-item" && scope == 0 && num(mem(&intent, "itemTemplateId")) == num(mem(binding, "itemTemplateId")) && quantity > 0 &&
        quantity <= maximum)
        return "";
    if (kind == "consume-mission-item" && enumValue(mem(&intent, "scope"), {"AssignmentIssued", "CharacterOwned"}, -1) == scope && quantity > 0 &&
        quantity <= maximum)
        return "";
    if (kind == "remove-mission-items" && scope == 0) return "";
    return "Mission item operation does not match its authored template, scope or allowed quantity.";
}

// MissionItemValidation.Errors
std::vector<std::string> itemErrors(const CatalogMission& m, const std::vector<const JsonValue*>& sceneIntents) {
    std::vector<std::string> out;
    for (const JsonValue* b : m.items) {
        if (std::string e = bindingError(*b); !e.empty()) out.push_back(e);
        const JsonValue* drop = mem(b, "drop");
        if (!present(drop)) continue;
        const auto o = m.d->objectives.find(num(mem(drop, "objectiveId")));
        bool acquires = false;
        if (o != m.d->objectives.end())
            for (const Executable& e : o->second.executable) acquires = acquires || (e.rule && e.rule->kind == ItemAcquired && !e.itemCounters.empty());
        if (!acquires) out.push_back("Mission item " + str(mem(b, "itemKey")) + " drop must name an item-acquisition objective.");
    }
    std::vector<const Action*> actions;
    for (const auto& o : m.d->objectives)
        for (const Executable& e : o.second.executable)
            for (const Action& a : e.actions) actions.push_back(&a);
    for (const Action* a : actions) {
        const std::string expected = a->kind == 10 ? "issue-mission-item" : a->kind == 11 ? "consume-mission-item" : a->kind == 12 ? "remove-mission-items"
                                   : a->kind == 13 ? "fail-related-mission" : "";
        if (a->intentKind != expected) out.push_back("Mission item action " + std::to_string(a->id) + " has an invalid intent discriminator.");
        if (a->intentKind == "fail-related-mission") {
            const int64_t other = num(mem(&a->intent, "missionId"));
            const std::string op = str(mem(&a->intent, "operationKey"));
            if (other == 0 || other == m.d->missionId || blank(op) || op.size() > 160)
                out.push_back("Related failure action " + std::to_string(a->id) + " must name another mission and a stable operation key.");
        }
    }
    std::set<std::string> keys, turnInKeys;
    for (const JsonValue* b : m.items)
        if (num(mem(b, "turnInQuantity")) > 0) turnInKeys.insert("mission-turn-in:" + str(mem(b, "itemKey")));
    std::vector<const JsonValue*> intents = m.acceptanceItems;
    for (const Action* a : actions)
        if (isItemIntentKind(a->intentKind)) intents.push_back(&a->intent);
    for (const JsonValue* intent : sceneIntents)
        if (isItemIntentKind(str(mem(intent, "$kind")))) intents.push_back(intent);
    for (const JsonValue* intent : intents) {
        const std::string op = str(mem(intent, "operationKey"));
        if (std::string e = intentError(m, *intent); !e.empty()) out.push_back(e);
        else if (turnInKeys.count(op)) out.push_back("Mission item operation key is reserved for turn-in: " + op + ".");
        else if (!keys.insert(op).second) out.push_back("Mission item operation key is duplicated: " + op + ".");
    }
    return out;
}

// MissionDialogueValidation.CanSelect
bool canSelect(const Executable& e, int64_t package, int64_t flag) {
    if (e.rule || (e.toState && *e.toState != 2 && *e.toState != 3)) return false;
    bool conversation = false;
    for (const auto& c : e.conversationKeys) conversation = conversation || (c.first == package && c.second == flag);
    if (!conversation) return false;
    for (const Action& a : e.actions)
        if (a.kind < 1 || a.kind > 13) return false;
    return true;
}

// MissionDialogueValidation.Errors
std::vector<std::string> dialogueErrors(const CatalogMission& m) {
    std::vector<std::string> out;
    std::set<std::tuple<int64_t, int64_t, int64_t, int64_t>> keys;          // (kind, objective, package, flag)
    const auto topicMatches = [&](int64_t objective, int64_t package, int64_t flag) {
        for (const JsonValue* t : m.dialogue)
            if (!t->isNull() && num(mem(t, "objectiveId")) == objective && num(mem(t, "npcPackageId")) == package && num(mem(t, "playerFlagId")) == flag)
                return true;
        return false;
    };
    for (const auto& o : m.d->objectives)
        for (const Executable& e : o.second.executable)
            for (const auto& c : e.conversationKeys)
                if (!topicMatches(o.first, c.first, c.second)) keys.emplace(0, o.first, c.first, c.second);
    const auto validId = [](int64_t id) { return id > 0 && id <= kIntMax; };
    for (const JsonValue* t : m.dialogue) {
        if (t->isNull()) {
            out.push_back("dialogue topic is null");
            continue;
        }
        const int64_t objectiveId = num(mem(t, "objectiveId")), package = num(mem(t, "npcPackageId")), flag = num(mem(t, "playerFlagId"));
        const Opt dialogObjective = optNum(mem(t, "dialogObjectiveId"));
        const int64_t dialogId = dialogObjective.value_or(objectiveId);
        const int64_t kind = enumValue(mem(t, "kind"), {"Completion", "Reminder", "Ambient", "Choice"}, 0);
        const JsonValue* source = mem(t, "sourceCreatureId");
        const std::string prefix = "dialogue objective " + std::to_string(objectiveId) + ", package " + std::to_string(package);
        const auto objective = m.d->objectives.find(objectiveId);
        if (!defined(kind, 4) || !validId(m.d->missionId) || !validId(dialogId) || !validId(package) || (present(source) && num(source) == 0) ||
            flag > kIntMax || objective == m.d->objectives.end()) {
            out.push_back(prefix + ": invalid native topic binding");
            continue;
        }
        if (!keys.emplace(kind, dialogId, package, flag).second) out.push_back(prefix + ": duplicate native topic binding");
        if (const JsonValue* requirement = mem(t, "requirement"); present(requirement))
            if (std::string e = requirementError(requirement, false); !e.empty()) out.push_back(prefix + ": invalid topic requirement: " + e);
        const JsonValue* transition = mem(t, "transitionId");
        const JsonValue* choices = mem(t, "choices");
        if (kind == 1 || kind == 2) {
            if (present(transition) || present(choices)) out.push_back(prefix + ": read-only dialogue cannot bind transitions");
            continue;
        }
        std::vector<int64_t> branch;
        if (kind == 3) {
            const auto& c = entries(choices);
            bool ok = !present(transition) && present(choices) && c.size() >= 2 && c.size() <= 3;
            for (int64_t i = 1; ok && i <= int64_t(c.size()); ++i) ok = mem(choices, std::to_string(i).c_str()) != nullptr;
            if (!ok) {
                out.push_back(prefix + ": choice dialogue must map native indices 1 and 2, optionally 3");
                continue;
            }
            for (const auto& entry : c)
                if (std::find(branch.begin(), branch.end(), num(&entry.second)) == branch.end()) branch.push_back(num(&entry.second));
        } else {
            if (!present(transition) || present(choices)) {
                out.push_back(prefix + ": completion dialogue requires one transition");
                continue;
            }
            branch.push_back(num(transition));
        }
        for (int64_t id : branch) {
            std::vector<const Executable*> found;
            for (const Executable& e : objective->second.executable)
                if (e.id == id) found.push_back(&e);
            if (found.size() != 1 || !canSelect(*found[0], package, flag))
                out.push_back(prefix + ": transition " + std::to_string(id) + " is not an executable dialogue branch for this source");
        }
    }
    return out;
}

// MissionSceneValidation.SupportsSharedAssignment
bool supportsSharedAssignment(const CatalogMission& m, const std::map<int64_t, std::vector<int64_t>>& graph) {
    const Definition& d = *m.d;
    std::map<int64_t, std::vector<int64_t>> dependencies;
    for (const auto& entry : graph)
        for (int64_t id : entry.second) {
            const auto target = d.objectives.find(id);
            if (target != d.objectives.end() && (target->second.initialState == Opt(4) || target->second.initialState == Opt(0)))
                dependencies[id].push_back(entry.first);
        }
    for (const auto& o : d.objectives) {
        for (const Executable& e : o.second.executable)
            if (e.rule && e.rule->kind == ObjectiveStateReached)
                for (int64_t s : e.rule->subjects) dependencies[o.first].push_back(s);
        if (o.second.aggregation)
            for (int64_t c : o.second.aggregation->children) dependencies[o.first].push_back(c);
    }
    std::set<int64_t> required;
    for (const auto& o : d.objectives)
        if (o.second.required == std::optional<bool>(true)) required.insert(o.first);
    std::vector<int64_t> pending(required.begin(), required.end());
    while (!pending.empty()) {
        const int64_t id = pending.back();
        pending.pop_back();
        for (int64_t dependency : dependencies[id])
            if (required.insert(dependency).second) pending.push_back(dependency);
    }
    for (const auto& o : d.objectives) {
        const auto credit = m.credit.find(o.first);
        const bool personal = credit == m.credit.end() || credit->second == 0;
        for (const Executable& e : o.second.executable) {
            if (e.rule && e.rule->kind == DeadlineElapsed) return false;
            if (required.count(o.first) && personal && e.rule && e.rule->kind == ScenarioEvent) return false;
            for (const Action& a : e.actions)
                if (a.kind == 5 || a.kind == 6) return false;
        }
    }
    return true;
}

// MissionConversationProjection.ForObject != null
bool objectConversationProjects(const CatalogMission& m, const JsonValue& c) {
    const int64_t objective = num(mem(&c, "objectiveId")), package = num(mem(&c, "npcPackageId")), flag = num(mem(&c, "playerFlagId"), 1);
    const int64_t dialog = num(mem(&c, "dialogObjectiveId"));
    const int64_t kind = enumValue(mem(&c, "kind"), {"Completion", "Reminder", "Ambient", "Choice"}, 0);
    int matches = 0;
    bool candidate = false;
    for (const JsonValue* t : m.dialogue) {
        const bool same = num(mem(t, "objectiveId")) == objective && num(mem(t, "npcPackageId")) == package && num(mem(t, "playerFlagId")) == flag;
        candidate = candidate || same;
        if (same && optNum(mem(t, "dialogObjectiveId")).value_or(num(mem(t, "objectiveId"))) == dialog &&
            enumValue(mem(t, "kind"), {"Completion", "Reminder", "Ambient", "Choice"}, 0) == kind)
            ++matches;
    }
    if (matches == 1) return true;
    if (matches > 1) return false;                       // SingleOrDefault throws
    return kind == 0 && !candidate;
}

void validateCatalogScenes(const std::map<int64_t, Definition>& definitions, const std::set<int64_t>& inactive, const MissionReferences& refs,
                           const std::function<void(const std::string&, int64_t, const std::string&)>& fail) {
    std::map<int64_t, CatalogMission> missions;
    for (const auto& d : definitions) missions[d.first].d = &d.second;
    std::vector<const JsonValue*> sceneDocuments;           // SceneBindings.Values (binding order), then the experiences' scenes
    std::vector<std::pair<const JsonValue*, const CatalogMission*>> bound;
    for (const auto& dp : definitions) {
        const Definition& d = dp.second;
        if (!d.hasScene) continue;
        const JsonValue& scene = d.scene;
        const std::string mission = "Mission " + std::to_string(d.missionId);
        const JsonValue* script = mem(&scene, "script");
        if (str(script) != d.sceneScriptKey || num(mem(&scene, "stateVersion"), 1) != d.sceneStateVersion) {
            fail(mission + " has inconsistent script/version bindings.", d.missionId, d.revision);
            continue;
        }
        if (std::string e = documentReadError(scene); !e.empty()) {
            fail(mission + " scene binding cannot be read: " + e, d.missionId, d.revision);
            continue;
        }
        std::set<int64_t> objectiveIds;
        for (const auto& o : d.objectives) objectiveIds.insert(o.first);
        if (std::string e = validateScene(d.missionId, scene, objectiveIds); !e.empty()) {
            fail(e, d.missionId, d.revision);
            continue;
        }
        bool failed = false;
        for (const auto& actor : entries(mem(&scene, "actors")))
            if (!failed && actorKind(&actor.second) == 0 && !refs.spawnPoolIds.count(num(mem(&actor.second, "templateId")))) {
                fail(mission + ": migrated public spawn " + std::to_string(num(mem(&actor.second, "templateId"))) + " does not exist.", d.missionId, d.revision);
                failed = true;
            }
        if (failed) continue;
        CatalogMission& m = missions[d.missionId];
        m.scene = &scene;
        for (const auto& credit : entries(mem(&scene, "credit")))
            m.credit[keyNumber(credit.first).value_or(0)] = enumValue(mem(&credit.second, "mode"), {"Personal", "NearbyParty", "EncounterParticipants"}, 0);
        for (const auto& credit : m.credit) {
            if (credit.second == 0) continue;
            for (const Executable& e : d.objectives.at(credit.first).executable)
                if (!e.rule || (!isKill(e.rule->kind) && e.rule->kind != ScenarioEvent)) {
                    if (!failed) fail(mission + ": objective " + std::to_string(credit.first) + " cannot share personal actions.", d.missionId, d.revision);
                    failed = true;
                }
        }
        if (failed) continue;
        for (const JsonValue& item : elements(mem(&scene, "items"))) m.items.push_back(&item);
        for (const JsonValue& item : elements(mem(&scene, "acceptanceItems"))) m.acceptanceItems.push_back(&item);
        std::vector<const JsonValue*> sceneIntents;
        for (const auto& seq : entries(mem(&scene, "sequences")))
            for (const JsonValue& intent : elements(mem(&seq.second, "character"))) sceneIntents.push_back(&intent);
        if (const std::vector<std::string> errors = itemErrors(m, sceneIntents); !errors.empty()) {
            fail(mission + ": " + join(errors, "; "), d.missionId, d.revision);
            continue;
        }
        bool templatesOk = true;
        for (const JsonValue* item : m.items) templatesOk = templatesOk && refs.itemTemplateClasses.count(num(mem(item, "itemTemplateId")));
        if (!templatesOk) {
            fail(mission + ": mission item template is missing.", d.missionId, d.revision);
            continue;
        }
        // MissionLootPlanner.ValidateReferences
        for (const JsonValue* b : m.items) {
            const JsonValue* drop = mem(b, "drop");
            if (failed || !present(drop)) continue;
            const std::string item = mission + ": item " + str(mem(b, "itemKey"));
            bool sources = refs.mapContextIds.count(num(mem(drop, "mapContextId"))) != 0;
            for (const JsonValue& c : elements(mem(drop, "creatureIds"))) sources = sources && refs.creatureClasses.count(num(&c));
            const int64_t templateId = num(mem(b, "itemTemplateId"));
            const auto link = refs.itemTemplateClasses.find(templateId);
            const auto stack = link == refs.itemTemplateClasses.end() ? refs.itemClassStackSizes.end() : refs.itemClassStackSizes.find(link->second);
            const auto objective = d.objectives.find(num(mem(drop, "objectiveId")));
            bool acquires = false;
            if (objective != d.objectives.end() && link != refs.itemTemplateClasses.end())
                for (const Executable& e : objective->second.executable) {
                    const auto counter = e.itemCounters.find(link->second);
                    acquires = acquires || (e.rule && e.rule->kind == ItemAcquired && e.rule->subjects.count(link->second) && counter != e.itemCounters.end() &&
                                            counter->second.target > 0);
                }
            if (std::string e = bindingError(*b); !e.empty()) fail(mission + ": " + e, d.missionId, d.revision);
            else if (!sources) fail(item + " has a missing drop source or map.", d.missionId, d.revision);
            else if (!refs.itemTemplateIds.count(templateId) || link == refs.itemTemplateClasses.end() || !refs.entityClassIds.count(link->second) ||
                     stack == refs.itemClassStackSizes.end() || num(mem(drop, "quantity")) > stack->second)
                fail(item + " has an invalid drop template or stack size.", d.missionId, d.revision);
            else if (!acquires) fail(item + " drop does not match its acquisition objective.", d.missionId, d.revision);
            else continue;
            failed = true;
        }
        if (failed) continue;
        const JsonValue* dialogue = mem(&scene, "dialogue");
        if (present(dialogue)) {
            bool packages = true, creatures = true;
            for (const JsonValue& t : elements(dialogue)) {
                const int64_t package = num(mem(&t, "npcPackageId"));
                packages = packages && (refs.npcPackageIds.count(package) || d.objectNpcPackages.count(package));
                if (const JsonValue* source = mem(&t, "sourceCreatureId"); present(source))
                    creatures = creatures && refs.npcPackageRows.count({num(source), package});
            }
            if (!packages) {
                fail(mission + ": dialogue references a missing NPC package.", d.missionId, d.revision);
                continue;
            }
            if (!creatures) {
                fail(mission + ": dialogue names the wrong creature for its native package.", d.missionId, d.revision);
                continue;
            }
            for (const JsonValue& t : elements(dialogue)) m.dialogue.push_back(&t);
            if (const std::vector<std::string> errors = dialogueErrors(m); !errors.empty()) {
                fail(mission + ": " + join(errors, "; "), d.missionId, d.revision);
                continue;
            }
            // ValidateDialogueActions
            const JsonValue* sequences = mem(&scene, "sequences");
            for (const JsonValue* t : m.dialogue) {
                const int64_t kind = enumValue(mem(t, "kind"), {"Completion", "Reminder", "Ambient", "Choice"}, 0);
                if (failed || (kind != 0 && kind != 3)) continue;
                std::vector<int64_t> branch;
                if (kind == 3)
                    for (const auto& c : entries(mem(t, "choices"))) branch.push_back(num(&c.second));
                else
                    branch.push_back(num(mem(t, "transitionId")));
                for (const Executable& e : d.objectives.at(num(mem(t, "objectiveId"))).executable) {
                    if (failed || std::find(branch.begin(), branch.end(), e.id) == branch.end()) continue;
                    for (const Action& a : e.actions) {
                        if (failed) break;
                        if (a.kind == 5 && (!present(script) || !a.scenario || !mem(sequences, std::to_string(*a.scenario).c_str()))) {
                            fail(mission + ": dialogue transition " + std::to_string(e.id) + " has an unbound scene sequence.", d.missionId, d.revision);
                            failed = true;
                        } else if (a.kind == 6) {
                            const JsonValue* name = a.spawnGroup ? mem(mem(&scene, "names"), ("spawn-group-" + std::to_string(*a.spawnGroup)).c_str()) : nullptr;
                            if (!present(script) || !name || !mem(sequences, std::to_string(num(name)).c_str())) {
                                fail(mission + ": dialogue transition " + std::to_string(e.id) + " has an unbound spawn-group sequence.", d.missionId, d.revision);
                                failed = true;
                            }
                        }
                    }
                }
            }
            if (failed) continue;
        }
        // ValidateSharing
        const JsonValue* allowJoin = mem(mem(&scene, "publicEncounter"), "allowPartyJoin");
        const bool joins = allowJoin && allowJoin->type == JsonValue::Type::Bool && allowJoin->boolean;
        if (d.row.i("shareable") != 0 && present(script) && (!joins || !supportsSharedAssignment(m, objectiveGraph(d.objectives)))) {
            fail(mission + ": scripted assignments must be nonshareable unless their public encounter supports party joining without assignment-owned scene "
                           "clocks, world-command transitions or required owner-only scene events, including optional objective dependencies.",
                 d.missionId, d.revision);
            continue;
        }
        sceneDocuments.push_back(&scene);
        bound.emplace_back(&scene, &m);
    }
    // ValidateObjectDialogue
    const auto objectDialogue = [&](const JsonValue& scene, int64_t missionForReport) {
        for (const auto& actor : entries(mem(&scene, "actors"))) {
            const JsonValue* c = mem(&actor.second, "conversation");
            if (!present(c)) continue;
            const auto m = missions.find(num(mem(c, "missionId")));
            if (m == missions.end() || !m->second.d->objectives.count(num(mem(c, "objectiveId"))) || !objectConversationProjects(m->second, *c)) {
                fail("Object " + str(mem(&actor.second, "role")) + ": conversation has no matching authored dialogue topic.", missionForReport, "");
                return false;
            }
        }
        return true;
    };
    for (const auto& b : bound) objectDialogue(*b.first, b.second->d->missionId);
    // (a binding that failed above has already stopped the server: judged by its document)
    for (const auto& dp : definitions)
        if (!dp.second.scenarios.empty() && !dp.second.sceneUnreadable && (!dp.second.hasScene || blank(str(mem(&dp.second.scene, "script")))))
            fail("Mission " + std::to_string(dp.first) + " requires a migrated scene script binding.", dp.first, dp.second.revision);
    // Experiences
    std::vector<JsonValue> experienceDocuments(refs.experiences.size());
    std::vector<std::pair<std::string, const JsonValue*>> experiences;     // key, document
    for (size_t i = 0; i < refs.experiences.size(); ++i) {
        const MissionReferences::Experience& row = refs.experiences[i];
        JsonValue& doc = experienceDocuments[i];
        std::string error;
        if (!parseJson(row.bindings, doc, error) || doc.isNull()) {
            fail("Experience " + row.key + " has no binding definition" + (error.empty() ? std::string(".") : " (" + error + ")."), -1, "");
            continue;
        }
        // JsonSerializer.Deserialize<MissionExperienceDefinition>(bindings, MissionContentCodec.Options) ?? throw.
        if (std::string e = sceneReadError(doc, true); !e.empty()) {
            fail("Experience " + row.key + " binding cannot be read: " + e, -1, "");
            continue;
        }
        const JsonValue* privateValue = mem(&doc, "privatePerCharacter");
        if (str(mem(&doc, "key")) != row.key || num(mem(&doc, "mapContextId")) != row.mapContextId ||
            !(privateValue && privateValue->type == JsonValue::Type::Bool && privateValue->boolean) || blank(str(mem(&doc, "revision")))) {
            fail("Experience " + row.key + " has inconsistent migrated bindings.", -1, "");
            continue;
        }
        const JsonValue* scene = mem(&doc, "scene");
        static const JsonValue emptyScene = [] {
            JsonValue v;
            v.type = JsonValue::Type::Object;
            return v;
        }();
        if (!present(scene)) scene = &emptyScene;
        if (std::string e = documentReadError(*scene); !e.empty()) {
            fail("Experience " + row.key + " scene cannot be read: " + e, -1, "");
            continue;
        }
        if (std::string e = validateScene(0, *scene, {}); !e.empty()) {
            fail(e, -1, "");
            continue;
        }
        bool failed = false;
        const JsonValue* sequences = mem(scene, "sequences");
        for (const JsonValue& t : elements(mem(&doc, "missionTriggers"))) {
            const std::string event = str(mem(&t, "event"));
            if (!failed && (!missions.count(num(mem(&t, "missionId"))) || !mem(sequences, std::to_string(num(mem(&t, "sequenceId"))).c_str()) ||
                            (event != "Accepted" && event != "Rewarded" && event != "Completeable" && event != "Departing"))) {
                fail("Experience " + row.key + ": invalid mission trigger " + std::to_string(num(mem(&t, "missionId"))) + "/" + event + ".", -1, "");
                failed = true;
            }
        }
        for (const auto& actor : entries(mem(scene, "actors")))
            if (!failed && actorKind(&actor.second) == 0 && !refs.spawnPoolIds.count(num(mem(&actor.second, "templateId")))) {
                fail("Experience " + row.key + ": migrated public spawn " + std::to_string(num(mem(&actor.second, "templateId"))) + " does not exist.", -1, "");
                failed = true;
            }
        for (const auto& actor : entries(mem(scene, "actors"))) {
            const JsonValue* c = mem(&actor.second, "conversation");
            if (failed || !present(c)) continue;
            const auto m = missions.find(num(mem(c, "missionId")));
            if (m == missions.end() || !m->second.d->objectives.count(num(mem(c, "objectiveId")))) {
                fail("Experience " + row.key + ": object conversation has an unknown mission objective.", -1, "");
                failed = true;
            }
        }
        if (failed || !objectDialogue(*scene, -1)) continue;
        experiences.emplace_back(row.key, &doc);
        sceneDocuments.push_back(scene);
    }
    // Radio offers from scenes need the target's Scene source keyed by the scene's script.
    for (const JsonValue* scene : sceneDocuments) {
        const JsonValue* script = mem(scene, "script");
        for (const auto& seq : entries(mem(scene, "sequences")))
            for (const JsonValue& intent : elements(mem(&seq.second, "character"))) {
                if (str(mem(&intent, "$kind")) != "offer-radio-mission") continue;
                const int64_t target = num(mem(&intent, "missionId"));
                const auto d = definitions.find(target);
                bool authorized = false;
                if (present(script) && d != definitions.end() && !inactive.count(target))
                    for (const Definition::RadioSource& s : d->second.radioSources) authorized = authorized || (s.kind == 1 && s.key == str(script));
                if (!authorized) fail("Scene " + str(script) + ": radio offer " + std::to_string(target) + " has no authorized target source.", target, "");
            }
    }
    // ValidateSharedActors
    std::map<std::string, const JsonValue*> sharedFirst;
    for (const JsonValue* scene : sceneDocuments)
        for (const auto& actor : entries(mem(scene, "actors"))) {
            const JsonValue* shared = mem(&actor.second, "sharedKey");
            if (!present(shared)) continue;
            const auto first = sharedFirst.emplace(str(shared), &actor.second);
            if (first.second) continue;
            const JsonValue* a = first.first->second;
            const JsonValue* pa = mem(a, "gameplayPolicy");
            const JsonValue* pb = mem(&actor.second, "gameplayPolicy");
            // EquivalentTo: the same settings, tags as a set, the same loot drops (compared as written).
            const auto equivalent = [](const JsonValue* x, const JsonValue* y) {
                if (!present(x) || !present(y)) return !present(x) && !present(y);
                const auto flag = [](const JsonValue* p, const char* name) {
                    const JsonValue* v = mem(p, name);
                    return v && v->type == JsonValue::Type::Bool && v->boolean;
                };
                std::set<std::string> tx, ty;
                for (const JsonValue& t : elements(mem(x, "tags"))) tx.insert(t.text);
                for (const JsonValue& t : elements(mem(y, "tags"))) ty.insert(t.text);
                const JsonValue* rx = mem(x, "defenseRadius");
                const JsonValue* ry = mem(y, "defenseRadius");
                const JsonValue* lx = mem(x, "loot");
                const JsonValue* ly = mem(y, "loot");
                return flag(x, "invulnerable") == flag(y, "invulnerable") && (rx ? rx->number() : 0.0) == (ry ? ry->number() : 0.0) &&
                       present(mem(x, "defenseTargetTag")) == present(mem(y, "defenseTargetTag")) &&
                       str(mem(x, "defenseTargetTag")) == str(mem(y, "defenseTargetTag")) && flag(x, "rewardScenarioKills") == flag(y, "rewardScenarioKills") &&
                       flag(x, "trackParticipation") == flag(y, "trackParticipation") && tx == ty && present(lx) == present(ly) &&
                       (!present(lx) || writeJson(*lx) == writeJson(*ly));
            };
            if (actorKind(a) != actorKind(&actor.second) || num(mem(a, "templateId")) != num(mem(&actor.second, "templateId")) || !equivalent(pa, pb)) {
                fail("Shared actor " + str(shared) + ": kind, template and gameplay policy must agree.", -1, "");
                sharedFirst[str(shared)] = &actor.second;
            }
        }
    // ValidateActorPolicies
    std::vector<std::pair<std::string, const JsonValue*>> policies;
    for (const auto& b : bound)
        for (const auto& actor : entries(mem(b.first, "actors")))
            if (const JsonValue* p = mem(&actor.second, "gameplayPolicy"); present(p))
                policies.emplace_back("Mission " + std::to_string(b.second->d->missionId) + ", actor " + str(mem(&actor.second, "role")), p);
    for (const auto& e : experiences) {
        for (const auto& actor : entries(mem(mem(e.second, "scene"), "actors")))
            if (const JsonValue* p = mem(&actor.second, "gameplayPolicy"); present(p))
                policies.emplace_back("Experience " + e.first + ", actor " + str(mem(&actor.second, "role")), p);
        for (const auto& policy : entries(mem(e.second, "actorPolicies"))) policies.emplace_back("Experience " + e.first + ", creature " + policy.first, &policy.second);
    }
    for (const auto& p : policies) {
        const std::string error = p.second->isNull() ? std::string("missing policy") : policyError(*p.second);
        if (!error.empty()) fail(p.first + ": invalid gameplay policy (" + error + ").", -1, "");
    }
    for (const auto& p : policies)
        for (const JsonValue& drop : elements(mem(mem(p.second, "loot"), "drops"))) {
            const int64_t templateId = num(mem(&drop, "templateId"));
            const auto link = refs.itemTemplateClasses.find(templateId);
            const auto stack = link == refs.itemTemplateClasses.end() ? refs.itemClassStackSizes.end() : refs.itemClassStackSizes.find(link->second);
            if (!refs.itemTemplateIds.count(templateId) || link == refs.itemTemplateClasses.end() || !refs.entityClassIds.count(link->second) ||
                stack == refs.itemClassStackSizes.end() || num(mem(&drop, "maximum")) > stack->second)
                fail(p.first + ": loot template " + std::to_string(templateId) + " is missing an item class or exceeds its stack size.", -1, "");
        }
    // Every operational mission: MissionRequirementService.Validate(mission) and MissionItemValidation.Errors(mission).
    for (const auto& entry : missions) {
        const CatalogMission& m = entry.second;
        const Definition& d = *m.d;
        if (inactive.count(d.missionId)) continue;
        const std::string mission = "Mission " + std::to_string(d.missionId);
        std::string error;                               // (a scene's own requirements were checked by validateScene above)
        for (const Definition::RadioSource& s : d.radioSources)
            if (error.empty()) error = requirementError(&s.requirement, true);
        for (const JsonValue* t : m.dialogue)
            if (error.empty()) error = requirementError(mem(t, "requirement"), false);
        if (!error.empty()) {
            fail(error, d.missionId, d.revision);
            continue;
        }
        // (a mission with a scene binding had these checked above, with its scene's intents too)
        if (d.hasScene) continue;
        if (const std::vector<std::string> errors = itemErrors(m, {}); !errors.empty()) fail(mission + ": " + join(errors, "; "), d.missionId, d.revision);
    }
}

std::string keyOf(int64_t mission, const std::string& revision) { return std::to_string(mission) + "\x1f" + revision; }

// ---- Reading a scene document as System.Text.Json does ------------------------------------------------------------------------
// MissionContentLoader (and the catalog after it) read each mission_scene_binding document as a MissionSceneDefinition, each
// experience as a MissionExperienceDefinition, with MissionContentCodec.Options; any failure is an exception and the server does
// not start. The contract is the server's own (MissionSceneSchema.h, dumped from JsonTypeInfo by tools/missioncheck); the reading
// rules below were measured against the real deserialiser (tools/missioncheck/scene_fuzz.py compares the two on generated
// documents): camelCase names, case-sensitive; unmapped members are errors; a "$kind" discriminator must come first in a
// polymorphic object, be a string and name a derived type (case-sensitive), and no other "$" member may follow; numbers read as
// integers only from plain integer literals in range (no fraction or exponent, no strings); floats take any number (an overflow
// is infinity); null only for reference types and nullables; enums (JsonStringEnumConverter) take an integer in the int range or
// a string read as Enum.TryParse with ignoreCase (a trimmed integer, or comma-separated names - only a trailing empty part is
// allowed); dictionary keys: uint digits only (leading zeros allowed), int an optional sign and digits, enum keys as enum
// strings; read-only members are skipped unread; missing [JsonRequired] members are errors; after its members are read, an
// object's constructor runs, and five of them throw on bad values (MissionHistoryAggregation, MissionObjectiveAggregation,
// MissionCreditPolicy, AuthoredLootProfile, MissionItemDropDefinition). The first failure in document order is reported (an
// object's own required / constructor checks after its members), as the deserialiser stops at it.

struct SchemaObject;
struct SchemaType {
    enum class Kind { String, Bool, UInt, Int, ULong, Long, UShort, Short, Byte, SByte, Float, Double, Json, Enum, Nullable, List, Dict, Object, Unsupported };
    Kind kind = Kind::Unsupported;
    std::string name;                                            // enum / object / unsupported name
    std::vector<std::pair<std::string, int64_t>> enumValues;
    const SchemaType* inner = nullptr;                           // nullable / list element / dictionary value
    const SchemaType* key = nullptr;                             // dictionary key
    const SchemaObject* object = nullptr;
};
struct SchemaProperty {
    std::string name;
    const SchemaType* type = nullptr;
    bool required = false, settable = true;
};
struct SchemaObject {
    std::string key;                                             // full .NET name
    bool isAbstract = false;
    std::vector<SchemaProperty> properties;
    std::string discriminator;                                   // "" = not polymorphic
    std::vector<std::pair<std::string, const SchemaObject*>> derived;
    const SchemaProperty* property(const std::string& name) const {
        for (const SchemaProperty& p : properties)
            if (p.name == name) return &p;
        return nullptr;
    }
};
struct SceneSchema {
    std::vector<std::unique_ptr<SchemaType>> types;
    std::map<std::string, std::unique_ptr<SchemaObject>> objects;
    const SchemaObject* scene = nullptr;
    const SchemaObject* experience = nullptr;

    SchemaObject* objectNamed(const std::string& key) {
        std::unique_ptr<SchemaObject>& o = objects[key];
        if (!o) {
            o = std::make_unique<SchemaObject>();
            o->key = key;
        }
        return o.get();
    }
    // A type expression of the dump ("dict:uint:obj:Rasa...", "enum:Name:flags:A=0,B=1", ...), read from `at`.
    const SchemaType* parse(const std::string& text, size_t& at) {
        const auto token = [&]() {
            const size_t end = std::min(text.find(':', at), text.size());
            std::string t = text.substr(at, end - at);
            at = end < text.size() ? end + 1 : end;
            return t;
        };
        auto t = std::make_unique<SchemaType>();
        const std::string head = token();
        static const std::map<std::string, SchemaType::Kind> leaves = {
            {"string", SchemaType::Kind::String}, {"bool", SchemaType::Kind::Bool},     {"uint", SchemaType::Kind::UInt},
            {"int", SchemaType::Kind::Int},       {"ulong", SchemaType::Kind::ULong},   {"long", SchemaType::Kind::Long},
            {"ushort", SchemaType::Kind::UShort}, {"short", SchemaType::Kind::Short},   {"byte", SchemaType::Kind::Byte},
            {"sbyte", SchemaType::Kind::SByte},   {"float", SchemaType::Kind::Float},   {"double", SchemaType::Kind::Double},
            {"json", SchemaType::Kind::Json}};
        if (const auto leaf = leaves.find(head); leaf != leaves.end()) t->kind = leaf->second;
        else if (head == "enum") {
            t->kind = SchemaType::Kind::Enum;
            t->name = token();
            token();                                             // [Flags]: read the same way by Enum.TryParse
            std::string list = token(), part;
            for (size_t p = 0; p <= list.size(); ++p) {
                if (p < list.size() && list[p] != ',') { part += list[p]; continue; }
                if (const size_t eq = part.find('='); eq != std::string::npos)
                    t->enumValues.emplace_back(part.substr(0, eq), std::strtoll(part.c_str() + eq + 1, nullptr, 10));
                part.clear();
            }
        } else if (head == "nullable") {
            t->kind = SchemaType::Kind::Nullable;
            t->inner = parse(text, at);
        } else if (head == "list") {
            t->kind = SchemaType::Kind::List;
            t->inner = parse(text, at);
        } else if (head == "dict") {
            t->kind = SchemaType::Kind::Dict;
            t->key = parse(text, at);
            t->inner = parse(text, at);
        } else if (head == "obj") {
            t->kind = SchemaType::Kind::Object;
            t->name = text.substr(at);                           // the rest: object names hold no ':'
            at = text.size();
            t->object = objectNamed(t->name);
        } else {
            t->name = text;
            at = text.size();
        }
        types.push_back(std::move(t));
        return types.back().get();
    }

    SceneSchema() {
        JsonValue dump;
        std::string error;
        if (!parseJson(missionSceneSchemaJson(), dump, error)) throw std::logic_error("MissionSceneSchema.h: " + error);
        for (const auto& entry : entries(dump.member("types"))) {
            SchemaObject* o = objectNamed(entry.first);
            const JsonValue* a = entry.second.member("abstract");
            o->isAbstract = a && a->boolean;
            for (const JsonValue& p : elements(entry.second.member("properties"))) {
                SchemaProperty prop;
                prop.name = str(p.member("name"));
                size_t at = 0;
                prop.type = parse(str(p.member("type")), at);
                const JsonValue* r = p.member("required");
                const JsonValue* s = p.member("settable");
                prop.required = r && r->boolean;
                prop.settable = !s || s->boolean;
                o->properties.push_back(prop);
            }
            o->discriminator = str(entry.second.member("discriminator"));
            for (const JsonValue& d : elements(entry.second.member("derived"))) {
                std::string type = str(d.member("type"));
                if (type.rfind("obj:", 0) == 0) type = type.substr(4);
                o->derived.emplace_back(str(d.member("id")), objectNamed(type));
            }
        }
        scene = objectNamed("Rasa.Missions.Content.MissionSceneDefinition");
        experience = objectNamed("Rasa.Missions.Content.MissionExperienceDefinition");
    }
};
const SceneSchema& sceneSchema() {
    static const SceneSchema schema;
    return schema;
}

// A JSON integer literal (no fraction, no exponent) in [low, high].
bool integerLiteral(const JsonValue& v, int64_t low, uint64_t high) {
    if (!v.isNumber() || v.text.find_first_of(".eE") != std::string::npos) return false;
    const bool negative = !v.text.empty() && v.text[0] == '-';
    if (negative && low >= 0) return false;                       // "-0" is not a uint either
    const std::string digits = v.text.substr(negative ? 1 : 0);
    if (digits.empty() || digits.size() > 20) return false;
    errno = 0;
    const unsigned long long magnitude = std::strtoull(digits.c_str(), nullptr, 10);
    if (errno == ERANGE) return false;
    if (negative) return magnitude <= static_cast<unsigned long long>(-(low + 1)) + 1ULL;
    return magnitude <= high;
}
// Digits with an optional sign (allowed signs given) in [low, high]: dictionary keys and enum strings.
bool integerText(const std::string& text, const char* signs, int64_t low, int64_t high) {
    size_t p = 0;
    bool negative = false;
    if (p < text.size() && std::strchr(signs, text[p]) && text[p] != '\0') negative = text[p++] == '-';
    if (p == text.size() || text.size() - p > 19) return false;
    for (size_t i = p; i < text.size(); ++i)
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    const long long magnitude = std::strtoll(text.c_str() + p, nullptr, 10);
    const long long value = negative ? -magnitude : magnitude;
    return value >= low && value <= high;
}
std::string trimmed(const std::string& text) {
    size_t a = 0, b = text.size();
    while (a < b && std::isspace(static_cast<unsigned char>(text[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(text[b - 1]))) --b;
    return text.substr(a, b - a);
}
bool sameIgnoringCase(const std::string& a, const std::string& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
           });
}
// An enum string (Enum.TryParse, ignoreCase); the value's -1 when it names more than one constant or none is needed.
bool enumText(const std::string& text, const SchemaType& t, int64_t* value) {
    const std::string s = trimmed(text);
    if (s.empty()) return false;
    if (std::isdigit(static_cast<unsigned char>(s[0])) || s[0] == '+' || s[0] == '-') {
        if (!integerText(s, "+-", -2147483648LL, 2147483647LL)) return false;
        if (value) *value = std::strtoll(s.c_str(), nullptr, 10);
        return true;
    }
    int64_t combined = 0;
    size_t start = 0;
    while (true) {
        const size_t comma = s.find(',', start);
        const std::string part = trimmed(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        const bool last = comma == std::string::npos;
        if (part.empty()) {
            if (!last || start == 0) return false;               // ",A" fails, "A," reads as A
        } else {
            bool found = false;
            for (const auto& e : t.enumValues)
                if (sameIgnoringCase(e.first, part)) {
                    combined |= e.second;
                    found = true;
                    break;
                }
            if (!found) return false;
        }
        if (last) break;
        start = comma + 1;
    }
    if (value) *value = combined;
    return true;
}
std::string pathMember(const std::string& path, const std::string& name) {
    const bool plain = !name.empty() && std::all_of(name.begin(), name.end(), [](unsigned char ch) { return std::isalnum(ch) || ch == '_' || ch == '$' || ch == '-' || ch == '+'; });
    return plain ? path + "." + name : path + "['" + name + "']";
}
std::string typeLabel(const SchemaType& t) {
    switch (t.kind) {
        case SchemaType::Kind::String: return "a string";
        case SchemaType::Kind::Bool: return "true / false";
        case SchemaType::Kind::UInt: return "a uint";
        case SchemaType::Kind::Int: return "an int";
        case SchemaType::Kind::ULong: return "a ulong";
        case SchemaType::Kind::Long: return "a long";
        case SchemaType::Kind::UShort: return "a ushort";
        case SchemaType::Kind::Short: return "a short";
        case SchemaType::Kind::Byte: return "a byte";
        case SchemaType::Kind::SByte: return "an sbyte";
        case SchemaType::Kind::Float:
        case SchemaType::Kind::Double: return "a number";
        case SchemaType::Kind::Json: return "any JSON";
        case SchemaType::Kind::Enum: return t.name;
        case SchemaType::Kind::Nullable: return typeLabel(*t.inner) + " or null";
        case SchemaType::Kind::List: return "a list";
        case SchemaType::Kind::Dict: return "an object (dictionary)";
        case SchemaType::Kind::Object: return t.name.substr(t.name.rfind('.') + 1);
        case SchemaType::Kind::Unsupported: return t.name;
    }
    return "?";
}
std::string cannotConvert(const std::string& path, const SchemaType& t) {
    return "The JSON value at " + path + " could not be converted to " + typeLabel(t) + ".";
}
std::string shortName(const std::string& key) { return key.substr(key.rfind('.') + 1); }

// The constructors that validate (run once the object's members are read). Values as the constructor gets them: absent = default.
std::string constructorError(const SchemaObject& o, const JsonValue& v) {
    const auto uints = [](const JsonValue* list, bool& isNull) {
        std::vector<int64_t> out;
        isNull = !list || !list->isArray();
        for (const JsonValue& x : elements(list)) out.push_back(num(&x));
        return out;
    };
    const auto distinctNonZero = [](const std::vector<int64_t>& ids) {
        std::set<int64_t> seen;
        for (int64_t id : ids)
            if (id == 0 || !seen.insert(id).second) return false;
        return true;
    };
    const std::string& k = o.key;
    if (k == "Rasa.Missions.Definitions.MissionHistoryAggregation") {
        const JsonValue* groups = mem(&v, "groups");
        if (!groups || !groups->isArray() || groups->items.empty())
            return "History aggregates require at least one mission group. (Parameter 'groups')";
        std::set<std::vector<int64_t>> seen;
        for (const JsonValue& g : groups->items) {
            bool isNull = false;
            std::vector<int64_t> ids = uints(&g, isNull);
            if (isNull || ids.empty() || !distinctNonZero(ids)) return "History groups require distinct non-zero mission IDs. (Parameter 'groups')";
            std::sort(ids.begin(), ids.end());
            if (!seen.insert(ids).second) return "History aggregates cannot repeat a mission group. (Parameter 'groups')";
        }
    } else if (k == "Rasa.Missions.Definitions.MissionObjectiveAggregation") {
        bool isNull = false;
        const std::vector<int64_t> ids = uints(mem(&v, "childObjectiveIds"), isNull);
        if (isNull || ids.empty() || !distinctNonZero(ids))
            return "Aggregates require distinct non-zero child objectives. (Parameter 'childObjectiveIds')";
        const int64_t target = num(mem(&v, "targetCount"));
        if (target == 0 || target > static_cast<int64_t>(ids.size()))
            return "Specified argument was out of the range of valid values. (Parameter 'targetCount')";
    } else if (k == "Rasa.Missions.Runtime.MissionCreditPolicy") {
        int64_t mode = 0;
        const JsonValue* m = mem(&v, "mode");
        if (present(m) && !m->integer(mode)) {
            const SchemaProperty* p = o.property("mode");
            if (!p || !enumText(m->text, *p->type, &mode)) mode = -1;
        }
        const JsonValue* r = mem(&v, "radius");
        const float radius = r ? std::strtof(r->text.c_str(), nullptr) : 0.0f;
        if (mode < 0 || mode > 2 || !std::isfinite(radius) || radius < 0 || (mode != 0 && radius == 0))
            return "Group credit needs an explicit positive finite radius.";
    } else if (k == "Rasa.Missions.Definitions.AuthoredLootProfile") {
        const JsonValue* drops = mem(&v, "drops");
        bool bad = !drops || !drops->isArray();
        for (const JsonValue& d : elements(drops)) {
            const int64_t chance = num(mem(&d, "chancePercent")), minimum = num(mem(&d, "minimum")), maximum = num(mem(&d, "maximum"));
            bad = bad || !d.isObject() || num(mem(&d, "templateId")) == 0 || chance < 0 || chance > 100 || minimum < 1 || maximum < minimum ||
                  maximum == kIntMax;
        }
        if (bad) return "Invalid authored loot distribution. (Parameter 'drops')";
    } else if (k == "Rasa.Missions.Definitions.MissionItemDropDefinition") {
        bool isNull = false;
        const std::vector<int64_t> ids = uints(mem(&v, "creatureIds"), isNull);
        const int64_t chance = num(mem(&v, "chancePercent"));
        if (isNull || ids.empty() || !distinctNonZero(ids) || num(mem(&v, "mapContextId")) == 0 || chance < 0 || chance > 100 ||
            num(mem(&v, "quantity")) == 0)
            return "Invalid mission item drop source, map, chance or quantity.";
    }
    return "";
}

std::string readAs(const JsonValue& v, const SchemaType& t, const std::string& path);

std::string readObject(const JsonValue& v, const SchemaObject& declared, const std::string& path) {
    const SchemaObject* o = &declared;
    size_t first = 0;
    if (!declared.discriminator.empty()) {
        const std::string& name = declared.discriminator;
        if (v.members.empty() || v.members.front().first != name)
            return "The JSON payload for polymorphic interface or abstract type '" + declared.key + "' must specify a type discriminator. Path: " + path;
        const JsonValue& id = v.members.front().second;
        if (!id.isString()) return "The '" + name + "' metadata property must be a JSON string. Path: " + pathMember(path, name);
        o = nullptr;
        for (const auto& d : declared.derived)
            if (d.first == id.text) o = d.second;
        if (!o) return "Read unrecognized type discriminator id '" + id.text + "'. Path: " + path;
        first = 1;
    } else if (declared.isAbstract)
        return "Deserialization of interface or abstract type '" + declared.key + "' is not supported. Path: " + path;
    std::set<std::string> seen;
    for (size_t i = first; i < v.members.size(); ++i) {
        const std::string& name = v.members[i].first;
        if (!declared.discriminator.empty() && !name.empty() && name[0] == '$') {
            if (name == declared.discriminator)
                return "Deserialized object contains a duplicate '" + name + "' metadata property. Path: " + pathMember(path, name);
            return "The metadata property is either not supported by the type or is not the first property in the deserialized JSON object. Path: " +
                   pathMember(path, name);
        }
        const SchemaProperty* p = o->property(name);
        if (!p) return "The JSON property '" + name + "' could not be mapped to any .NET member contained in type '" + o->key + "'.";
        seen.insert(name);
        if (!p->settable) continue;                                 // read-only: skipped unread
        if (std::string e = readAs(v.members[i].second, *p->type, pathMember(path, name)); !e.empty()) return e;
    }
    std::vector<std::string> missing;
    for (const SchemaProperty& p : o->properties)
        if (p.required && !seen.count(p.name)) missing.push_back("'" + p.name + "'");
    if (!missing.empty()) {
        if (missing.size() > 5) missing.resize(5);                  // the deserialiser lists at most five
        return "JSON deserialization for type '" + o->key + "' was missing required properties including: " + join(missing, ", ") + ".";
    }
    return constructorError(*o, v);
}

std::string readKey(const std::string& text, const SchemaType& t, const std::string& path) {
    bool ok = true;
    switch (t.kind) {
        case SchemaType::Kind::String: break;
        case SchemaType::Kind::UInt: ok = integerText(text, "", 0, 4294967295LL); break;
        case SchemaType::Kind::Int: ok = integerText(text, "+-", -2147483648LL, 2147483647LL); break;
        case SchemaType::Kind::Enum: ok = enumText(text, t, nullptr); break;
        default: ok = false;
    }
    return ok ? "" : "The dictionary key '" + text + "' at " + path + " could not be converted to " + typeLabel(t) + ".";
}

std::string readAs(const JsonValue& v, const SchemaType& t, const std::string& path) {
    using K = SchemaType::Kind;
    if (v.isNull()) {
        switch (t.kind) {
            case K::String: case K::Json: case K::Nullable: case K::List: case K::Dict: case K::Object: return "";
            default: return cannotConvert(path, t);
        }
    }
    bool ok = true;
    switch (t.kind) {
        case K::String: ok = v.isString(); break;
        case K::Bool: ok = v.type == JsonValue::Type::Bool; break;
        case K::UInt: ok = integerLiteral(v, 0, 4294967295ULL); break;
        case K::Int: ok = integerLiteral(v, -2147483648LL, 2147483647ULL); break;
        case K::ULong: ok = integerLiteral(v, 0, 18446744073709551615ULL); break;
        case K::Long: ok = integerLiteral(v, INT64_MIN, 9223372036854775807ULL); break;
        case K::UShort: ok = integerLiteral(v, 0, 65535); break;
        case K::Short: ok = integerLiteral(v, -32768, 32767); break;
        case K::Byte: ok = integerLiteral(v, 0, 255); break;
        case K::SByte: ok = integerLiteral(v, -128, 127); break;
        case K::Float: case K::Double: ok = v.isNumber(); break;
        case K::Json: break;
        case K::Enum: ok = v.isNumber() ? integerLiteral(v, -2147483648LL, 2147483647ULL) : v.isString() && enumText(v.text, t, nullptr); break;
        case K::Nullable: return readAs(v, *t.inner, path);
        case K::List:
            if (!v.isArray()) return cannotConvert(path, t);
            for (size_t i = 0; i < v.items.size(); ++i)
                if (std::string e = readAs(v.items[i], *t.inner, path + "[" + std::to_string(i) + "]"); !e.empty()) return e;
            return "";
        case K::Dict:
            if (!v.isObject()) return cannotConvert(path, t);
            for (const auto& m : v.members) {
                const std::string at = pathMember(path, m.first);
                if (std::string e = readKey(m.first, *t.key, at); !e.empty()) return e;
                if (std::string e = readAs(m.second, *t.inner, at); !e.empty()) return e;
            }
            return "";
        case K::Object:
            if (!v.isObject()) return cannotConvert(path, t);
            return readObject(v, *t.object, path);
        case K::Unsupported: return "";                             // a custom converter: not checked (none in the contract)
    }
    return ok ? "" : cannotConvert(path, t);
}

// "" when the deserialiser reads the document; "null" documents read as null (the callers report "no binding document").
std::string sceneReadError(const JsonValue& document, bool experience) {
    const SceneSchema& schema = sceneSchema();
    SchemaType root;
    root.kind = SchemaType::Kind::Object;
    root.object = experience ? schema.experience : schema.scene;
    root.name = root.object->key;
    return readAs(document, root, "$");
}

}  // namespace

std::string missionSceneReadError(const JsonValue& document, bool experience) { return sceneReadError(document, experience); }

MissionReferences MissionReferences::load(const WorldDatabase& database) {
    MissionReferences r;
    const auto each = [&](const char* table, const std::function<void(const Row&)>& f) {
        if (!database.hasTable(table)) return;
        const DbTable t = database.loadTable(table);
        for (const auto& row : t.rows) f(Row{&t, &row});
    };
    each("npc_package", [&](const Row& row) { r.npcPackageIds.insert(row.i("package_id")); });
    each("itemtemplate_itemclass", [&](const Row& row) { r.itemTemplateClasses[row.i("itemTemplateId")] = row.i("itemClassId"); });
    each("entityclass", [&](const Row& row) {
        r.entityClassIds.insert(row.i("id"));
        r.entityClassAugs[row.i("id")] = row.s("aug_list");
    });
    each("creature", [&](const Row& row) { r.creatureClasses[row.i("id")] = row.i("class_id"); });
    each("map_info", [&](const Row& row) { r.mapContextIds.insert(row.i("map_context_id")); });
    each("creature_class_flag", [&](const Row& row) { r.creatureFlagIds.insert(row.i("flag_id")); });
    each("npc_package", [&](const Row& row) { r.npcPackageRows.emplace(row.i("id"), row.i("package_id")); });
    each("spawnpool", [&](const Row& row) { r.spawnPoolIds.insert(row.i("id")); });
    each("module_class", [&](const Row& row) { r.moduleClasses.emplace(row.i("id"), std::make_pair(row.i("variant_id"), row.i("class_set_id"))); });
    each("modifiable_class", [&](const Row& row) { r.modifiableClasses.emplace(row.i("id"), row.i("class_set_id")); });
    each("itemtemplate", [&](const Row& row) { r.itemTemplateIds.insert(row.i("id")); });
    each("itemclass", [&](const Row& row) { r.itemClassStackSizes[row.i("id")] = row.i("stack_size"); });
    each("mission_experience_binding", [&](const Row& row) {
        if (row.i("enabled") != 0) r.experiences.push_back(MissionReferences::Experience{row.s("experience_key"), row.i("map_context_id"), row.s("bindings")});
    });
    return r;
}

MissionValidationResult validateMissionContent(const std::map<std::string, const DbTable*>& tables, const MissionReferences& references) {
    MissionValidationResult result;
    std::vector<MissionDiagnostic> startup;
    const auto fatal = [&](const std::string& message, int64_t mission = -1, const std::string& revision = "") {
        MissionDiagnostic d;
        d.code = "startup";
        d.message = message;
        d.missionId = mission;
        d.revision = revision;
        d.startup = true;
        startup.push_back(std::move(d));
    };
    const auto rowsOf = [&](const char* name) {
        std::vector<Row> rows;
        const auto it = tables.find(name);
        if (it == tables.end() || !it->second) return rows;
        for (const auto& r : it->second->rows) rows.push_back(Row{it->second, &r});
        return rows;
    };
    // Rows grouped by (mission, revision).
    const auto grouped = [&](const char* name) {
        std::map<std::string, std::vector<Row>> g;
        for (const Row& r : rowsOf(name)) g[keyOf(r.i("mission_id"), r.s("content_revision"))].push_back(r);
        return g;
    };

    // MissionContentCatalog: the enabled definitions, one per mission.
    std::map<int64_t, std::vector<Row>> enabled;
    for (const Row& r : rowsOf("mission_content_definition"))
        if (r.i("enabled") != 0) enabled[r.i("mission_id")].push_back(r);
    if (enabled.empty()) fatal("No enabled mission definitions. Apply the required World data migrations before starting Game.");
    for (const auto& e : enabled)
        if (e.second.size() != 1) fatal("Mission migrations enabled more than one definition for a mission.", e.first);
    result.enabledMissions = enabled.size();

    const auto objectives = grouped("mission_objective_definition"), transitions = grouped("mission_objective_transition"), triggers = grouped("mission_trigger"),
               actions = grouped("mission_action"), indicators = grouped("mission_indicator"), prerequisites = grouped("mission_prerequisite"),
               rewards = grouped("mission_reward_definition"), rewardItems = grouped("mission_reward_item"), areas = grouped("mission_area"),
               spawnGroups = grouped("mission_spawn_group"), spawns = grouped("mission_spawn"), scenarios = grouped("mission_scenario"),
               steps = grouped("mission_scenario_step"), channels = grouped("mission_channel_policy"), repeats = grouped("mission_repeat_policy"),
               scenes = grouped("mission_scene_binding");
    const auto in = [](const std::map<std::string, std::vector<Row>>& g, const std::string& key) -> const std::vector<Row>& {
        static const std::vector<Row> none;
        const auto it = g.find(key);
        return it == g.end() ? none : it->second;
    };

    // MissionContentLoader for each enabled mission.
    std::map<int64_t, Definition> definitions;
    for (const auto& e : enabled) {
        if (e.second.size() != 1) continue;
        Definition d;
        d.row = e.second.front();
        d.missionId = e.first;
        d.revision = d.row.s("content_revision");
        d.requirement = d.row.i("requirement");
        const std::string key = keyOf(d.missionId, d.revision);

        // Scene binding metadata (hidden objectives, existing facts, aggregates, object conversations, scripted scenarios).
        JsonValue scene;
        bool hasScene = false;
        for (const Row& s : in(scenes, key)) {
            std::string error;
            if (!parseJson(s.s("bindings"), scene, error)) {
                fatal("Mission " + std::to_string(d.missionId) + " scene binding is not readable JSON (" + error + ").", d.missionId, d.revision);
                d.sceneUnreadable = true;
                continue;
            }
            // MissionContentLoader: JsonSerializer.Deserialize<MissionSceneDefinition>(bindings, MissionContentCodec.Options) ?? throw.
            if (scene.isNull()) {
                fatal("Mission " + std::to_string(d.missionId) + " has no binding document.", d.missionId, d.revision);
                d.sceneUnreadable = true;
                continue;
            }
            if (std::string e = sceneReadError(scene, false); !e.empty()) {
                fatal("Mission " + std::to_string(d.missionId) + " scene binding cannot be read: " + e, d.missionId, d.revision);
                d.sceneUnreadable = true;
                continue;
            }
            hasScene = true;
            d.hasScene = true;
            d.scene = scene;
            d.sceneScriptKey = s.s("script_key");
            d.sceneStateVersion = s.i("state_version");
            // GetNativeObjectPackages
            if (const JsonValue* actors = scene.member("actors"))
                for (const auto& actor : actors->members) {
                    const JsonValue* conversation = actor.second.member("conversation");
                    if (!conversation || conversation->isNull()) continue;
                    const JsonValue* kind = actor.second.member("kind");
                    const JsonValue* templateId = actor.second.member("templateId");
                    const JsonValue* conversationMission = conversation->member("missionId");
                    int64_t cls = 0, mission = 0, package = 0;
                    const bool isObject = kind && kind->isString() && kind->text == "Object";
                    const bool hasClass = templateId && templateId->integer(cls) && references.entityClassIds.count(cls);
                    const auto aug = hasClass ? references.entityClassAugs.find(cls) : references.entityClassAugs.end();
                    static const std::regex npcAug(R"((?:^|\D)52(?:\D|$))");
                    const bool talks = aug != references.entityClassAugs.end() && std::regex_search(aug->second, npcAug);
                    if (!isObject || !conversationMission || !conversationMission->integer(mission) || mission != d.missionId || !talks)
                        fatal("Mission " + std::to_string(d.missionId) + ", object " + actor.first + ": conversation requires its native NPC-capable class.",
                              d.missionId, d.revision);
                    if (const JsonValue* p = conversation->member("npcPackageId"); p && p->integer(package)) d.objectNpcPackages.insert(package);
                }
            // GetTypedScenarioIds (the script registry is approximated: a binding whose script and state version agree with the row)
            const JsonValue* script = scene.member("script");
            const JsonValue* version = scene.member("stateVersion");
            int64_t versionValue = 0;
            if (script && script->isString() && !script->text.empty() && script->text == s.s("script_key") && version && version->integer(versionValue) &&
                versionValue == s.i("state_version")) {
                d.hasTypedScenarios = true;
                if (const JsonValue* sequences = scene.member("sequences"))
                    for (const auto& seq : sequences->members) {
                        bool nonEmpty = script->text != "data.sequence";
                        for (const char* list : {"world", "character", "signals", "timers"})
                            if (const JsonValue* l = seq.second.member(list); l && ((l->isArray() && !l->items.empty()) || (l->isObject() && !l->members.empty())))
                                nonEmpty = true;
                        if (nonEmpty) d.typedScenarios.insert(std::strtoll(seq.first.c_str(), nullptr, 10));
                    }
            }
        }
        const auto idList = [&](const char* name) {
            std::set<int64_t> ids;
            if (!hasScene) return ids;
            if (const JsonValue* list = scene.member(name))
                for (const JsonValue& v : list->items) {
                    int64_t id = 0;
                    if (v.integer(id)) ids.insert(id);
                }
            return ids;
        };
        const std::set<int64_t> hidden = idList("hiddenObjectiveIds"), existing = idList("existingFactObjectiveIds");

        // Transitions with their triggers and actions.
        for (const Row& t : in(transitions, key)) {
            Transition tr;
            tr.objective = t.i("objective_id");
            tr.id = t.i("transition_id");
            tr.sequence = t.i("sequence");
            const Opt to = t.o("to_state");
            if (validState(to)) tr.toState = to;
            for (const Row& g : in(triggers, key)) {
                if (g.i("objective_id") != tr.objective || g.i("transition_id") != tr.id) continue;
                Trigger x;
                x.id = g.i("trigger_id");
                x.kind = g.i("kind");
                x.eventKind = g.o("event_kind");
                x.subject = g.o("subject_id");
                x.counter = g.o("counter_id");
                x.initial = g.o("initial_value");
                x.target = g.o("target_value");
                x.area = g.o("area_id");
                x.duration = g.o("duration_seconds");
                x.npc = g.o("npc_package_id");
                x.flag = g.o("player_flag_id");
                x.relatedObjective = g.o("related_objective_id");
                x.relatedState = g.o("related_state");
                if (const Opt ssr = g.o("source_spawn_resolved")) x.sourceSpawnResolved = *ssr != 0;
                tr.triggers.push_back(x);
            }
            for (const Row& a : in(actions, key)) {
                if (a.i("objective_id") != tr.objective || a.i("transition_id") != tr.id) continue;
                Action x;
                x.id = a.i("action_id");
                x.kind = a.i("kind");
                x.targetObjective = a.o("target_objective_id");
                x.reward = a.o("reward_id");
                x.spawnGroup = a.o("spawn_group_id");
                x.scenario = a.o("scenario_id");
                x.indicator = a.o("indicator_id");
                x.flagId = a.o("player_flag_id");
                x.flagValue = a.o("player_flag_value");
                x.npc = a.o("npc_package_id");
                if (!a.isNull("item_intent")) {
                    // CharacterIntent: a "$kind" discriminator; the loader throws on anything it cannot read.
                    JsonValue intent;
                    std::string error;
                    static const std::set<std::string> kinds = {"reward", "ability", "qualification", "character-flag", "entitlement", "objective", "deadline",
                                                                "issue-mission-item", "consume-mission-item", "remove-mission-items", "offer-radio-mission",
                                                                "fail-related-mission"};
                    const JsonValue* k = nullptr;
                    if (!parseJson(a.s("item_intent"), intent, error))
                        fatal("Mission " + std::to_string(d.missionId) + " action " + std::to_string(x.id) + " item_intent is not readable JSON (" + error + ").",
                              d.missionId, d.revision);
                    else if (const std::string why = intentReadError(intent); !why.empty())
                        fatal("Mission " + std::to_string(d.missionId) + " action " + std::to_string(x.id) + " item_intent cannot be read: " + why + ".",
                              d.missionId, d.revision);
                    else if ((k = intent.member("$kind")) && kinds.count(k->text)) {
                        x.intentKind = k->text;
                        x.intent = intent;
                    }
                }
                tr.actions.push_back(x);
            }
            d.transitions.push_back(std::move(tr));
        }

        // Objectives (ordinal, then id), with the runtime analysis of their transitions.
        std::vector<Row> objectiveRows = in(objectives, key);
        std::stable_sort(objectiveRows.begin(), objectiveRows.end(), [](const Row& a, const Row& b) {
            return a.i("ordinal") != b.i("ordinal") ? a.i("ordinal") < b.i("ordinal") : a.i("objective_id") < b.i("objective_id");
        });
        for (const Row& r : objectiveRows) {
            Objective o;
            o.id = r.i("objective_id");
            if (d.objectives.count(o.id)) continue;
            o.ordinal = r.i("ordinal");
            const auto text = [&](const char* c) { const int64_t v = r.i(c); return v == 0 ? Opt() : Opt(v); };
            o.nameText = text("client_name_text_id");
            o.bodyText = text("client_body_text_id");
            o.counterText[0] = text("client_counter_0_text_id");
            o.counterText[1] = text("client_counter_1_text_id");
            o.counterText[2] = text("client_counter_2_text_id");
            const Opt state = r.o("initial_state");
            if (validState(state)) o.initialState = state;
            if (const Opt required = r.o("is_required")) o.required = *required != 0;
            o.visible = !hidden.count(o.id);
            o.recognizeExisting = existing.count(o.id) != 0;
            for (const Row& ind : in(indicators, key))
                if (ind.i("objective_id") == o.id) o.indicators.emplace_back(ind.i("indicator_id"), ind.od("radius").value_or(0));
            const auto aggregation = [&](const char* name, bool history) -> std::optional<Aggregation> {
                if (!hasScene) return std::nullopt;
                const JsonValue* map = scene.member(name);
                const JsonValue* entry = map ? map->member(std::to_string(o.id)) : nullptr;
                if (!entry || entry->isNull()) return std::nullopt;
                Aggregation a;
                if (history) {
                    if (const JsonValue* groups = entry->member("groups")) a.target = int64_t(groups->items.size());
                } else {
                    if (const JsonValue* children = entry->member("childObjectiveIds"))
                        for (const JsonValue& v : children->items) {
                            int64_t id = 0;
                            if (v.integer(id)) a.children.push_back(id);
                        }
                    if (const JsonValue* t = entry->member("targetCount")) t->integer(a.target);
                }
                int64_t counter = 0;
                if (const JsonValue* cv = entry->member("counterId"); cv && cv->integer(counter)) a.counter = counter;
                return a;
            };
            o.aggregation = aggregation("objectiveAggregations", false);
            o.history = aggregation("objectiveHistoryAggregations", true);
            std::vector<const Transition*> own;
            for (const Transition& t : d.transitions)
                if (t.objective == o.id) own.push_back(&t);
            analyzeObjective(d.missionId, o, own);
            if (o.zeroSubject)
                fatal("Mission " + std::to_string(d.missionId) + " objective " + std::to_string(o.id) + ": a progress rule has a zero subject (Progress rules require non-zero subjects).",
                      d.missionId, d.revision);
            // The aggregate counter joins the objective's counters (MissionObjectiveDefinition: a conflicting range throws).
            const Opt aggregateCounter = o.aggregation ? o.aggregation->counter : o.history ? o.history->counter : Opt();
            if (aggregateCounter) {
                const int64_t target = o.aggregation ? o.aggregation->target : o.history->target;
                const auto it = o.counters.find(*aggregateCounter);
                if (it != o.counters.end() && (it->second.initial != 0 || it->second.target != target))
                    fatal("Mission " + std::to_string(d.missionId) + " objective " + std::to_string(o.id) +
                              ": aggregate counter range conflicts with the objective counter.",
                          d.missionId, d.revision);
                o.counters[*aggregateCounter] = Counter{*aggregateCounter, 0, target};
            }
            d.objectives.emplace(o.id, std::move(o));
        }

        d.prerequisites = in(prerequisites, key);
        for (const Row& r : in(rewards, key)) {
            Reward w;
            w.id = r.i("reward_id");
            w.selection = r.i("selection_count");
            for (const Row& it : in(rewardItems, key)) {
                if (it.i("reward_id") != w.id) continue;
                const Reward::Item item{it.i("item_id"), it.i("item_template_id"), it.i("quantity"),
                                        it.i("module_1"), it.i("module_2"), it.i("module_3"), it.i("module_4")};
                if (it.i("kind") == 1) w.fixed.push_back(item);
                else if (it.i("kind") == 2) w.selectable.push_back(item);
            }
            d.rewards[w.id] = w;
        }
        for (const Row& r : in(areas, key)) {
            const int64_t id = r.i("area_id");
            if (!d.areas.count(id)) d.areas[id] = Area{id, r.i("map_context_id"), r.i("shape"), r.od("radius")};
        }
        for (const Row& r : in(spawnGroups, key)) {
            SpawnGroup g;
            g.id = r.i("spawn_group_id");
            g.map = r.i("map_context_id");
            g.policy = r.i("spawn_policy");
            g.area = r.o("area_id");
            g.respawn = r.o("respawn_seconds");
            for (const Row& s : in(spawns, key))
                if (s.i("spawn_group_id") == g.id) g.spawns.push_back(SpawnGroup::Spawn{s.i("spawn_id"), s.i("creature_id"), s.i("quantity")});
            d.spawnGroups[g.id] = g;
        }
        for (const Row& r : in(scenarios, key)) {
            Scenario s;
            s.id = r.i("scenario_id");
            for (const Row& st : in(steps, key))
                if (st.i("scenario_id") == s.id) s.steps.push_back(Step{st.i("step_id"), st.i("kind"), st});
            d.scenarios[s.id] = s;
        }
        for (const Row& r : in(channels, key)) {
            d.acceptance = r.i("acceptance_channel");
            d.completion = r.i("completion_channel");
            // JsonSerializer.Deserialize<MissionOfferSourceDefinition[]> with default options: PascalCase names, numeric enum.
            JsonValue sources;
            std::string error;
            if (!parseJson(r.s("radio_sources"), sources, error) || !(sources.isArray() || sources.isNull())) {
                fatal("Mission " + std::to_string(d.missionId) + " radio_sources is not a readable JSON array (" + (error.empty() ? "not an array" : error) + ").",
                      d.missionId, d.revision);
                continue;
            }
            for (const JsonValue& s : sources.items) {
                Definition::RadioSource rs;
                if (!s.isObject()) {
                    rs.valid = false;                    // a null source
                    d.radioSources.push_back(rs);
                    continue;
                }
                if (const JsonValue* k = s.member("Kind")) k->integer(rs.kind);
                if (const JsonValue* k = s.member("Key"); k && k->isString()) rs.key = k->text;
                int64_t map = 0;
                if (const JsonValue* m = s.member("MapContextId"); m && m->integer(map)) rs.map = map;
                if (const JsonValue* q = s.member("Requirement")) rs.requirement = *q;
                d.radioSources.push_back(rs);
            }
        }
        for (const Row& r : in(repeats, key)) {
            d.hasRepeat = true;
            d.repeatKind = r.i("repeat_kind");
            d.cooldown = r.o("cooldown_seconds");
            d.resetSecond = r.o("reset_second_utc");
        }
        definitions.emplace(d.missionId, std::move(d));
    }

    // MissionContentValidator.Validate
    std::vector<MissionDiagnostic> diagnostics;
    for (const auto& d : definitions) {
        Context c{references, diagnostics, &d.second};
        validateMission(c, definitions);
    }
    // BuildRequiredChainInactiveDiagnostics
    std::set<int64_t> inactive;
    for (const MissionDiagnostic& m : diagnostics) inactive.insert(m.missionId);
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& dp : definitions) {
            const Definition& d = dp.second;
            if (d.requirement != 1 || inactive.count(d.missionId)) continue;
            std::vector<std::pair<int64_t, int64_t>> blocked;
            for (const Row& p : d.prerequisites)
                if (const Opt required = p.o("required_mission_id"); required && inactive.count(*required)) blocked.emplace_back(p.i("prerequisite_id"), *required);
            if (blocked.empty()) continue;
            std::sort(blocked.begin(), blocked.end());
            for (const auto& b : blocked) {
                MissionDiagnostic m;
                m.code = "required-chain-inactive";
                m.message = "required prerequisite mission " + std::to_string(b.second) + " is inactive; fix that mission or mark the chain optional.";
                m.missionId = d.missionId;
                m.revision = d.revision;
                diagnostics.push_back(std::move(m));
            }
            inactive.insert(d.missionId);
            changed = true;
        }
    }
    // MissionContentCatalog: the scene bindings and experiences (start-up errors).
    validateCatalogScenes(definitions, inactive, references,
                          [&](const std::string& message, int64_t mission, const std::string& revision) { fatal(message, mission, revision); });
    std::stable_sort(diagnostics.begin(), diagnostics.end(), [](const MissionDiagnostic& a, const MissionDiagnostic& b) { return a.missionId < b.missionId; });
    for (const MissionDiagnostic& m : diagnostics) {
        const auto d = definitions.find(m.missionId);
        if (d != definitions.end() && d->second.requirement == 1) result.blocksReadiness = true;
    }
    result.inactiveMissions = inactive;
    result.startupFails = !startup.empty();
    result.diagnostics = std::move(startup);
    result.diagnostics.insert(result.diagnostics.end(), diagnostics.begin(), diagnostics.end());
    return result;
}
