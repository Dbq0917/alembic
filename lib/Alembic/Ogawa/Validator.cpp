#include <Alembic/Ogawa/Validator.h>
#include <Alembic/Ogawa/IncrementalFormat.h>
#include <Alembic/Util/SpookyV2.h>

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <vector>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

namespace {

class ArchiveFile
{
public:
    ArchiveFile( const std::string & iFileName ) :
        mFile( iFileName.c_str(), std::ios_base::binary )
    {
        if ( mFile )
        {
            mFile.seekg( 0, std::ios_base::end );
            std::streamoff end = mFile.tellg();
            mSize = end < 0 ? 0 :
                static_cast< Alembic::Util::uint64_t >( end );
        }
    }

    bool isOpen() const
    {
        return mFile.is_open();
    }

    Alembic::Util::uint64_t size() const
    {
        return mSize;
    }

    bool read( Alembic::Util::uint64_t iPos,
               Alembic::Util::uint64_t iSize, void * oBuf )
    {
        if ( iPos > mSize || iSize > mSize - iPos )
        {
            return false;
        }
        mFile.clear();
        mFile.seekg( static_cast< std::streamoff >( iPos ),
                     std::ios_base::beg );
        if ( !mFile )
        {
            return false;
        }
        mFile.read( static_cast< char * >( oBuf ),
                    static_cast< std::streamsize >( iSize ) );
        return static_cast< Alembic::Util::uint64_t >( mFile.gcount() ) ==
            iSize;
    }

private:
    std::ifstream mFile;
    Alembic::Util::uint64_t mSize = 0;
};

bool ValidateBaseHash( const ReadAtFn & iReadAt,
                       Alembic::Util::uint64_t iBaseEnd,
                       Alembic::Util::uint64_t iExpected1,
                       Alembic::Util::uint64_t iExpected2,
                       std::string & oError )
{
    if ( iBaseEnd < 16 )
    {
        oError = "Ogawa v2 base region is invalid.";
        return false;
    }

    Alembic::Util::SpookyHash hash;
    hash.Init( 0x6f67617761326261ULL, 0x6f67617761326262ULL );
    std::vector< char > buffer( 1024 * 1024 );
    Alembic::Util::uint64_t pos = 16;
    while ( pos < iBaseEnd )
    {
        Alembic::Util::uint64_t count =
            std::min< Alembic::Util::uint64_t >( buffer.size(),
                                                 iBaseEnd - pos );
        if ( !iReadAt( pos, count, &buffer[0] ) )
        {
            oError = "Could not read the Ogawa v2 base region.";
            return false;
        }
        hash.Update( &buffer[0], static_cast< std::size_t >( count ) );
        pos += count;
    }

    Alembic::Util::uint64_t h1 = 0;
    Alembic::Util::uint64_t h2 = 0;
    hash.Final( &h1, &h2 );
    if ( h1 != iExpected1 || h2 != iExpected2 )
    {
        oError = "Ogawa v2 base region checksum mismatch.";
        return false;
    }
    return true;
}

bool ValidateTree( const ReadAtFn & iReadAt,
                   Alembic::Util::uint64_t iRef,
                   Alembic::Util::uint64_t iCommittedEnd,
                   std::set< Alembic::Util::uint64_t > & ioActive,
                   std::set< Alembic::Util::uint64_t > & ioValidated,
                   std::string & oError )
{
    if ( iRef == EMPTY_GROUP || iRef == EMPTY_DATA )
    {
        return true;
    }

    Alembic::Util::uint64_t pos = iRef & OGAWA_V2_REF_MASK;
    if ( pos < 16 )
    {
        oError = "Ogawa reference points into the archive header.";
        return false;
    }
    if ( ioValidated.find( pos ) != ioValidated.end() )
    {
        return true;
    }
    if ( ioActive.find( pos ) != ioActive.end() )
    {
        oError = "Ogawa object graph contains a cycle.";
        return false;
    }

    ioActive.insert( pos );
    bool result = false;
    if ( ( iRef & OGAWA_V2_DATA_FLAG ) != 0 )
    {
        result = ValidateV2DataRecord( iReadAt, iRef, iCommittedEnd,
                                       oError );
    }
    else
    {
        if ( !ValidateV2GroupRecord( iReadAt, iRef, iCommittedEnd,
                                     oError ) )
        {
            ioActive.erase( pos );
            return false;
        }

        bool checked = ( iRef & OGAWA_V2_RECORD_FLAG ) != 0;
        char countData[8];
        if ( !iReadAt( pos, 8, countData ) )
        {
            oError = "Could not read an Ogawa group record.";
            ioActive.erase( pos );
            return false;
        }

        Alembic::Util::uint64_t count = 0;
        for ( std::size_t i = 0; i < 8; ++i )
        {
            count |= static_cast< Alembic::Util::uint64_t >(
                static_cast< unsigned char >( countData[i] ) ) << ( i * 8 );
        }

        Alembic::Util::uint64_t trailing = checked ? 8 : 0;
        if ( pos + 8 + trailing > iCommittedEnd ||
             count > ( iCommittedEnd - pos - 8 - trailing ) / 8 )
        {
            oError = "Invalid Ogawa group child count.";
            ioActive.erase( pos );
            return false;
        }

        std::vector< char > childData(
            static_cast< std::size_t >( count * 8 ) );
        if ( count > 0 &&
             !iReadAt( pos + 8, childData.size(), &childData[0] ) )
        {
            oError = "Could not read Ogawa group children.";
            ioActive.erase( pos );
            return false;
        }

        result = true;
        for ( std::size_t i = 0; i < static_cast< std::size_t >( count );
              ++i )
        {
            Alembic::Util::uint64_t child = 0;
            for ( std::size_t b = 0; b < 8; ++b )
            {
                child |= static_cast< Alembic::Util::uint64_t >(
                    static_cast< unsigned char >(
                        childData[i * 8 + b] ) ) << ( b * 8 );
            }
            if ( !ValidateTree( iReadAt, child, iCommittedEnd, ioActive,
                                ioValidated, oError ) )
            {
                result = false;
                break;
            }
        }
    }

    ioActive.erase( pos );
    if ( result )
    {
        ioValidated.insert( pos );
    }
    return result;
}

} // End anonymous namespace

