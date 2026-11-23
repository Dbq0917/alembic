.. change::
    :tags: feature, mysql

    Added support for MySQL / MariaDB "online" DDL clauses when adding a
    column.  :meth:`.Operations.add_column` now accepts
    :paramref:`.AddColumnOp.mysql_algorithm` and
    :paramref:`.AddColumnOp.mysql_lock` parameters which render
    ``ALGORITHM=<value>`` and ``LOCK=<value>`` clauses as part of the
    ``ALTER TABLE`` statement, e.g. ``ALGORITHM=INPLACE, LOCK=NONE``,
    allowing a column to be added to a very large table without blocking
    concurrent reads and writes; the statement fails immediately if the
    server cannot honor the requested algorithm or lock level, rather
    than silently falling back to a locking table copy.  The clauses may
    also be enabled environment-wide for all migrations by passing
    ``mysql_algorithm`` / ``mysql_lock`` to
    :meth:`.EnvironmentContext.configure`, so that existing migration
    scripts run online without modification; per-operation values take
    precedence over the environment-wide setting.
