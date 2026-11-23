from __future__ import annotations

import re
from typing import Any
from typing import TYPE_CHECKING

from sqlalchemy import inspect
from sqlalchemy import schema as sa_schema
from sqlalchemy.sql.elements import TextClause

from ..ddl import base as ddl_base
from ..util import exc

if TYPE_CHECKING:
    from sqlalchemy.engine import Connection
    from sqlalchemy.engine import Inspector
    from sqlalchemy.sql import Executable


_UNKNOWN = object()
_DDL_STATEMENT = re.compile(
    r"\b(?:create|alter|drop|rename|truncate)\b", re.IGNORECASE
)


class _TableState:
    __slots__ = ("columns", "indexes", "constraints")

    def __init__(
        self,
        columns: set[str],
        indexes: set[str],
        constraints: set[str],
    ) -> None:
        self.columns = columns
        self.indexes = indexes
        self.constraints = constraints


def _name(value: Any) -> str:
    return str(value)


def _table_key(table_name: str, schema: str | None) -> tuple[str | None, str]:
    return (
        str(schema) if schema is not None else None,
        _name(table_name),
    )


class DowngradeChecker:
    """Read-only preflight checker for downgrade operations.

    Migration functions are invoked normally, but all SQL execution is
    intercepted by :class:`.DefaultImpl` and passed to this object.  The
    checker keeps a simulated view of the schema, seeded from the live
    database, and reports objects that a downgrade would try to drop even
    though they do not exist at that point.

    Extra objects in the database (for example an index that was added
    manually) are ignored.  Objects guarded with ``if_exists=True`` are also
    not treated as failures; they are recorded as skipped instead.

    """

    def __init__(self) -> None:
        self.connection: Connection | None = None
        self._inspector: Inspector | None = None
        self._tables: dict[
            tuple[str | None, str], _TableState | None | object
        ] = {}
        self._missing: list[str] = []
        self._skipped: list[str] = []
        self._unchecked: list[str] = []
        self._schema_unknown = False

    @property
    def missing(self) -> list[str]:
        return list(self._missing)

    @property
    def skipped(self) -> list[str]:
        return list(self._skipped)

    @property
    def unchecked(self) -> list[str]:
        return list(self._unchecked)

    @property
    def inspector(self) -> Inspector:
        if self._inspector is None:
            if self.connection is None:
                raise exc.CommandError(
                    "Downgrade check requires an online database connection; "
                    "the configured environment did not provide one"
                )
            self._inspector = inspect(self.connection)
        return self._inspector

    def _report(self, description: str, construct: Any) -> None:
        if getattr(construct, "if_exists", False):
            if description not in self._skipped:
                self._skipped.append(description)
        elif self._schema_unknown:
            self._unknown(
                "%s (schema may have been changed by unchecked SQL)"
                % description
            )
        elif description not in self._missing:
            self._missing.append(description)

    def _unknown(self, description: str) -> None:
        if description not in self._unchecked:
            self._unchecked.append(description)

    def _get_table(
        self, table_name: str, schema: str | None
    ) -> _TableState | None | object:
        key = _table_key(table_name, schema)
        if key in self._tables:
            return self._tables[key]

        try:
            if not self.inspector.has_table(key[1], key[0]):
                state: _TableState | None = None
            else:
                state = _TableState(
                    {
                        _name(column["name"])
                        for column in self.inspector.get_columns(
                            key[1], schema=key[0]
                        )
                    },
                    {
                        _name(index["name"])
                        for index in self.inspector.get_indexes(
                            key[1], schema=key[0]
                        )
                        if index.get("name") is not None
                    },
                    self._constraint_names(key[1], key[0]),
                )
        except Exception as err:
            self._unknown(
                "table %s (reflection failed: %s)"
                % (self._table_description(key[1], key[0]), err)
            )
            state = _UNKNOWN

        self._tables[key] = state
        return state

    def _constraint_names(
        self, table_name: str, schema: str | None
    ) -> set[str]:
        names: set[str] = set()
        methods = (
            self.inspector.get_pk_constraint,
            self.inspector.get_foreign_keys,
            self.inspector.get_unique_constraints,
            self.inspector.get_check_constraints,
        )
        for method in methods:
            try:
                result = method(table_name, schema=schema)
            except Exception:
                continue
            if isinstance(result, dict):
                result = [result]
            for constraint in result:
                if constraint.get("name") is not None:
                    names.add(_name(constraint["name"]))
        return names

    def _table_description(self, table_name: str, schema: str | None) -> str:
        if schema is None:
            return "table %r" % table_name
        else:
            return "table %r in schema %r" % (table_name, schema)

    def _index_description(
        self,
        index_name: str,
        table_name: str | None,
        schema: str | None,
    ) -> str:
        if table_name is None:
            return "index %r" % index_name
        elif schema is None:
            return "index %r on table %r" % (index_name, table_name)
        else:
            return "index %r on table %r in schema %r" % (
                index_name,
                table_name,
                schema,
            )

    def _column_description(
        self, column_name: str, table_name: str, schema: str | None
    ) -> str:
        if schema is None:
            return "column %r on table %r" % (column_name, table_name)
        else:
            return "column %r on table %r in schema %r" % (
                column_name,
                table_name,
                schema,
            )

    @staticmethod
    def _constraint_name(constraint: Any) -> str | None:
        name = getattr(constraint, "name", None)
        return _name(name) if name is not None else None

    @staticmethod
    def _constraint_table(constraint: Any) -> Any:
        table = getattr(constraint, "table", None)
        if table is not None:
            return table
        return getattr(constraint, "parent", None)

    def _create_table(self, table: Any) -> None:
        key = _table_key(table.name, table.schema)
        self._tables[key] = _TableState(
            {_name(column.name) for column in table.columns},
            {
                _name(index.name)
                for index in table.indexes
                if index.name is not None
            },
            {
                _name(constraint.name)
                for constraint in table.constraints
                if constraint.name is not None
            },
        )

    def _drop_table(self, construct: Any) -> None:
        table = construct.element
        key = _table_key(table.name, table.schema)
        state = self._get_table(table.name, table.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(
                self._table_description(table.name, table.schema), construct
            )
        else:
            self._tables[key] = None

    def _create_index(self, index: Any) -> None:
        if index.table is None:
            return
        state = self._get_table(index.table.name, index.table.schema)
        if isinstance(state, _TableState) and index.name is not None:
            state.indexes.add(_name(index.name))

    def _drop_index(self, construct: Any) -> None:
        index = construct.element
        if index.table is None or index.name is None:
            self._unknown("index %r without a table or name" % index.name)
            return

        table_name = index.table.name
        schema = index.table.schema
        description = self._index_description(
            _name(index.name), table_name, schema
        )
        state = self._get_table(table_name, schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(description, construct)
        elif index.name is None or _name(index.name) not in state.indexes:
            self._report(description, construct)
        else:
            state.indexes.discard(_name(index.name))

    def _add_column(self, construct: Any) -> None:
        state = self._get_table(construct.table_name, construct.schema)
        if isinstance(state, _TableState):
            state.columns.add(_name(construct.column.name))

    def _drop_column(self, construct: Any) -> None:
        description = self._column_description(
            _name(construct.column.name),
            construct.table_name,
            construct.schema,
        )
        state = self._get_table(construct.table_name, construct.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(description, construct)
        elif _name(construct.column.name) not in state.columns:
            self._report(description, construct)
        else:
            state.columns.discard(_name(construct.column.name))

    def _alter_column(self, construct: Any) -> None:
        description = self._column_description(
            _name(construct.column_name),
            construct.table_name,
            construct.schema,
        )
        state = self._get_table(construct.table_name, construct.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(description, construct)
        elif _name(construct.column_name) not in state.columns:
            self._report(description, construct)
        elif isinstance(construct, ddl_base.ColumnName):
            state.columns.discard(_name(construct.column_name))
            state.columns.add(_name(construct.newname))

    def _rename_table(self, construct: Any) -> None:
        old_key = _table_key(construct.table_name, construct.schema)
        state = self._get_table(construct.table_name, construct.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(
                self._table_description(
                    construct.table_name, construct.schema
                ),
                construct,
            )
        else:
            self._tables[old_key] = None
            self._tables[
                _table_key(construct.new_table_name, construct.schema)
            ] = state

    def _add_constraint(self, const: Any) -> None:
        table = self._constraint_table(const)
        name = self._constraint_name(const)
        if table is None or name is None:
            return
        state = self._get_table(table.name, table.schema)
        if isinstance(state, _TableState):
            state.constraints.add(name)

    def _drop_constraint(self, construct: Any) -> None:
        const = construct.element
        table = self._constraint_table(const)
        name = self._constraint_name(const)
        if table is None or name is None:
            self._unknown(
                "unnamed constraint %r" % getattr(const, "name", None)
            )
            return

        description = "constraint %r on table %r" % (name, table.name)
        if table.schema is not None:
            description += " in schema %r" % table.schema

        state = self._get_table(table.name, table.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(description, construct)
        elif name not in state.constraints:
            self._report(description, construct)
        else:
            state.constraints.discard(name)

    def _table_comment(self, construct: Any) -> None:
        table = construct.element
        if table is None:
            self._unknown(
                "table comment operation %s" % construct.__class__.__name__
            )
            return
        state = self._get_table(table.name, table.schema)
        if state is _UNKNOWN:
            return
        elif state is None:
            self._report(
                self._table_description(table.name, table.schema), construct
            )

    def process(self, construct: Executable | str) -> None:
        """Inspect a single DDL construct without executing it."""

        if isinstance(construct, sa_schema.CreateTable):
            self._create_table(construct.element)
        elif isinstance(construct, sa_schema.DropTable):
            self._drop_table(construct)
        elif isinstance(construct, sa_schema.CreateIndex):
            self._create_index(construct.element)
        elif isinstance(construct, sa_schema.DropIndex):
            self._drop_index(construct)
        elif isinstance(construct, ddl_base.AddColumn):
            self._add_column(construct)
        elif isinstance(construct, ddl_base.DropColumn):
            self._drop_column(construct)
        elif isinstance(construct, ddl_base.ColumnName):
            self._alter_column(construct)
        elif isinstance(construct, ddl_base.AlterColumn):
            self._alter_column(construct)
        elif isinstance(construct, ddl_base.RenameTable):
            self._rename_table(construct)
        elif isinstance(construct, sa_schema.AddConstraint):
            self._add_constraint(construct.element)
        elif isinstance(construct, sa_schema.DropConstraint):
            self._drop_constraint(construct)
        elif isinstance(construct, sa_schema.SetTableComment):
            self._table_comment(construct)
        elif isinstance(construct, sa_schema.DropTableComment):
            self._table_comment(construct)
        elif isinstance(construct, TextClause):
            statement = str(construct)
            if _DDL_STATEMENT.search(statement):
                self._schema_unknown = True
            self._unknown("SQL statement %r" % statement)
        elif isinstance(construct, sa_schema.DDLElement):
            self._schema_unknown = True
            self._unknown("DDL construct %s" % construct.__class__.__name__)
        elif isinstance(construct, str):
            if _DDL_STATEMENT.search(construct):
                self._schema_unknown = True
            self._unknown("SQL statement %r" % construct)
        else:
            # DML such as version-table updates or bulk inserts does not
            # affect the schema objects being checked.
            return

    def raise_if_missing(self) -> None:
        if self.connection is None:
            raise exc.CommandError(
                "Downgrade check requires an online database connection; "
                "the configured environment did not provide one"
            )
        if self._missing:
            message = "Downgrade check detected missing schema objects: %s" % (
                "; ".join(self._missing),
            )
            if self._skipped:
                message += (
                    "; %d guarded drop(s) referenced missing objects and "
                    "would be skipped by if_exists=True" % len(self._skipped)
                )
            if self._unchecked:
                message += "; %d operation(s) could not be checked" % len(
                    self._unchecked
                )
            raise exc.DowngradeCheckDetected(
                message,
                missing=self._missing,
                skipped=self._skipped,
                unchecked=self._unchecked,
            )
