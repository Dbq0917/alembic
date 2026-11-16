#include <Alembic/Ogawa/IncrementalFormat.h>

#include <algorithm>
#include <cstring>
#include <sstream>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

namespace {

const Alembic::Util::uint64_t CONTROL_HASH_SEED =
    0x6f67617761326374ULL;
const Alembic::Util::uint64_t SLOT_HASH_SEED =
    0x6f6761776132736cULL;
const Alembic::Util::uint64_t GROUP_HASH_SEED =
    0x6f67617761326772ULL;
const Alembic::Util::uint64_t DATA_HASH_SEED =
    0x6f67617761326474ULL;

void PutU64( char * oBuf, Alembic::Util::uint64_t iValue )
{
    for ( std::size_t i = 0; i < 8; ++i )
    {
        oBuf[i] = static_cast< char >( ( iValue >> ( i * 8 ) ) & 0xff );
    }
}

Alembic::Util::uint64_t GetU64( const char * iBuf )
{
    Alembic::Util::uint64_t value = 0;
    for ( std::size_t i = 0; i < 8; ++i )
    {
        value |= static_cast< Alembic::Util::uint64_t >(
            static_cast< unsigned char >( iBuf[i] ) ) << ( i * 8 );
    }
    return value;
}

Alembic::Util::uint32_t GetU32( const char * iBuf )
{
    Alembic::Util::uint32_t value = 0;
    for ( std::size_t i = 0; i < 4; ++i )
    {
        value |= static_cast< Alembic::Util::uint32_t >(
            static_cast< unsigned char >( iBuf[i] ) ) << ( i * 8 );
    }
    return value;
}

Alembic::Util::uint64_t HashBuffer(
    Alembic::Util::uint64_t iSeed, const void * iData,
    std::size_t iSize )
{
    return Alembic::Util::SpookyHash::Hash64( iData, iSize, iSeed );
}

} // End anonymous namespace

bool ReadOgawaHeader( const ReadAtFn & iReadAt,
                      Alembic::Util::uint64_t iFileSize,
                      OgawaHeader & oHeader,
                      std::string & oError )
{
    oHeader.frozen = false;
    oHeader.version = 0;
    oHeader.groupPos = 0;

    if ( iFileSize < 16 )
    {
        oError = "Ogawa file is smaller than the archive header.";
        return false;
    }

    char header[16] = { 0 };
    if ( !iReadAt( 0, 16, header ) )
    {
        oError = "Could not read the Ogawa archive header.";
        return false;
    }

    if ( std::memcmp( header, "Ogawa", 5 ) != 0 )
    {
        oError = "Not an Ogawa archive.";
        return false;
    }

    oHeader.frozen = ( header[5] == static_cast< char >( 0xff ) );
    oHeader.version = static_cast< Alembic::Util::uint16_t >(
        ( static_cast< unsigned char >( header[6] ) << 8 ) |
        static_cast< unsigned char >( header[7] ) );
    oHeader.groupPos = GetU64( header + 8 );
    return true;
}

Alembic::Util::uint64_t HashV2Slot( const OgawaV2Slot & iSlot )
{
    char data[24];
    PutU64( data, iSlot.sequence );
    PutU64( data + 8, iSlot.root );
    PutU64( data + 16, iSlot.committedEnd );
    return HashBuffer( SLOT_HASH_SEED, data, sizeof( data ) );
}

Alembic::Util::uint64_t HashV2DataRecord(
    Alembic::Util::uint64_t iSize, const void * iData )
{
    Alembic::Util::SpookyHash hash;
    hash.Init( DATA_HASH_SEED, DATA_HASH_SEED );
    char size[8];
    PutU64( size, iSize );
    hash.Update( size, sizeof( size ) );
    if ( iSize > 0 && iData )
    {
        hash.Update( iData, static_cast< std::size_t >( iSize ) );
    }

    Alembic::Util::uint64_t h1 = 0;
    Alembic::Util::uint64_t h2 = 0;
    hash.Final( &h1, &h2 );
    return h1;
}

Alembic::Util::uint64_t HashV2GroupRecord(
    Alembic::Util::uint64_t iCount,
    const Alembic::Util::uint64_t * iChildren )
{
    Alembic::Util::SpookyHash hash;
    hash.Init( GROUP_HASH_SEED, GROUP_HASH_SEED );
    char count[8];
    PutU64( count, iCount );
    hash.Update( count, sizeof( count ) );
    if ( iCount > 0 && iChildren )
    {
        hash.Update( iChildren, static_cast< std::size_t >( iCount * 8 ) );
    }

    Alembic::Util::uint64_t h1 = 0;
    Alembic::Util::uint64_t h2 = 0;
    hash.Final( &h1, &h2 );
    return h1;
}

