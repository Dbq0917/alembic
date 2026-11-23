from io import StringIO

from sqlalchemy import create_engine
from sqlalchemy import inspect
from sqlalchemy import text

from alembic import command
from alembic import config
from alembic import util
from alembic.script import ScriptDirectory
from alembic.testing import assert_raises_message
from alembic.testing import eq_
from alembic.testing.env import _sqlite_testing_config
from alembic.testing.env import clear_staging_env
from alembic.testing.env import staging_env
from alembic.testing.env import write_script
from alembic.testing.fixtures import TestBase


class DowngradeCheckTest(TestBase):
    def setUp(self):
        staging_env()
        self.cfg = _sqlite_testing_config()
        self.script = ScriptDirectory.from_config(self.cfg)
        self.engine = create_engine(self.cfg.get_main_option("sqlalchemy.url"))

    def tearDown(self):
        self.engine.dispose()
        clear_staging_env()

    def _revision(self, downgrade_body):
        rev = util.rev_id()
        self.script.generate_revision(
            rev, "downgrade check", refresh=True, head="base"
        )
        write_script(
            self.script,
            rev,
            f"""\
revision = {rev!r}
down_revision = None

from alembic import op
import sqlalchemy as sa


def upgrade():
    op.create_table(
        "orders",
        sa.Column("id", sa.Integer, primary_key=True),
        sa.Column("code", sa.String(50)),
    )
    op.create_index("ix_orders_code", "orders", ["code"])


def downgrade():
{downgrade_body}
""",
        )

    def _drop_index(self):
        with self.engine.begin() as conn:
            conn.execute(text("DROP INDEX ix_orders_code"))

    def _has_table(self, name):
        with self.engine.connect() as conn:
            return inspect(conn).has_table(name)

    def test_missing_index_detected_and_nothing_is_executed(self):
        self._revision("""\
    op.drop_index("ix_orders_code", table_name="orders")
    op.drop_table("orders")
""")
        command.upgrade(self.cfg, "head")
        self._drop_index()

        assert_raises_message(
            util.DowngradeCheckDetected,
            r"index 'ix_orders_code' on table 'orders'",
            command.check,
            self.cfg,
            downgrade=True,
        )

        assert self._has_table("orders")
        with self.engine.connect() as conn:
            eq_(
                conn.execute(
                    text("select version_num from alembic_version")
                ).scalar(),
                self.script.get_current_head(),
            )

    def test_missing_index_guarded_by_if_exists_is_only_reported(self):
        self._revision("""\
    op.drop_index(
        "ix_orders_code", table_name="orders", if_exists=True
    )
    op.drop_table("orders")
""")
        command.upgrade(self.cfg, "head")
        self._drop_index()

        buf = StringIO()
        self.cfg.stdout = buf
        command.check(self.cfg, downgrade=True, downgrade_revision="base")

        assert (
            "1 missing object(s) are guarded by if_exists=True"
            in buf.getvalue()
        )
        assert self._has_table("orders")

    def test_manually_created_index_does_not_block_check(self):
        self._revision("""\
    op.drop_index("ix_orders_manual", table_name="orders")
    op.drop_table("orders")
""")
        command.upgrade(self.cfg, "head")
        with self.engine.begin() as conn:
            conn.execute(
                text("CREATE INDEX ix_orders_manual " "ON orders (code)")
            )

        command.check(self.cfg, downgrade=True)

        assert self._has_table("orders")

    def test_drop_table_before_drop_index_reports_missing_index(self):
        self._revision("""\
    op.drop_table("orders")
    op.drop_index("ix_orders_code", table_name="orders")
""")
        command.upgrade(self.cfg, "head")

        assert_raises_message(
            util.DowngradeCheckDetected,
            r"index 'ix_orders_code' on table 'orders'",
            command.check,
            self.cfg,
            downgrade=True,
        )

        assert self._has_table("orders")

    def test_raw_sql_is_not_executed(self):
        self._revision("""\
    op.execute("CREATE TABLE should_not_exist (id integer)")
    op.get_bind().execute(
        "CREATE TABLE also_should_not_exist (id integer)"
    )
""")
        command.upgrade(self.cfg, "head")

        command.check(self.cfg, downgrade=True)

        assert not self._has_table("should_not_exist")
        assert not self._has_table("also_should_not_exist")
        assert self._has_table("orders")

    def test_raw_sql_ddl_does_not_cause_false_missing(self):
        self._revision("""\
    op.execute("CREATE TABLE dynamic_table (id integer)")
    op.drop_table("dynamic_table")
""")
        command.upgrade(self.cfg, "head")

        buf = StringIO()
        self.cfg.stdout = buf
        command.check(self.cfg, downgrade=True)

        assert "could not be checked" in buf.getvalue()
        assert self._has_table("orders")

    def test_check_does_not_create_version_table(self):
        self._revision("""\
    op.drop_table("orders")
""")

        command.check(self.cfg, downgrade=True)

        assert not self._has_table("alembic_version")

    def test_cli_options(self):
        parser = config.CommandLine().parser

        options = parser.parse_args(
            ["check", "--downgrade", "--downgrade-revision=-1"]
        )

        assert options.downgrade is True
        eq_(options.downgrade_revision, "-1")
