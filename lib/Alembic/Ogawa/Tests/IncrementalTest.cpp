#include <Alembic/Ogawa/All.h>
#include <Alembic/AbcCoreAbstract/Tests/Assert.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

const std::size_t kObjectCount = 10000;
const std::size_t kObjectSize = 64;

std::string objectValue(std::size_t iIndex)
{
    std::ostringstream strm;
    strm << "object-" << iIndex << "-";
    std::string value = strm.str();
    if (value.size() < kObjectSize)
    {
        value.append(kObjectSize - value.size(), 'x');
    }
    value.resize(kObjectSize);
    return value;
}

Alembic::Util::uint64_t fileSize(const std::string & iFileName)
{
    struct stat st;
    if (stat(iFileName.c_str(), &st) != 0)
    {
        return 0;
    }
    return static_cast<Alembic::Util::uint64_t>(st.st_size);
}

bool legacyVersionAccepts(const std::string & iFileName)
{
    std::ifstream file(iFileName.c_str(), std::ios_base::binary);
    char header[16] = {0};
    file.read(header, sizeof(header));
    if (std::string(header, 5) != "Ogawa")
    {
        return false;
    }
    Alembic::Util::uint16_t version =
        (static_cast<unsigned char>(header[6]) << 8) |
        static_cast<unsigned char>(header[7]);
    return version == 1;
}

void writeBaseArchive(const std::string & iFileName)
{
    Alembic::Ogawa::OArchive archive(iFileName);
    TESTING_ASSERT(archive.isValid());
    Alembic::Ogawa::OGroupPtr root = archive.getGroup();
    for (std::size_t i = 0; i < kObjectCount; ++i)
    {
        std::string value = objectValue(i);
        root->addData(value.size(), value.data());
    }
}

std::string readObject(const std::string & iFileName, std::size_t iIndex,
                       bool & oRecovered)
{
    Alembic::Ogawa::IArchive archive(iFileName);
    TESTING_ASSERT(archive.isValid());
    oRecovered = archive.wasRecovered();
    Alembic::Ogawa::IGroupPtr root = archive.getGroup();
    TESTING_ASSERT(root);
    TESTING_ASSERT(root->getNumChildren() == kObjectCount);
    Alembic::Ogawa::IDataPtr data = root->getData(iIndex, 0);
    TESTING_ASSERT(data);
    std::vector<char> buffer(data->getSize());
    data->read(data->getSize(), buffer.data(), 0, 0);
    return std::string(buffer.begin(), buffer.end());
}

void testIncrementalUpdate()
{
    const std::string fileName = "ogawaIncremental.ogawa";
    writeBaseArchive(fileName);

    const Alembic::Util::uint64_t before = fileSize(fileName);
    TESTING_ASSERT(legacyVersionAccepts(fileName));

    const std::size_t changedIndex = 4321;
    const std::string replacement = "material changed to brushed metal";
    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        TESTING_ASSERT(archive.isValid());
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::ODataPtr data =
            root->createData(replacement.size(), replacement.data());
        root->replaceData(changedIndex, data);
        TESTING_ASSERT(archive.commit());
        TESTING_ASSERT(!archive.hasPendingChanges());
    }

    const Alembic::Util::uint64_t after = fileSize(fileName);
    TESTING_ASSERT(after > before);
    TESTING_ASSERT(after - before < 256 * 1024);
    TESTING_ASSERT(!legacyVersionAccepts(fileName));

    bool recovered = false;
    TESTING_ASSERT(readObject(fileName, changedIndex, recovered) ==
                   replacement);
    TESTING_ASSERT(!recovered);
    TESTING_ASSERT(readObject(fileName, 0, recovered) == objectValue(0));
    TESTING_ASSERT(readObject(fileName, kObjectCount - 1, recovered) ==
                   objectValue(kObjectCount - 1));

    Alembic::Ogawa::ValidationResult validation =
        Alembic::Ogawa::ValidateArchive(fileName);
    TESTING_ASSERT(validation.valid);
    TESTING_ASSERT(!validation.recovered);
}