bool ReadOgawaV2Control( const ReadAtFn & iReadAt,
                         Alembic::Util::uint64_t iFileSize,
                         Alembic::Util::uint64_t iControlPos,
                         OgawaV2Control & oControl,
                         std::string & oError )
{
    oControl.selectedSlot = -1;

    if ( iControlPos > iFileSize ||
         iControlPos + OGAWA_V2_CONTROL_SIZE > iFileSize )
    {
        oError = "Ogawa v2 control block is outside the file.";
        return false;
    }

    std::vector< char > control( OGAWA_V2_CONTROL_SIZE );
    if ( !iReadAt( iControlPos, control.size(), &control[0] ) )
    {
        oError = "Could not read the Ogawa v2 control block.";
        return false;
    }

    if ( std::memcmp( &control[0], OGAWA_V2_CONTROL_MAGIC, 8 ) != 0 )
    {
        oError = "Invalid Ogawa v2 control block magic.";
        return false;
    }

    if ( GetU32( &control[8] ) != 1 ||
         GetU32( &control[12] ) != OGAWA_V2_CONTROL_SIZE )
    {
        oError = "Unsupported Ogawa v2 control block version.";
        return false;
    }

    Alembic::Util::uint64_t storedControlHash = GetU64( &control[128] );
    Alembic::Util::uint64_t computedControlHash =
        HashBuffer( CONTROL_HASH_SEED, &control[0], 64 );
    if ( storedControlHash != computedControlHash )
    {
        oError = "Ogawa v2 control block checksum mismatch.";
        return false;
    }

    oControl.position = iControlPos;
    oControl.baseRoot = GetU64( &control[16] );
    oControl.baseEnd = GetU64( &control[24] );
    oControl.baseHash1 = GetU64( &control[32] );
    oControl.baseHash2 = GetU64( &control[40] );

    if ( oControl.baseEnd != iControlPos )
    {
        oError = "Ogawa v2 base region does not end at the control block.";
        return false;
    }

    Alembic::Util::uint64_t maxSequence = 0;
    for ( int i = 0; i < 2; ++i )
    {
        const char * slotData = &control[OGAWA_V2_SLOT_OFFSET +
                                       i * OGAWA_V2_SLOT_SIZE];
        OgawaV2Slot & slot = oControl.slots[i];
        slot.sequence = GetU64( slotData );
        slot.root = GetU64( slotData + 8 );
        slot.committedEnd = GetU64( slotData + 16 );
        slot.checksum = GetU64( slotData + 24 );
        slot.valid = slot.sequence > 0 &&
            slot.committedEnd >= iControlPos + OGAWA_V2_CONTROL_SIZE &&
            slot.committedEnd <= iFileSize &&
            slot.checksum == HashV2Slot( slot );

        if ( slot.valid )
        {
            std::string rootError;
            slot.valid = ValidateV2GroupRecord( iReadAt, slot.root,
                                                slot.committedEnd,
                                                rootError );
        }

        if ( slot.valid && slot.sequence > maxSequence )
        {
            maxSequence = slot.sequence;
            oControl.selectedSlot = i;
        }
    }

    if ( oControl.selectedSlot < 0 )
    {
        oError = "No valid Ogawa v2 commit slot was found.";
        return false;
    }

    return true;
}

