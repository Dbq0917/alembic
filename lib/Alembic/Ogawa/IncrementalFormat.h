#ifndef Alembic_Ogawa_IncrementalFormat_h
#define Alembic_Ogawa_IncrementalFormat_h

#include <Alembic/Ogawa/Foundation.h>
#include <Alembic/Util/SpookyV2.h>

#include <functional>
#include <string>
#include <vector>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

const Alembic::Util::uint64_t OGAWA_V2_DATA_FLAG =
    0x8000000000000000ULL;
const Alembic::Util::uint64_t OGAWA_V2_RECORD_FLAG =
    0x4000000000000000ULL;
const Alembic::Util::uint64_t OGAWA_V2_REF_MASK =
    0x3fffffffffffffffULL;

const Alembic::Util::uint16_t OGAWA_V2_VERSION = 2;
const Alembic::Util::uint64_t OGAWA_V2_CONTROL_SIZE = 144;
const Alembic::Util::uint64_t OGAWA_V2_SLOT_OFFSET = 64;
const Alembic::Util::uint64_t OGAWA_V2_SLOT_SIZE = 32;
const char OGAWA_V2_CONTROL_MAGIC[8] =
    { 'O', 'G', 'W', '2', 'C', 'T', 'L', '\0' };

typedef std::function< bool ( Alembic::Util::uint64_t,
                              Alembic::Util::uint64_t, void * ) > ReadAtFn;

struct OgawaHeader
{
    bool frozen;
    Alembic::Util::uint16_t version;
    Alembic::Util::uint64_t groupPos;
};

struct OgawaV2Slot
{
    Alembic::Util::uint64_t sequence;
    Alembic::Util::uint64_t root;
    Alembic::Util::uint64_t committedEnd;
    Alembic::Util::uint64_t checksum;
    bool valid;
};

struct OgawaV2Control
{
    Alembic::Util::uint64_t position;
    Alembic::Util::uint64_t baseRoot;
    Alembic::Util::uint64_t baseEnd;
    Alembic::Util::uint64_t baseHash1;
    Alembic::Util::uint64_t baseHash2;
    OgawaV2Slot slots[2];
    int selectedSlot;
};

bool ReadOgawaHeader( const ReadAtFn & iReadAt,
                      Alembic::Util::uint64_t iFileSize,
                      OgawaHeader & oHeader,
                      std::string & oError );

bool ReadOgawaV2Control( const ReadAtFn & iReadAt,
                         Alembic::Util::uint64_t iFileSize,
                         Alembic::Util::uint64_t iControlPos,
                         OgawaV2Control & oControl,
                         std::string & oError );

Alembic::Util::uint64_t HashV2Slot( const OgawaV2Slot & iSlot );

Alembic::Util::uint64_t HashV2DataRecord(
    Alembic::Util::uint64_t iSize, const void * iData );

Alembic::Util::uint64_t HashV2GroupRecord(
    Alembic::Util::uint64_t iCount,
    const Alembic::Util::uint64_t * iChildren );

bool ValidateV2GroupRecord( const ReadAtFn & iReadAt,
                            Alembic::Util::uint64_t iRef,
                            Alembic::Util::uint64_t iCommittedEnd,
                            std::string & oError );

bool ValidateV2DataRecord( const ReadAtFn & iReadAt,
                           Alembic::Util::uint64_t iRef,
                           Alembic::Util::uint64_t iCommittedEnd,
                           std::string & oError );

} // End namespace ALEMBIC_VERSION_NS

using namespace ALEMBIC_VERSION_NS;

} // End namespace Ogawa
} // End namespace Alembic

#endif