void testTailRecovery()
{
    const std::string fileName = "ogawaTailRecovery.ogawa";
    writeBaseArchive(fileName);

    const std::size_t changedIndex = 77;
    const std::string replacement = "first committed replacement";
    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::ODataPtr data =
            root->createData(replacement.size(), replacement.data());
        root->replaceData(changedIndex, data);
        TESTING_ASSERT(archive.commit());
    }

    const Alembic::Util::uint64_t committedSize = fileSize(fileName);
    {
        std::ofstream tail(fileName.c_str(),
                           std::ios_base::binary | std::ios_base::app);
        std::vector<char> garbage(4096, static_cast<char>(0x5a));
        tail.write(garbage.data(), garbage.size());
    }

    bool recovered = false;
    TESTING_ASSERT(readObject(fileName, changedIndex, recovered) ==
                   replacement);
    TESTING_ASSERT(recovered);

    Alembic::Ogawa::ValidationResult validation =
        Alembic::Ogawa::ValidateArchive(fileName);
    TESTING_ASSERT(validation.valid);
    TESTING_ASSERT(validation.recovered);

    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        TESTING_ASSERT(archive.isValid());
        TESTING_ASSERT(archive.wasRecovered());
        const std::size_t secondIndex = 78;
        const std::string secondValue = "recovered then committed";
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::ODataPtr data =
            root->createData(secondValue.size(), secondValue.data());
        root->replaceData(secondIndex, data);
        TESTING_ASSERT(archive.commit());
    }
    TESTING_ASSERT(fileSize(fileName) > committedSize);

    TESTING_ASSERT(readObject(fileName, changedIndex, recovered) ==
                   replacement);
    TESTING_ASSERT(readObject(fileName, 78, recovered) ==
                   "recovered then committed");
    TESTING_ASSERT(Alembic::Ogawa::ValidateArchive(fileName).valid);
}

void testTornCommitSlot()
{
    const std::string fileName = "ogawaTornSlot.ogawa";
    writeBaseArchive(fileName);

    const std::size_t changedIndex = 99;
    const std::string replacement = "committed before power loss";
    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::ODataPtr data =
            root->createData(replacement.size(), replacement.data());
        root->replaceData(changedIndex, data);
        TESTING_ASSERT(archive.commit());
    }

    Alembic::Ogawa::IArchive controlReader(fileName);
    TESTING_ASSERT(controlReader.isValid());

    std::fstream file(fileName.c_str(),
                      std::ios_base::binary | std::ios_base::in |
                      std::ios_base::out);
    char header[16] = {0};
    file.read(header, sizeof(header));
    Alembic::Util::uint64_t controlPos = 0;
    std::memcpy(&controlPos, header + 8, sizeof(controlPos));
    const std::streamoff liveSlot =
        static_cast<std::streamoff>(controlPos + 96 + 24);
    file.seekp(liveSlot);
    char badChecksum = 0x01;
    file.write(&badChecksum, 1);
    file.close();

    bool recovered = false;
    TESTING_ASSERT(readObject(fileName, changedIndex, recovered) ==
                   objectValue(changedIndex));
    TESTING_ASSERT(recovered);

    Alembic::Ogawa::OArchive repaired(
        fileName, Alembic::Ogawa::OArchive::kAppend);
    TESTING_ASSERT(repaired.isValid());
    TESTING_ASSERT(repaired.wasRecovered());
}

void testValidationDetectsCorruption()
{
    const std::string fileName = "ogawaCorrupt.ogawa";
    writeBaseArchive(fileName);

    const std::size_t changedIndex = 123;
    const std::string replacement = "new data protected by a checksum";
    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::ODataPtr data =
            root->createData(replacement.size(), replacement.data());
        root->replaceData(changedIndex, data);
        TESTING_ASSERT(archive.commit());
    }

    TESTING_ASSERT(Alembic::Ogawa::ValidateArchive(fileName).valid);

    std::fstream file(fileName.c_str(),
                      std::ios_base::binary | std::ios_base::in |
                      std::ios_base::out);
    file.seekp(32);
    char bad = '!';
    file.write(&bad, 1);
    file.close();

    Alembic::Ogawa::ValidationResult validation =
        Alembic::Ogawa::ValidateArchive(fileName);
    TESTING_ASSERT(!validation.valid);
}