bool ValidateV2GroupRecord( const ReadAtFn & iReadAt,
                            Alembic::Util::uint64_t iRef,
                            Alembic::Util::uint64_t iCommittedEnd,
                            std::string & oError )
{
    if ( iRef == EMPTY_GROUP )
    {
        return true;
    }

    if ( ( iRef & OGAWA_V2_DATA_FLAG ) != 0 )
    {
        oError = "Expected a group reference.";
        return false;
    }

    Alembic::Util::uint64_t pos = iRef & OGAWA_V2_REF_MASK;
    bool checked = ( iRef & OGAWA_V2_RECORD_FLAG ) != 0;
    if ( pos < 16 || pos + 8 > iCommittedEnd )
    {
        oError = "Ogawa v2 group reference is outside the committed file.";
        return false;
    }

    char countData[8];
    if ( !iReadAt( pos, 8, countData ) )
    {
        oError = "Could not read an Ogawa v2 group record.";
        return false;
    }

    Alembic::Util::uint64_t count = GetU64( countData );
    Alembic::Util::uint64_t checksumSize = checked ? 8 : 0;
    if ( count > ( iCommittedEnd - pos - 8 - checksumSize ) / 8 )
    {
        oError = "Ogawa v2 group child count exceeds the committed file.";
        return false;
    }

    std::vector< Alembic::Util::uint64_t > children(
        static_cast< std::size_t >( count ) );
    if ( count > 0 )
    {
        std::vector< char > childData(
            static_cast< std::size_t >( count * 8 ) );
        if ( !iReadAt( pos + 8, childData.size(), &childData[0] ) )
        {
            oError = "Could not read Ogawa v2 group children.";
            return false;
        }
        for ( std::size_t i = 0; i < children.size(); ++i )
        {
            children[i] = GetU64( &childData[i * 8] );
        }
    }

    if ( checked )
    {
        char checksumData[8];
        if ( !iReadAt( pos + 8 + count * 8, 8, checksumData ) )
        {
            oError = "Could not read the Ogawa v2 group checksum.";
            return false;
        }
        Alembic::Util::uint64_t expected = GetU64( checksumData );
        Alembic::Util::uint64_t actual = HashV2GroupRecord(
            count, count > 0 ? &children[0] : NULL );
        if ( expected != actual )
        {
            std::ostringstream msg;
            msg << "Ogawa v2 group checksum mismatch at " << pos << ".";
            oError = msg.str();
            return false;
        }
    }

    return true;
}

bool ValidateV2DataRecord( const ReadAtFn & iReadAt,
                           Alembic::Util::uint64_t iRef,
                           Alembic::Util::uint64_t iCommittedEnd,
                           std::string & oError )
{
    if ( iRef == EMPTY_DATA )
    {
        return true;
    }

    if ( ( iRef & OGAWA_V2_DATA_FLAG ) == 0 )
    {
        oError = "Expected a data reference.";
        return false;
    }

    Alembic::Util::uint64_t pos = iRef & OGAWA_V2_REF_MASK;
    bool checked = ( iRef & OGAWA_V2_RECORD_FLAG ) != 0;
    if ( pos < 16 || pos + 8 > iCommittedEnd )
    {
        oError = "Ogawa v2 data reference is outside the committed file.";
        return false;
    }

    char sizeData[8];
    if ( !iReadAt( pos, 8, sizeData ) )
    {
        oError = "Could not read an Ogawa v2 data record.";
        return false;
    }

    Alembic::Util::uint64_t size = GetU64( sizeData );
    Alembic::Util::uint64_t overhead = checked ? 8 : 0;
    if ( size > iCommittedEnd - pos - 8 - overhead )
    {
        oError = "Ogawa v2 data record exceeds the committed file.";
        return false;
    }

    if ( !checked )
    {
        return true;
    }

    Alembic::Util::SpookyHash hash;
    hash.Init( DATA_HASH_SEED, DATA_HASH_SEED );
    hash.Update( sizeData, sizeof( sizeData ) );

    std::vector< char > buffer( 1024 * 1024 );
    Alembic::Util::uint64_t offset = 0;
    while ( offset < size )
    {
        Alembic::Util::uint64_t count =
            std::min< Alembic::Util::uint64_t >( buffer.size(),
                                                 size - offset );
        if ( !iReadAt( pos + 8 + offset, count, &buffer[0] ) )
        {
            oError = "Could not read Ogawa v2 data.";
            return false;
        }
        hash.Update( &buffer[0], static_cast< std::size_t >( count ) );
        offset += count;
    }

    Alembic::Util::uint64_t h1 = 0;
    Alembic::Util::uint64_t h2 = 0;
    hash.Final( &h1, &h2 );

    char checksumData[8];
    if ( !iReadAt( pos + 8 + size, 8, checksumData ) )
    {
        oError = "Could not read the Ogawa v2 data checksum.";
        return false;
    }

    if ( GetU64( checksumData ) != h1 )
    {
        oError = "Ogawa v2 data checksum mismatch.";
        return false;
    }

    return true;
}

} // End namespace ALEMBIC_VERSION_NS
} // End namespace Ogawa
} // End namespace Alembic
