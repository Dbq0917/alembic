# Ogawa incremental archives

Ogawa keeps its v1 full-write format unchanged. Existing callers can keep
using `OArchive(filename)` and `IArchive(filename)` without changes, and those
files remain readable by older Alembic releases.

Incremental updates use a v2 append-only commit layer:

```cpp
{
    Alembic::Ogawa::OArchive archive(
        filename, Alembic::Ogawa::OArchive::kAppend);

    Alembic::Ogawa::OGroupPtr root = archive.getGroup();
    Alembic::Ogawa::OGroupPtr material = root->getGroup(materialIndex);
    Alembic::Ogawa::ODataPtr data =
        material->createData(newValue.size(), newValue.data());
    material->replaceData(valueIndex, data);

    archive.commit();
}
```

`OArchive::kAppend` does not rewrite the base archive. It writes the changed
payload, copy-on-write groups along the path to the root, and a commit record.
Unchanged data and groups continue to reference their existing offsets.

## Format

- A v1 archive remains a normal v1 archive until the first incremental commit.
- The first commit appends a fixed v2 control block and changes the header
  version to 2. Older readers reject version 2 instead of silently reading an
  older root.
- The control block records the immutable v1 base region, its checksum, and
  two commit slots.
- Each commit writes and flushes its data and metadata before publishing an
  A/B commit slot. Slots contain a sequence number, root reference, committed
  file length, and checksum. A torn latest slot falls back to the previous
  slot.
- New data and group records carry their own checksums. The validator also
  checks the checksum of the immutable base region.
- Updates use a lock around the read-apply-publish section. A writer re-reads
  the latest root while holding the lock, so concurrent writers changing
  different objects are merged instead of overwriting each other's commits.

## Recovery and validation

`IArchive` detects an incomplete tail while opening the archive and exposes
the result through `IArchive::wasRecovered()`. It reads the latest valid
commit slot, so callers never observe a half-written transaction.

Opening with `OArchive::kAppend` performs recovery immediately: it selects the
latest valid slot and truncates an incomplete tail before accepting changes.
No manual repair step is required.

`Alembic::Ogawa::ValidateArchive(path)` performs an offline structural and
checksum validation of the complete reachable object graph. The installed
`abcvalidate` command is the command-line wrapper:

```sh
abcvalidate asset.abc
```

It exits nonzero on corruption and reports when a valid archive was recovered
to its last committed state.
