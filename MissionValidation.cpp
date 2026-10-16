#include "MissionValidation.h"

#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <functional>
#include <optional>
#include <regex>
#include <stdexcept>

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
    struct Item { int64_t id = 0, templateId = 0, quantity = 0; };
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
    struct RadioSource { int64_t kind = -1; std::string key; Opt map; bool valid = true; };
    std::vector<RadioSource> radioSources;
    bool hasRepeat = false;
    int64_t repeatKind = 0;
    Opt cooldown, resetSecond;
    std::set<int64_t> objectNpcPackages;
    bool hasTypedScenarios = false;
    std::set<int64_t> typedScenarios;
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

std::string keyOf(int64_t mission, const std::string& revision) { return std::to_string(mission) + "\x1f" + revision; }

}  // namespace

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
                continue;
            }
            hasScene = true;
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
                    else if ((k = intent.member("$kind")) && kinds.count(k->text))
                        x.intentKind = k->text;
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
                const Reward::Item item{it.i("item_id"), it.i("item_template_id"), it.i("quantity")};
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
