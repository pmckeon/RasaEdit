"""Inject mission-content defects into a World database copy, one per mission (defects.py <source.db> <copy.db>), for comparing the
server's mission checks (viewport_test RASA_MISSIONCHECK) with Rasa.NET's own validator (mvharness)."""
import shutil, sqlite3, sys

src, dst = sys.argv[1], sys.argv[2]
shutil.copyfile(src, dst)
c = sqlite3.connect(dst)
c.row_factory = sqlite3.Row
enabled = [(r['mission_id'], r['content_revision']) for r in c.execute('select mission_id, content_revision from mission_content_definition where enabled=1 order by mission_id')]
used = set()

def pick(sql):
    """the first enabled mission (not used yet) the query returns a row for; the query gets ?1 mission, ?2 revision"""
    for m, r in enabled:
        if m in used: continue
        row = c.execute(sql, (m, r)).fetchone()
        if row is not None:
            used.add(m)
            return m, r, row
    raise SystemExit('no mission for: ' + sql)

applied = []
def defect(name, sql, change):
    m, r, row = pick(sql)
    try:
        change(m, r, row)
    except sqlite3.IntegrityError as e:
        print(f'{m:6d} {name}: refused by the database ({e})')
        return
    applied.append((name, m))

K = 'mission_id=?1 and content_revision=?2'
defect('objective name text 0', f'select objective_id from mission_objective_definition where {K}',
       lambda m, r, row: c.execute(f'update mission_objective_definition set client_name_text_id=0 where {K} and objective_id=?3', (m, r, row[0])))