ValidationResult ValidateArchive( const std::string & iFileName )
{
    ValidationResult result;
    ArchiveFile file( iFileName );
    if ( !file.isOpen() )
    {
        result.error = "Could not open Ogawa archive: " + iFileName;
        return result;
    }

    ReadAtFn readAt = [&file]( Alembic::Util::uint64_t iPos,
                               Alembic::Util::uint64_t iSize,
                               void * oBuf )
    {
        return file.read( iPos, iSize, oBuf );
    };

    OgawaHeader header;
    if ( !ReadOgawaHeader( readAt, file.size(), header, result.error ) )
    {
        return result;
    }

    Alembic::Util::uint64_t root = header.groupPos;
    Alembic::Util::uint64_t committedEnd = file.size();

    if ( header.version == OGAWA_V2_VERSION )
    {
        OgawaV2Control control;
        if ( !ReadOgawaV2Control( readAt, file.size(), header.groupPos,
                                  control, result.error ) )
        {
            return result;
        }

        const OgawaV2Slot & slot = control.slots[control.selectedSlot];
        root = slot.root;
        committedEnd = slot.committedEnd;
        result.recovered = !header.frozen ||
            file.size() != committedEnd ||
            ( control.slots[1 - control.selectedSlot].sequence >
              slot.sequence );

        if ( !ValidateBaseHash( readAt, control.baseEnd,
                                control.baseHash1, control.baseHash2,
                                result.error ) )
        {
            return result;
        }
    }
    else if ( header.version != 1 )
    {
        std::ostringstream msg;
        msg << "Unsupported Ogawa version " << header.version << ".";
        result.error = msg.str();
        return result;
    }
    else
    {
        result.recovered = !header.frozen;
    }

    std::set< Alembic::Util::uint64_t > active;
    std::set< Alembic::Util::uint64_t > validated;
    if ( !ValidateTree( readAt, root, committedEnd, active, validated,
                        result.error ) )
    {
        return result;
    }

    result.valid = true;
    result.committedSize = committedEnd;
    return result;
}

} // End namespace ALEMBIC_VERSION_NS
} // End namespace Ogawa
} // End namespace Alembic
