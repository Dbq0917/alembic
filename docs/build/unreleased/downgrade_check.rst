.. change::
    :tags: feature, commands

    Added a read-only downgrade preflight check, available as
    ``alembic check --downgrade``.  The check invokes the downgrade path but
    intercepts DDL execution, comparing each drop or alter operation against
    the live schema.  Missing tables, indexes, columns and constraints are
    reported before a real downgrade can leave the database at an
    intermediate revision.  Objects that already exist, including indexes
    that were created outside of migrations, do not cause the check to fail;
    missing objects guarded with ``if_exists=True`` are reported as skipped.
    The target revision can be selected with ``--downgrade-revision`` and
    defaults to ``base``.