defect('conversation package missing', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=1',
       lambda m, r, row: c.execute(f'update mission_trigger set npc_package_id=999999 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('area trigger area missing', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=4',
       lambda m, r, row: c.execute(f'update mission_trigger set area_id=9999 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('progress event kind unknown', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=2',
       lambda m, r, row: c.execute(f'update mission_trigger set event_kind=99 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('progress subject 0', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=2 and event_kind=2',
       lambda m, r, row: c.execute(f'update mission_trigger set subject_id=0 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('kill missing creature', f'select objective_id, transition_id, trigger_id from mission_trigger t where {K} and kind=2 and event_kind=2 and (select count(*) from mission_trigger u where u.mission_id=t.mission_id and u.content_revision=t.content_revision and u.objective_id=t.objective_id and u.transition_id=t.transition_id)=1',
       lambda m, r, row: c.execute(f'update mission_trigger set subject_id=999999 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('counter target not above initial', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=2 and counter_id is not null and initial_value is not null and target_value is not null',
       lambda m, r, row: c.execute(f'update mission_trigger set target_value=initial_value where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
defect('mixed trigger kinds', f'select objective_id, transition_id from mission_trigger where {K} and kind=2',
       lambda m, r, row: c.execute(f"insert into mission_trigger (mission_id, content_revision, objective_id, transition_id, trigger_id, requirement, kind, sequence, area_id, comment) values (?1, ?2, ?3, ?4, 90, 1, 4, 90, 1, 'defect')", (m, r, *row)))
defect('reveal target missing', f'select objective_id, transition_id, action_id from mission_action where {K} and kind=1',
       lambda m, r, row: c.execute(f'update mission_action set target_objective_id=999 where {K} and objective_id=?3 and transition_id=?4 and action_id=?5', (m, r, *row)))
defect('grant reward missing', f'select objective_id, transition_id, action_id from mission_action where {K} and kind=4',
       lambda m, r, row: c.execute(f'update mission_action set reward_id=999 where {K} and objective_id=?3 and transition_id=?4 and action_id=?5', (m, r, *row)))
defect('set flag reserved id', f'select objective_id, transition_id, action_id from mission_action where {K} and kind=8',
       lambda m, r, row: c.execute(f'update mission_action set player_flag_id=3000000000 where {K} and objective_id=?3 and transition_id=?4 and action_id=?5', (m, r, *row)))
defect('item intent of another kind', f'select objective_id, transition_id, action_id from mission_action where {K} and kind=10',
       lambda m, r, row: c.execute('update mission_action set item_intent=?6 where ' + K + ' and objective_id=?3 and transition_id=?4 and action_id=?5', (m, r, *row, '{"$kind":"consume-mission-item","missionId":%d,"itemKey":"x","quantity":1,"scope":"AssignmentIssued","operationKey":"defect"}' % m)))
defect('action kind unknown', f'select objective_id, transition_id, action_id from mission_action where {K} and kind=3',
       lambda m, r, row: c.execute(f'update mission_action set kind=77 where {K} and objective_id=?3 and transition_id=?4 and action_id=?5', (m, r, *row)))
defect('reward item template missing', f'select reward_id, item_id from mission_reward_item where {K}',
       lambda m, r, row: c.execute(f'update mission_reward_item set item_template_id=99999999 where {K} and reward_id=?3 and item_id=?4', (m, r, *row)))
defect('reward item quantity 0', f'select reward_id, item_id from mission_reward_item where {K}',
       lambda m, r, row: c.execute(f'update mission_reward_item set quantity=0 where {K} and reward_id=?3 and item_id=?4', (m, r, *row)))
defect('reward selection without selectables', f'select reward_id from mission_reward_definition where {K} and selection_count=0',
       lambda m, r, row: c.execute(f'update mission_reward_definition set selection_count=1 where {K} and reward_id=?3', (m, r, row[0])))
defect('sphere area radius 0', f'select area_id from mission_area where {K} and shape=1',
       lambda m, r, row: c.execute(f'update mission_area set radius=0 where {K} and area_id=?3', (m, r, row[0])))
defect('spawn group map missing', f'select spawn_group_id from mission_spawn_group where {K}',
       lambda m, r, row: c.execute(f'update mission_spawn_group set map_context_id=9999 where {K} and spawn_group_id=?3', (m, r, row[0])))
defect('spawn quantity 0', f'select spawn_group_id, spawn_id from mission_spawn where {K}',
       lambda m, r, row: c.execute(f'update mission_spawn set quantity=0 where {K} and spawn_group_id=?3 and spawn_id=?4', (m, r, *row)))
defect('scenario step kind unknown', f'select scenario_id, step_id from mission_scenario_step where {K}',
       lambda m, r, row: c.execute(f'update mission_scenario_step set kind=99 where {K} and scenario_id=?3 and step_id=?4', (m, r, *row)))
defect('step missing spawn group', f'select scenario_id, step_id from mission_scenario_step where {K} and kind in (1,2)',
       lambda m, r, row: c.execute(f'update mission_scenario_step set spawn_group_id=999 where {K} and scenario_id=?3 and step_id=?4', (m, r, *row)))
defect('mission name text 0', f'select 1 from mission_content_definition where {K}',
       lambda m, r, row: c.execute(f'update mission_content_definition set client_name_text_id=0 where {K}', (m, r)))
defect('all objectives inactive', f'select 1 from mission_objective_definition where {K}',
       lambda m, r, row: c.execute(f'update mission_objective_definition set initial_state=4 where {K}', (m, r)))
defect('repeat cooldown 0', f'select 1 from mission_repeat_policy where {K}',
       lambda m, r, row: c.execute(f'update mission_repeat_policy set repeat_kind=2, cooldown_seconds=0, reset_second_utc=null where {K}', (m, r)))
defect('radio acceptance without sources', f"select 1 from mission_channel_policy where {K} and acceptance_channel=2",
       lambda m, r, row: c.execute(f'update mission_channel_policy set acceptance_channel=1 where {K}', (m, r)))
defect('objective counter text missing', f'select objective_id from mission_objective_definition o where {K} and client_counter_0_text_id is not null and exists (select 1 from mission_trigger t where t.mission_id=o.mission_id and t.content_revision=o.content_revision and t.objective_id=o.objective_id and t.counter_id=0)',
       lambda m, r, row: c.execute(f'update mission_objective_definition set client_counter_0_text_id=null where {K} and objective_id=?3', (m, r, row[0])))
defect('indicator radius 0', f'select objective_id, indicator_id from mission_indicator where {K}',
       lambda m, r, row: c.execute(f'update mission_indicator set radius=0 where {K} and objective_id=?3 and indicator_id=?4', (m, r, *row)))
defect('timer duration 0', f'select objective_id, transition_id, trigger_id from mission_trigger where {K} and kind=5',
       lambda m, r, row: c.execute(f'update mission_trigger set duration_seconds=0 where {K} and objective_id=?3 and transition_id=?4 and trigger_id=?5', (m, r, *row)))
# Reward item modules (MissionContentValidator.ValidateRewardModules, development b22b4a4d; worlds with module_1..module_4 only).
if any(col[1] == 'module_1' for col in c.execute('pragma table_info(mission_reward_item)')):
    MODIFIABLE = (f'select i.reward_id, i.item_id, m.class_set_id from mission_reward_item i join itemtemplate_itemclass t on t.itemTemplateId=i.item_template_id '
                  f'join modifiable_class m on m.id=t.itemClassId where i.{K.replace(" and ", " and i.")} and m.class_set_id=212')
    def modules(m, r, row, ids):
        c.execute(f'update mission_reward_item set module_1=?5, module_2=?6, module_3=?7, module_4=?8 where {K} and reward_id=?3 and item_id=?4',
                  (m, r, row[0], row[1], *ids))
    armor = [row[0] for row in c.execute('select id from module_class where class_set_id=212 and variant_id=4 order by id limit 2')]
    weapon = c.execute('select id from module_class where class_set_id<>212 and class_set_id<>0 order by id limit 1').fetchone()[0]
    defect('reward module slot gap', MODIFIABLE, lambda m, r, row: modules(m, r, row, (0, armor[0], 0, 0)))
    defect('reward module missing', MODIFIABLE, lambda m, r, row: modules(m, r, row, (999999, 0, 0, 0)))
    defect('reward module wrong kind', MODIFIABLE, lambda m, r, row: modules(m, r, row, (weapon, 0, 0, 0)))
    defect('reward module duplicate variant', MODIFIABLE, lambda m, r, row: modules(m, r, row, (armor[0], armor[1], 0, 0)))
    defect('reward item takes no modules',
           f'select i.reward_id, i.item_id from mission_reward_item i join itemtemplate_itemclass t on t.itemTemplateId=i.item_template_id '
           f'where i.{K.replace(" and ", " and i.")} and t.itemClassId not in (select id from modifiable_class)',
           lambda m, r, row: modules(m, r, row, (armor[0], 0, 0, 0)))
c.commit()
for name, m in applied: print(f'{m:6d} {name}')
