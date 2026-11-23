"""Exercise the modules' actual SQL strings and migrations against an isolated MySQL fixture.

Opt in with CQ_TEST_MYSQL=<absolute path to mysql.exe>. The client only uses
named pipe cq_lifecycle_audit_fixture, never TCP or option files. Put the paired
legacy worktree next to this one, or point CQ_TEST_LEGACY at it.
"""
import ast
import os
from pathlib import Path
import re
import subprocess
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
LEGACY = Path(os.getenv('CQ_TEST_LEGACY', str(ROOT.parent / 'legacy')))
MYSQL = os.getenv('CQ_TEST_MYSQL')


def strings(cpp):
    return ''.join(ast.literal_eval(token) for token in re.findall(r'"(?:[^"\\]|\\.)*"', cpp))


def function(src, name):
    return src.split(name, 1)[1].split('\n    }', 1)[0]


def foreign(src, guid):
    code = function(src, 'std::string ForeignCustodyClause(')
    tables = re.findall(r'"([a-z_]+)"', code.split('for (char const* table : {', 1)[1].split('}', 1)[0])
    template = strings(code.split('sql += fmt::format(', 1)[1].split(', table, guid', 1)[0])
    return ''.join(template.format(table, guid) for table in tables)


def free_clause(src):
    code = function(src, 'std::string FreeCustodyClause(')
    format_code = code.split('+ fmt::format(', 1)[1]
    # Resolve the C++ class constant with Wrath's Death Knight class ID.
    bindings = (6, 0, 0) if 'CLASS_DEATH_KNIGHT' in format_code else (0, 0)
    return (strings(code.split('return ', 1)[1].split('ForeignCustodyClause(', 1)[0]) + foreign(src, 'c.guid')
            + strings(format_code.split(',', 1)[0]).format(*bindings))


def current_clause(src):
    code = function(src, 'std::string CurrentCustodyClause(')
    before = code.split('ForeignCustodyClause(', 1)[0]
    if '(tracked ?' in before:
        before = before.split('(tracked ?', 1)[0] + '" AND x.tracking = 1 "'
    return strings(before) + foreign(src, 'b.bot_guid')


def claim(src, owner, bot, name, clazz=1):
    code = src.split('auto trans = CharacterDatabase.BeginTransaction();', 1)[1].split('CharacterDatabase.DirectCommitTransaction', 1)[0]
    update, row_count, insert = re.findall(r'trans->Append\((.*?);', code, re.S)
    before, after = update.split('FreeCustodyClause()', 1)
    update_sql = (strings(before) + free_clause(src) + strings(after)).format(
        owner, name, name, name, bot, '100', 1, clazz, owner, 4)
    return ('START TRANSACTION;\n' + update_sql + ';\n' + strings(row_count) + ';\n'
            + strings(insert).format(owner, bot, name) + ';\nCOMMIT;')


def retirement_request(src, marker, owner, bot):
    code = function(src, marker).split('CharacterDatabase.DirectExecute(', 1)[1]
    return strings(code.split('ForeignCustodyClause', 1)[0]).format(owner, bot) + foreign(src, 'x.bot_guid') + ';'