void testNestedGroupUpdate()
{
    const std::string fileName = "ogawaNested.ogawa";
    {
        Alembic::Ogawa::OArchive archive(fileName);
        Alembic::Ogawa::OGroupPtr root = archive.getGroup();
        Alembic::Ogawa::OGroupPtr child = root->addGroup();
        child->addData(4, "old!");
        child->addData(4, "keep");
    }

    const std::string replacement = "nested replacement";
    {
        Alembic::Ogawa::OArchive archive(
            fileName, Alembic::Ogawa::OArchive::kAppend);
        Alembic::Ogawa::OGroupPtr child = archive.getGroup()->getGroup(0);
        TESTING_ASSERT(child);
        Alembic::Ogawa::ODataPtr data =
            child->createData(replacement.size(), replacement.data());
        child->replaceData(0, data);
        TESTING_ASSERT(archive.commit());
    }

    Alembic::Ogawa::IArchive archive(fileName);
    TESTING_ASSERT(archive.isValid());
    Alembic::Ogawa::IGroupPtr child = archive.getGroup()->getGroup(0, false, 0);
    TESTING_ASSERT(child);
    Alembic::Ogawa::IDataPtr changed = child->getData(0, 0);
    std::vector<char> changedData(changed->getSize());
    changed->read(changed->getSize(), changedData.data(), 0, 0);
    TESTING_ASSERT(std::string(changedData.begin(), changedData.end()) ==
                   replacement);
    Alembic::Ogawa::IDataPtr unchanged = child->getData(1, 0);
    char unchangedData[4] = {0};
    unchanged->read(4, unchangedData, 0, 0);
    TESTING_ASSERT(std::string(unchangedData, 4) == "keep");
    TESTING_ASSERT(Alembic::Ogawa::ValidateArchive(fileName).valid);
}

#ifndef _WIN32
void testConcurrentWriters()
{
    const std::string fileName = "ogawaConcurrent.ogawa";
    writeBaseArchive(fileName);

    const std::size_t indexA = 111;
    const std::size_t indexB = 8888;
    const std::string valueA = "writer A material";
    const std::string valueB = "writer B material";

    pid_t childA = fork();
    if (childA == 0)
    {
        try
        {
            Alembic::Ogawa::OArchive archive(
                fileName, Alembic::Ogawa::OArchive::kAppend);
            Alembic::Ogawa::OGroupPtr root = archive.getGroup();
            Alembic::Ogawa::ODataPtr data =
                root->createData(valueA.size(), valueA.data());
            root->replaceData(indexA, data);
            _exit(archive.commit() ? 0 : 1);
        }
        catch (...)
        {
            _exit(2);
        }
    }

    pid_t childB = fork();
    if (childB == 0)
    {
        try
        {
            Alembic::Ogawa::OArchive archive(
                fileName, Alembic::Ogawa::OArchive::kAppend);
            Alembic::Ogawa::OGroupPtr root = archive.getGroup();
            Alembic::Ogawa::ODataPtr data =
                root->createData(valueB.size(), valueB.data());
            root->replaceData(indexB, data);
            _exit(archive.commit() ? 0 : 1);
        }
        catch (...)
        {
            _exit(2);
        }
    }

    int statusA = -1;
    int statusB = -1;
    TESTING_ASSERT(waitpid(childA, &statusA, 0) == childA);
    TESTING_ASSERT(waitpid(childB, &statusB, 0) == childB);
    TESTING_ASSERT(WIFEXITED(statusA) && WEXITSTATUS(statusA) == 0);
    TESTING_ASSERT(WIFEXITED(statusB) && WEXITSTATUS(statusB) == 0);

    bool recovered = false;
    TESTING_ASSERT(readObject(fileName, indexA, recovered) == valueA);
    TESTING_ASSERT(readObject(fileName, indexB, recovered) == valueB);
    TESTING_ASSERT(readObject(fileName, 0, recovered) == objectValue(0));
    TESTING_ASSERT(Alembic::Ogawa::ValidateArchive(fileName).valid);
}
#endif

} // End anonymous namespace

int main(int argc, char *argv[])
{
    testIncrementalUpdate();
    testTailRecovery();
    testTornCommitSlot();
    testValidationDetectsCorruption();
    testNestedGroupUpdate();
#ifndef _WIN32
    testConcurrentWriters();
#endif
    return 0;
}
