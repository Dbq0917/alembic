#include <Alembic/AbcCoreAbstract/All.h>
#include <Alembic/AbcCoreOgawa/All.h>
#include <Alembic/Ogawa/All.h>
#include <Alembic/AbcCoreAbstract/Tests/Assert.h>

namespace AO = Alembic::AbcCoreOgawa;
namespace ABCA = Alembic::AbcCoreAbstract;

int main()
{
    const std::string fileName = "incrementalCompatibility.abc";

    {
        AO::WriteArchive writer;
        ABCA::MetaData metadata;
        metadata.set("source", "incremental-compatibility-test");
        ABCA::ArchiveWriterPtr archive =
            writer(fileName, metadata);
        TESTING_ASSERT(archive->getTop());
    }

    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        TESTING_ASSERT(archive.isValid());
        TESTING_ASSERT(archive.commit());
    }

    {
        AO::ReadArchive reader;
        ABCA::ArchiveReaderPtr archive = reader(fileName);
        TESTING_ASSERT(archive);
        ABCA::ObjectReaderPtr top = archive->getTop();
        TESTING_ASSERT(top);
        TESTING_ASSERT(top->getName() == "ABC");
        TESTING_ASSERT(top->getHeader().getMetaData().get("source") ==
                       "incremental-compatibility-test");
    }

    Alembic::Ogawa::ValidationResult validation =
        Alembic::Ogawa::ValidateArchive(fileName);
    TESTING_ASSERT(validation.valid);
    return 0;
}