@unittest.skipUnless(MYSQL, 'set CQ_TEST_MYSQL for isolated MySQL integration tests')
class MySQLLifecycle(unittest.TestCase):
    def setUp(self):
        self.db = 'cq_lifecycle_fixture_' + uuid.uuid4().hex
        self.cq = (ROOT / 'src/mod_companion_questsync.cpp').read_text(encoding='utf-8')
        self.legacy = (LEGACY / 'src/wowlegends_companion.cpp').read_text(encoding='utf-8')
        self.run_sql(f'CREATE DATABASE {self.db};', use_db=False)
        self.addCleanup(self.run_sql, f'DROP DATABASE {self.db};', use_db=False)
        self.run_sql((ROOT / 'sql/companion-custody.sql').read_text() + '''
            CREATE TABLE characters (guid INT PRIMARY KEY, account INT, name VARCHAR(24), race INT, class INT,
                online INT, level INT, at_login INT) ENGINE=InnoDB;
            CREATE TABLE guild_member (guid INT PRIMARY KEY) ENGINE=InnoDB;
            CREATE TABLE group_member (memberGuid INT PRIMARY KEY) ENGINE=InnoDB;
            CREATE TABLE cq_bindings (owner_guid INT, bot_guid INT PRIMARY KEY,
                companion_name VARCHAR(24) UNIQUE, created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP) ENGINE=InnoDB;
            CREATE TABLE wowlegends_companion (owner_guid INT PRIMARY KEY, bot_guid INT UNIQUE,
                companion_name VARCHAR(24) UNIQUE) ENGINE=InnoDB;
            CREATE TABLE wowlegends_companion_multi LIKE cq_bindings;
            CREATE TABLE wowlegends_companion_released (bot_guid INT PRIMARY KEY, released_at TIMESTAMP) ENGINE=InnoDB;
            INSERT INTO characters VALUES
                (1,900,'Owner',1,1,1,12,0), (2,901,'Other',1,1,1,12,0),
                (10,100,'Blankcq',1,1,0,1,0), (11,100,'Blankwl',1,1,0,1,0),
                (12,100,'Unvetted',1,1,0,1,0);
            INSERT INTO cq_companion_custody (bot_guid,module,body_account,body_name,state,reviewed_by) VALUES
                (10,'cq',100,'Blankcq','free','fixture'), (11,'legacy',100,'Blankwl','free','fixture');
        ''')

    def run_sql(self, sql, use_db=True, fails=False):
        cmd = [MYSQL, '--no-defaults', '--protocol=PIPE', '--socket=cq_lifecycle_audit_fixture',
               '--user=root', '--batch', '--skip-column-names']
        if use_db:
            cmd.append(self.db)
        out = subprocess.run(cmd, input=sql, text=True, capture_output=True, timeout=15)
        if fails:
            self.assertNotEqual(out.returncode, 0, out.stdout)
            return out.stderr
        self.assertEqual(out.returncode, 0, out.stderr)
        return out.stdout.strip()

    def test_cross_module_pool_partitions_and_unverified_bodies(self):
        self.run_sql(claim(self.legacy, 2, 10, 'Foreign'))
        self.run_sql(claim(self.cq, 1, 11, 'Foreign'))
        self.run_sql(claim(self.cq, 1, 12, 'Unknown'))
        self.assertEqual(self.run_sql('SELECT COUNT(*) FROM cq_companion_custody WHERE state != "free"'), '0')
        self.run_sql('INSERT INTO wowlegends_companion VALUES (2,10,"Reserved");')
        self.run_sql(claim(self.cq, 1, 10, 'Cross'))
        self.assertEqual(self.run_sql('SELECT name FROM characters WHERE guid = 10'), 'Blankcq')
        self.run_sql('INSERT INTO wowlegends_companion_multi (owner_guid, bot_guid, companion_name) VALUES (1,11,"OldCQ");')
        self.run_sql(claim(self.legacy, 1, 11, 'Cross'))
        self.assertEqual(self.run_sql('SELECT name FROM characters WHERE guid = 11'), 'Blankwl')

    def test_current_claim_survives_same_second_history_and_untracking(self):
        self.run_sql(claim(self.cq, 1, 10, 'Current'))
        self.run_sql('INSERT INTO wowlegends_companion_released SELECT bot_guid,created_at FROM cq_bindings;')
        roster = 'SELECT b.bot_guid' + current_clause(self.cq) + ' AND b.owner_guid=1'
        self.assertEqual(self.run_sql(roster), '10')
        self.run_sql('UPDATE cq_companion_custody SET tracking=0 WHERE bot_guid=10;')
        self.assertEqual(self.run_sql(roster), '')
        self.run_sql(claim(self.legacy, 2, 10, 'Cross'))
        self.assertEqual(self.run_sql('SELECT owner_guid,name FROM cq_bindings JOIN characters ON guid=bot_guid'), '1\tCurrent')

    def test_heroic_bodies_are_neither_stock_nor_claimable_even_at_level_one(self):
        self.run_sql('''
            INSERT INTO characters VALUES (20,100,'Cqnormaldk',1,6,0,55,0), (21,100,'Cqlevelonedk',1,6,0,1,0),
                (22,100,'Wlnormaldk',1,6,0,55,0), (23,100,'Wllevelonedk',1,6,0,1,0);
            INSERT INTO cq_companion_custody (bot_guid,module,body_account,body_name,state,reviewed_by) VALUES
                (20,'cq',100,'Cqnormaldk','free','fixture'), (21,'cq',100,'Cqlevelonedk','free','fixture'),
                (22,'legacy',100,'Wlnormaldk','free','fixture'), (23,'legacy',100,'Wllevelonedk','free','fixture');
        ''')
        stock = 'SELECT c.guid FROM characters c JOIN cq_companion_custody x ON x.bot_guid = c.guid WHERE c.account IN (100)'
        for src, owner, ordinary, heroic in ((self.cq, 1, 10, (20, 21)), (self.legacy, 2, 11, (22, 23))):
            with self.subTest(owner=owner):
                self.assertEqual(self.run_sql(stock + free_clause(src)), str(ordinary))
                for bot in heroic:
                    self.run_sql(claim(src, owner, bot, 'Heroic', clazz=6))
                    self.assertEqual(self.run_sql(f'SELECT state FROM cq_companion_custody WHERE bot_guid={bot}'), 'free')
                self.run_sql(claim(src, owner, ordinary, 'Ordinary' + str(owner)))
                self.assertEqual(self.run_sql(f'SELECT state FROM cq_companion_custody WHERE bot_guid={ordinary}'), 'owned')

    def test_insertion_failure_rolls_back_rename_and_custody(self):
        self.run_sql('INSERT INTO cq_bindings (owner_guid,bot_guid,companion_name) VALUES (2,99,"Collision");')
        error = self.run_sql(claim(self.cq, 1, 10, 'Collision'), fails=True)
        self.assertIn('Duplicate entry', error)
        self.assertEqual(self.run_sql('SELECT x.state,c.name FROM cq_companion_custody x JOIN characters c ON c.guid=x.bot_guid WHERE x.bot_guid=10'), 'free\tBlankcq')
        self.run_sql(claim(self.legacy, 2, 11, 'Valid'))
        self.assertEqual(self.run_sql('SELECT b.bot_guid' + current_clause(self.legacy)), '11')

    def test_offline_retirement_rolls_back_on_conflict_and_never_recycles(self):
        self.run_sql(claim(self.cq, 1, 10, 'Current'))
        self.run_sql(retirement_request(self.cq, 'bool MarkCompanionRetiring(', 1, 10))
        self.run_sql('DROP TABLE wowlegends_companion_multi;')
        self.run_sql((ROOT / 'sql/companion-retirement-maintenance.sql').read_text())
        call = "CALL cq_finalize_companion_retirement('cq',10,1,100,'Current');"
        self.run_sql('INSERT INTO wowlegends_companion VALUES (2,10,"Reserved");')
        self.assertIn('Conflicting legacy', self.run_sql(call, fails=True))
        self.assertEqual(self.run_sql('SELECT state FROM cq_companion_custody WHERE bot_guid=10'), 'retiring')
        self.run_sql('DELETE FROM wowlegends_companion;')
        self.run_sql(call)
        self.assertEqual(self.run_sql('SELECT state,name FROM cq_companion_custody JOIN characters ON guid=bot_guid WHERE bot_guid=10'), 'retired\tCurrent')
        self.assertEqual(self.run_sql('SELECT COUNT(*) FROM cq_bindings WHERE bot_guid=10'), '0')

    def test_wrong_owner_and_changed_body_cannot_enter_retirement(self):
        self.run_sql(claim(self.legacy, 2, 11, 'Current'))
        request = 'static bool RequestRetirement('
        self.run_sql(retirement_request(self.legacy, request, 1, 11))
        self.assertEqual(self.run_sql('SELECT state FROM cq_companion_custody WHERE bot_guid=11'), 'owned')
        self.run_sql('UPDATE characters SET account=101 WHERE guid=11;')
        self.run_sql(retirement_request(self.legacy, request, 2, 11))
        self.assertEqual(self.run_sql('SELECT state FROM cq_companion_custody WHERE bot_guid=11'), 'owned')
        self.run_sql('UPDATE characters SET account=100 WHERE guid=11;')
        self.run_sql(retirement_request(self.legacy, request, 2, 11))
        self.assertEqual(self.run_sql('SELECT state FROM cq_companion_custody WHERE bot_guid=11'), 'retiring')
        self.assertEqual(self.run_sql('SELECT bot_guid FROM wowlegends_companion WHERE owner_guid=2'), '11')

    def test_free_seed_with_party_membership_cannot_be_claimed(self):
        self.run_sql('INSERT INTO group_member VALUES (10),(11);')
        self.run_sql(claim(self.cq, 1, 10, 'NotClaimedcq'))
        self.run_sql(claim(self.legacy, 2, 11, 'NotClaimedwl'))
        self.assertEqual(self.run_sql('SELECT COUNT(*) FROM cq_companion_custody WHERE state="owned"'), '0')
