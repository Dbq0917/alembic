.. change::
    :tags: feature, commands

    Added the ``alembic converge`` command, which resolves a "multiple
    heads" condition, such as the one produced when two source branches
    each added migrations in parallel, by generating a single merge-point
    revision that joins all current heads.  Convergence only adds a new
    revision file and never modifies existing migration history; on a
    subsequent upgrade, revisions that were already applied are not
    re-run, and the merge point can be downgraded to return to the
    individual branch heads.  If the script directory already has a
    single head, the command is a no-op.
