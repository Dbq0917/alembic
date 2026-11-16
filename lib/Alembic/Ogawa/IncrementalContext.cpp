#include <Alembic/Ogawa/IncrementalContext.h>
#include <Alembic/Ogawa/IncrementalFormat.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <sstream>
#include <stdexcept>

#if defined( _WIN32 )
    #include <io.h>
    #include <windows.h>
#else
    #include <sys/file.h>
    #include <unistd.h>
#endif

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

namespace {

void PutU16( char * oBuf, Alembic::Util::uint16_t iValue )
{
    oBuf[0] = static_cast< char >( ( iValue >> 8 ) & 0xff );
    oBuf[1] = static_cast< char >( iValue & 0xff );
}

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

bool IsDataRef( Alembic::Util::uint64_t iRef )
{
    return ( iRef & OGAWA_V2_DATA_FLAG ) != 0;
}

Alembic::Util::uint64_t RefPos( Alembic::Util::uint64_t iRef )
{
    return iRef & OGAWA_V2_REF_MASK;
}

#if defined( _WIN32 )
std::wstring Utf8ToWide( const std::string & iName )
{
    int length = MultiByteToWideChar( CP_UTF8, 0, iName.c_str(), -1,
                                      NULL, 0 );
    if ( length <= 0 )
    {
        return std::wstring();
    }

    std::vector< wchar_t > result( static_cast< std::size_t >( length ) );
    MultiByteToWideChar( CP_UTF8, 0, iName.c_str(), -1, &result[0], length );
    return std::wstring( &result[0] );
}
#endif

FILE * OpenReadWrite( const std::string & iFileName )
{
#if defined( _WIN32 )
    std::wstring wide = Utf8ToWide( iFileName );
    return _wfopen( wide.c_str(), L"r+b" );
#else
    return std::fopen( iFileName.c_str(), "r+b" );
#endif
}

FILE * CreateReadWrite( const std::string & iFileName )
{
#if defined( _WIN32 )
    std::wstring wide = Utf8ToWide( iFileName );
    return _wfopen( wide.c_str(), L"w+b" );
#else
    return std::fopen( iFileName.c_str(), "w+b" );
#endif
}

int CommitFile( FILE * iFile )
{
    std::fflush( iFile );
#if defined( _WIN32 )
    return _commit( _fileno( iFile ) );
#else
    return fsync( fileno( iFile ) );
#endif
}

int SeekFile( FILE * iFile, Alembic::Util::uint64_t iPos, int iOrigin )
{
#if defined( _WIN32 )
    return _fseeki64( iFile, static_cast< __int64 >( iPos ), iOrigin );
#else
    return fseeko( iFile, static_cast< off_t >( iPos ), iOrigin );
#endif
}

long long TellFile( FILE * iFile )
{
#if defined( _WIN32 )
    return _ftelli64( iFile );
#else
    return static_cast< long long >( ftello( iFile ) );
#endif
}

bool LockFileHandle( FILE * iFile )
{
#if defined( _WIN32 )
    HANDLE handle = reinterpret_cast< HANDLE >(
        _get_osfhandle( _fileno( iFile ) ) );
    OVERLAPPED overlapped;
    std::memset( &overlapped, 0, sizeof( overlapped ) );
    return LockFileEx( handle, LOCKFILE_EXCLUSIVE_LOCK, 0,
                       MAXDWORD, MAXDWORD, &overlapped ) != 0;
#else
    return flock( fileno( iFile ), LOCK_EX ) == 0;
#endif
}

void UnlockFileHandle( FILE * iFile )
{
#if defined( _WIN32 )
    HANDLE handle = reinterpret_cast< HANDLE >(
        _get_osfhandle( _fileno( iFile ) ) );
    OVERLAPPED overlapped;
    std::memset( &overlapped, 0, sizeof( overlapped ) );
    UnlockFileEx( handle, 0, MAXDWORD, MAXDWORD, &overlapped );
#else
    flock( fileno( iFile ), LOCK_UN );
#endif
}

} // End anonymous namespace

IncrementalContext::IncrementalContext( const std::string & iFileName )
    : mFileName( iFileName ), mFile( NULL ), mValid( false ),
      mRecovered( false ), mIsV2( false ), mRootRef( 0 ),
      mCommittedEnd( 0 ), mSequence( 0 ), mControlPos( 0 ),
      mBaseRoot( 0 ), mBaseEnd( 0 ), mBaseHash1( 0 ), mBaseHash2( 0 )
{
}

IncrementalContext::~IncrementalContext()
{
    if ( mFile )
    {
        std::fclose( mFile );
    }
}

IncrementalContextPtr IncrementalContext::open(
    const std::string & iFileName )
{
    IncrementalContextPtr context( new IncrementalContext( iFileName ) );
    if ( !context->openFile( iFileName ) )
    {
        throw std::runtime_error( context->getError() );
    }

    context->lockFile();
    std::string error;
    bool refreshed = context->refreshLocked( error );
    if ( refreshed )
    {
        context->mRecovered = context->repairLocked( error );
    }
    context->unlockFile();

    if ( !refreshed || !error.empty() )
    {
        context->mValid = false;
        context->mError = error;
        throw std::runtime_error( error );
    }

    context->mValid = true;
    return context;
}

bool IncrementalContext::openFile( const std::string & iFileName )
{
    mFile = OpenReadWrite( iFileName );
    if ( !mFile )
    {
        mFile = CreateReadWrite( iFileName );
        if ( !mFile )
        {
            mError = "Could not open Ogawa archive for incremental update.";
            return false;
        }

        char emptyHeader[16] = { 0 };
        std::memcpy( emptyHeader, "Ogawa", 5 );
        emptyHeader[5] = static_cast< char >( 0xff );
        PutU16( emptyHeader + 6, 1 );
        bool wrote = std::fwrite( emptyHeader, 1, 16, mFile ) == 16 &&
            CommitFile( mFile ) == 0;
        std::fclose( mFile );
        mFile = NULL;

        if ( !wrote )
        {
            mError = "Could not initialize an empty Ogawa archive.";
            return false;
        }

        mFile = OpenReadWrite( iFileName );
    }

    if ( !mFile )
    {
        mError = "Could not reopen Ogawa archive for incremental update.";
        return false;
    }
    return true;
}

bool IncrementalContext::readAt( Alembic::Util::uint64_t iPos,
                                 Alembic::Util::uint64_t iSize,
                                 void * oBuf )
{
    std::lock_guard< std::mutex > guard( mMutex );
    if ( !mFile || SeekFile( mFile, iPos, SEEK_SET ) != 0 )
    {
        return false;
    }
    return std::fread( oBuf, 1, static_cast< std::size_t >( iSize ),
                       mFile ) == static_cast< std::size_t >( iSize );
}

bool IncrementalContext::writeAt( Alembic::Util::uint64_t iPos,
                                  Alembic::Util::uint64_t iSize,
                                  const void * iBuf )
{
    std::lock_guard< std::mutex > guard( mMutex );
    if ( !mFile || SeekFile( mFile, iPos, SEEK_SET ) != 0 )
    {
        return false;
    }
    return std::fwrite( iBuf, 1, static_cast< std::size_t >( iSize ),
                        mFile ) == static_cast< std::size_t >( iSize );
}

bool IncrementalContext::appendBytes( Alembic::Util::uint64_t iSize,
                                      const void * iBuf )
{
    std::lock_guard< std::mutex > guard( mMutex );
    if ( !mFile || SeekFile( mFile, 0, SEEK_END ) != 0 )
    {
        return false;
    }
    return std::fwrite( iBuf, 1, static_cast< std::size_t >( iSize ),
                        mFile ) == static_cast< std::size_t >( iSize );
}

Alembic::Util::uint64_t IncrementalContext::fileSize()
{
    std::lock_guard< std::mutex > guard( mMutex );
    if ( !mFile )
    {
        return 0;
    }

    if ( SeekFile( mFile, 0, SEEK_END ) != 0 )
    {
        return 0;
    }
    long long end = TellFile( mFile );
    return end < 0 ? 0 : static_cast< Alembic::Util::uint64_t >( end );
}

bool IncrementalContext::syncFile()
{
    std::lock_guard< std::mutex > guard( mMutex );
    return mFile && CommitFile( mFile ) == 0;
}

bool IncrementalContext::truncateFile( Alembic::Util::uint64_t iSize )
{
    std::lock_guard< std::mutex > guard( mMutex );
    if ( !mFile )
    {
        return false;
    }
    std::fflush( mFile );
#if defined( _WIN32 )
    return _chsize_s( _fileno( mFile ),
                      static_cast< __int64 >( iSize ) ) == 0;
#else
    return ftruncate( fileno( mFile ),
                      static_cast< off_t >( iSize ) ) == 0;
#endif
}

void IncrementalContext::lockFile()
{
    if ( mFile )
    {
        LockFileHandle( mFile );
    }
}

void IncrementalContext::unlockFile()
{
    if ( mFile )
    {
        UnlockFileHandle( mFile );
    }
}

bool IncrementalContext::refreshLocked( std::string & oError )
{
    Alembic::Util::uint64_t size = fileSize();
    ReadAtFn readAtFn = [this]( Alembic::Util::uint64_t iPos,
                                Alembic::Util::uint64_t iSize,
                                void * oBuf )
    {
        return this->readAt( iPos, iSize, oBuf );
    };

    OgawaHeader header;
    if ( !ReadOgawaHeader( readAtFn, size, header, oError ) )
    {
        return false;
    }

    if ( header.version == 1 )
    {
        mIsV2 = false;
        mRootRef = header.groupPos;
        mCommittedEnd = size;
        mSequence = 0;
        mBaseRoot = header.groupPos;
        mBaseEnd = 0;
        return true;
    }

    if ( header.version != OGAWA_V2_VERSION )
    {
        std::ostringstream msg;
        msg << "Unsupported Ogawa version " << header.version << ".";
        oError = msg.str();
        return false;
    }

    OgawaV2Control control;
    if ( !ReadOgawaV2Control( readAtFn, size, header.groupPos,
                              control, oError ) )
    {
        return false;
    }

    const OgawaV2Slot & slot = control.slots[control.selectedSlot];
    mIsV2 = true;
    mRootRef = slot.root;
    mCommittedEnd = slot.committedEnd;
    mSequence = slot.sequence;
    mControlPos = control.position;
    mBaseRoot = control.baseRoot;
    mBaseEnd = control.baseEnd;
    mBaseHash1 = control.baseHash1;
    mBaseHash2 = control.baseHash2;
    return true;
}

bool IncrementalContext::repairLocked( std::string & oError )
{
    Alembic::Util::uint64_t actualSize = fileSize();
    bool repaired = false;

    if ( mIsV2 && actualSize > mCommittedEnd )
    {
        if ( !truncateFile( mCommittedEnd ) )
        {
            oError = "Could not truncate an incomplete Ogawa transaction.";
            return false;
        }
        repaired = true;
    }

    char frozen = static_cast< char >( 0xff );
    if ( !writeAt( 5, 1, &frozen ) || !syncFile() )
    {
        oError = "Could not repair the Ogawa archive header.";
        return false;
    }

    return repaired;
}

bool IncrementalContext::hasChanges() const
{
    return !mChanges.empty();
}

bool IncrementalContext::isValid() const
{
    return mValid;
}

bool IncrementalContext::wasRecovered() const
{
    return mRecovered;
}

Alembic::Util::uint64_t IncrementalContext::getRootRef() const
{
    return mRootRef;
}

const std::string & IncrementalContext::getError() const
{
    return mError;
}

ODataPtr IncrementalContext::createData(
    Alembic::Util::uint64_t iSize, const void * iData )
{
    return ODataPtr( new OData( iSize, iData ) );
}

void IncrementalContext::replaceData(
    const std::vector< Alembic::Util::uint64_t > & iPath,
    Alembic::Util::uint64_t iIndex, ODataPtr iData )
{
    IncrementalChange change;
    change.path = iPath;
    change.index = iIndex;
    change.data = iData;
    change.add = false;
    mChanges.push_back( change );
}

void IncrementalContext::addData(
    const std::vector< Alembic::Util::uint64_t > & iPath,
    Alembic::Util::uint64_t iIndex, ODataPtr iData )
{
    IncrementalChange change;
    change.path = iPath;
    change.index = iIndex;
    change.data = iData;
    change.add = true;
    mChanges.push_back( change );
}

bool IncrementalContext::loadGroup(
    Alembic::Util::uint64_t iRef,
    std::vector< Alembic::Util::uint64_t > & oChildren,
    std::string & oError )
{
    oChildren.clear();
    if ( iRef == EMPTY_GROUP )
    {
        return true;
    }

    if ( IsDataRef( iRef ) )
    {
        oError = "Expected an Ogawa group reference.";
        return false;
    }

    Alembic::Util::uint64_t pos = RefPos( iRef );
    bool checked = ( iRef & OGAWA_V2_RECORD_FLAG ) != 0;
    Alembic::Util::uint64_t readableEnd = fileSize();
    if ( pos < 16 || pos + 8 > readableEnd )
    {
        oError = "Ogawa group is outside the committed archive.";
        return false;
    }

    char countData[8];
    if ( !readAt( pos, 8, countData ) )
    {
        oError = "Could not read an Ogawa group.";
        return false;
    }
    Alembic::Util::uint64_t count = GetU64( countData );
    Alembic::Util::uint64_t trailing = checked ? 8 : 0;
    if ( pos + 8 + trailing > readableEnd ||
         count > ( readableEnd - pos - 8 - trailing ) / 8 )
    {
        oError = "Invalid Ogawa group child count.";
        return false;
    }

    oChildren.resize( static_cast< std::size_t >( count ) );
    if ( count > 0 )
    {
        std::vector< char > childData(
            static_cast< std::size_t >( count * 8 ) );
        if ( !readAt( pos + 8, childData.size(), &childData[0] ) )
        {
            oError = "Could not read Ogawa group children.";
            return false;
        }
        for ( std::size_t i = 0; i < oChildren.size(); ++i )
        {
            oChildren[i] = GetU64( &childData[i * 8] );
        }
    }

    if ( checked )
    {
        char checksumData[8];
        if ( !readAt( pos + 8 + count * 8, 8, checksumData ) )
        {
            oError = "Could not read the Ogawa group checksum.";
            return false;
        }
        Alembic::Util::uint64_t expected = GetU64( checksumData );
        Alembic::Util::uint64_t actual = HashV2GroupRecord(
            count, count > 0 ? &oChildren[0] : NULL );
        if ( expected != actual )
        {
            oError = "Ogawa group checksum mismatch.";
            return false;
        }
    }

    return true;
}

bool IncrementalContext::convertToV2Locked( std::string & oError )
{
    Alembic::Util::uint64_t size = fileSize();
    if ( size < 16 )
    {
        oError = "Ogawa archive is too small to convert.";
        return false;
    }

    OgawaHeader header;
    ReadAtFn readAtFn = [this]( Alembic::Util::uint64_t iPos,
                                Alembic::Util::uint64_t iSize,
                                void * oBuf )
    {
        return this->readAt( iPos, iSize, oBuf );
    };
    if ( !ReadOgawaHeader( readAtFn, size, header, oError ) ||
         header.version != 1 )
    {
        if ( oError.empty() )
        {
            oError = "Ogawa archive changed while converting it.";
        }
        return false;
    }

    Alembic::Util::SpookyHash baseHash;
    baseHash.Init( 0x6f67617761326261ULL, 0x6f67617761326262ULL );
    std::vector< char > buffer( 1024 * 1024 );
    Alembic::Util::uint64_t pos = 16;
    while ( pos < size )
    {
        Alembic::Util::uint64_t count =
            std::min< Alembic::Util::uint64_t >( buffer.size(), size - pos );
        if ( !readAt( pos, count, &buffer[0] ) )
        {
            oError = "Could not hash the Ogawa base archive.";
            return false;
        }
        baseHash.Update( &buffer[0], static_cast< std::size_t >( count ) );
        pos += count;
    }

    Alembic::Util::uint64_t hash1 = 0;
    Alembic::Util::uint64_t hash2 = 0;
    baseHash.Final( &hash1, &hash2 );

    mBaseRoot = header.groupPos;
    mBaseEnd = size;
    mControlPos = size;
    mBaseHash1 = hash1;
    mBaseHash2 = hash2;
    mSequence = 1;
    mCommittedEnd = size + OGAWA_V2_CONTROL_SIZE;
    mRootRef = header.groupPos;
    mIsV2 = true;

    char control[OGAWA_V2_CONTROL_SIZE];
    std::memset( control, 0, sizeof( control ) );
    std::memcpy( control, OGAWA_V2_CONTROL_MAGIC, 8 );
    control[8] = 1;
    PutU64( control + 12, OGAWA_V2_CONTROL_SIZE );
    PutU64( control + 16, mBaseRoot );
    PutU64( control + 24, mBaseEnd );
    PutU64( control + 32, mBaseHash1 );
    PutU64( control + 40, mBaseHash2 );

    OgawaV2Slot slot;
    slot.sequence = 1;
    slot.root = mBaseRoot;
    slot.committedEnd = mCommittedEnd;
    slot.checksum = HashV2Slot( slot );
    PutU64( control + OGAWA_V2_SLOT_OFFSET, slot.sequence );
    PutU64( control + OGAWA_V2_SLOT_OFFSET + 8, slot.root );
    PutU64( control + OGAWA_V2_SLOT_OFFSET + 16, slot.committedEnd );
    PutU64( control + OGAWA_V2_SLOT_OFFSET + 24, slot.checksum );

    Alembic::Util::uint64_t controlHash = Alembic::Util::SpookyHash::Hash64(
        control, 64, 0x6f67617761326374ULL );
    PutU64( control + 128, controlHash );

    if ( !appendBytes( sizeof( control ), control ) || !syncFile() )
    {
        oError = "Could not write the Ogawa v2 control block.";
        return false;
    }

    char headerData[16] = { 0 };
    std::memcpy( headerData, "Ogawa", 5 );
    headerData[5] = static_cast< char >( 0xff );
    PutU16( headerData + 6, OGAWA_V2_VERSION );
    PutU64( headerData + 8, mControlPos );
    if ( !writeAt( 0, 16, headerData ) || !syncFile() )
    {
        oError = "Could not commit the Ogawa v2 header.";
        return false;
    }

    return true;
}

bool IncrementalContext::appendDataLocked( ODataPtr iData,
                                           Alembic::Util::uint64_t & oRef,
                                           std::string & oError )
{
    if ( !iData )
    {
        oError = "Cannot append a null Ogawa data object.";
        return false;
    }

    if ( iData->getSize() == 0 )
    {
        oRef = EMPTY_DATA;
        return true;
    }

    Alembic::Util::uint64_t pos = fileSize();
    Alembic::Util::uint64_t size = iData->getSize();
    const void * data = iData->getStagedData();
    Alembic::Util::uint64_t checksum = HashV2DataRecord( size, data );

    char sizeData[8];
    PutU64( sizeData, size );
    char checksumData[8];
    PutU64( checksumData, checksum );
    if ( !appendBytes( 8, sizeData ) ||
         !appendBytes( size, data ) ||
         !appendBytes( 8, checksumData ) )
    {
        oError = "Could not append an Ogawa data record.";
        return false;
    }

    oRef = OGAWA_V2_DATA_FLAG | OGAWA_V2_RECORD_FLAG | pos;
    return true;
}

bool IncrementalContext::appendGroupLocked(
    const std::vector< Alembic::Util::uint64_t > & iChildren,
    Alembic::Util::uint64_t & oRef, std::string & oError )
{
    Alembic::Util::uint64_t pos = fileSize();
    Alembic::Util::uint64_t count = iChildren.size();
    std::vector< char > record(
        static_cast< std::size_t >( 8 + count * 8 + 8 ) );
    PutU64( &record[0], count );
    for ( std::size_t i = 0; i < iChildren.size(); ++i )
    {
        PutU64( &record[8 + i * 8], iChildren[i] );
    }
    Alembic::Util::uint64_t checksum = HashV2GroupRecord(
        count, count > 0 ? &iChildren[0] : NULL );
    PutU64( &record[static_cast< std::size_t >( 8 + count * 8 )],
            checksum );

    if ( !appendBytes( record.size(), &record[0] ) )
    {
        oError = "Could not append an Ogawa group record.";
        return false;
    }

    oRef = OGAWA_V2_RECORD_FLAG | pos;
    return true;
}

bool IncrementalContext::applyChangeRecursiveLocked(
    Alembic::Util::uint64_t iGroupRef,
    const std::vector< Alembic::Util::uint64_t > & iPath,
    std::size_t iDepth, const IncrementalChange & iChange,
    Alembic::Util::uint64_t iDataRef,
    Alembic::Util::uint64_t & oGroupRef, std::string & oError )
{
    std::vector< Alembic::Util::uint64_t > children;
    if ( !loadGroup( iGroupRef, children, oError ) )
    {
        return false;
    }

    if ( iDepth == iPath.size() )
    {
        if ( iChange.add )
        {
            if ( iChange.index > children.size() )
            {
                oError = "Concurrent update changed the target group shape.";
                return false;
            }
            children.insert( children.begin() + iChange.index, iDataRef );
        }
        else
        {
            if ( iChange.index >= children.size() ||
                 !IsDataRef( children[iChange.index] ) )
            {
                oError = "Incremental replacement target is not a data child.";
                return false;
            }
            children[iChange.index] = iDataRef;
        }

        return appendGroupLocked( children, oGroupRef, oError );
    }

    Alembic::Util::uint64_t childIndex = iPath[iDepth];
    if ( childIndex >= children.size() ||
         IsDataRef( children[childIndex] ) )
    {
        oError = "Incremental replacement path no longer identifies a group.";
        return false;
    }

    Alembic::Util::uint64_t newChild = 0;
    if ( !applyChangeRecursiveLocked( children[childIndex], iPath,
                                      iDepth + 1, iChange, iDataRef,
                                      newChild, oError ) )
    {
        return false;
    }

    children[childIndex] = newChild;
    return appendGroupLocked( children, oGroupRef, oError );
}

bool IncrementalContext::applyChangeLocked(
    const IncrementalChange & iChange,
    Alembic::Util::uint64_t iDataRef,
    Alembic::Util::uint64_t & ioRoot, std::string & oError )
{
    Alembic::Util::uint64_t newRoot = ioRoot;
    if ( !applyChangeRecursiveLocked( ioRoot, iChange.path, 0, iChange,
                                      iDataRef, newRoot, oError ) )
    {
        return false;
    }
    ioRoot = newRoot;
    return true;
}

bool IncrementalContext::writeCommitLocked(
    Alembic::Util::uint64_t iRoot,
    Alembic::Util::uint64_t iCommittedEnd,
    Alembic::Util::uint64_t iSequence,
    std::string & oError )
{
    OgawaV2Slot slot;
    slot.sequence = iSequence;
    slot.root = iRoot;
    slot.committedEnd = iCommittedEnd;
    slot.checksum = HashV2Slot( slot );

    Alembic::Util::uint64_t slotIndex = 1 - ( iSequence & 1 );
    char slotData[OGAWA_V2_SLOT_SIZE];
    PutU64( slotData, slot.sequence );
    PutU64( slotData + 8, slot.root );
    PutU64( slotData + 16, slot.committedEnd );
    PutU64( slotData + 24, slot.checksum );

    Alembic::Util::uint64_t slotPos = mControlPos +
        OGAWA_V2_SLOT_OFFSET + slotIndex * OGAWA_V2_SLOT_SIZE;
    if ( !writeAt( slotPos, sizeof( slotData ), slotData ) )
    {
        oError = "Could not write the Ogawa v2 commit slot.";
        return false;
    }

    if ( !syncFile() )
    {
        oError = "Could not flush the Ogawa v2 commit slot.";
        return false;
    }

    char frozen = static_cast< char >( 0xff );
    if ( !writeAt( 5, 1, &frozen ) || !syncFile() )
    {
        oError = "Could not finalize the Ogawa v2 commit.";
        return false;
    }

    mRootRef = iRoot;
    mCommittedEnd = iCommittedEnd;
    mSequence = iSequence;
    mIsV2 = true;
    return true;
}

bool IncrementalContext::commit( std::string & oError )
{
    lockFile();
    oError.clear();

    bool success = false;
    try
    {
        if ( !refreshLocked( oError ) )
        {
            unlockFile();
            return false;
        }

        mRecovered = repairLocked( oError );
        if ( !oError.empty() )
        {
            unlockFile();
            return false;
        }

        if ( !mIsV2 )
        {
            if ( !convertToV2Locked( oError ) )
            {
                unlockFile();
                return false;
            }
        }
        else if ( fileSize() > mCommittedEnd &&
                  !truncateFile( mCommittedEnd ) )
        {
            oError = "Could not truncate an incomplete Ogawa transaction.";
            unlockFile();
            return false;
        }

        char dirty = 0;
        if ( !writeAt( 5, 1, &dirty ) )
        {
            oError = "Could not mark the Ogawa archive as dirty.";
            unlockFile();
            return false;
        }

        std::map< OData *, Alembic::Util::uint64_t > refs;
        for ( std::size_t i = 0; i < mChanges.size(); ++i )
        {
            OData * data = mChanges[i].data.get();
            if ( refs.find( data ) == refs.end() )
            {
                Alembic::Util::uint64_t ref = EMPTY_DATA;
                if ( !appendDataLocked( mChanges[i].data, ref, oError ) )
                {
                    unlockFile();
                    return false;
                }
                refs[data] = ref;
            }
        }

        if ( !syncFile() )
        {
            oError = "Could not flush incremental Ogawa data.";
            unlockFile();
            return false;
        }

        Alembic::Util::uint64_t root = mRootRef;
        for ( std::size_t i = 0; i < mChanges.size(); ++i )
        {
            if ( !applyChangeLocked( mChanges[i],
                                     refs[mChanges[i].data.get()],
                                     root, oError ) )
            {
                unlockFile();
                return false;
            }
        }

        Alembic::Util::uint64_t end = fileSize();
        if ( !syncFile() ||
             !writeCommitLocked( root, end, mSequence + 1, oError ) )
        {
            unlockFile();
            return false;
        }

        for ( std::map< OData *, Alembic::Util::uint64_t >::iterator it =
                  refs.begin(); it != refs.end(); ++it )
        {
            if ( it->first )
            {
                it->first->setPos( RefPos( it->second ) );
            }
        }

        mChanges.clear();
        mValid = true;
        success = true;
    }
    catch ( const std::exception & e )
    {
        oError = e.what();
    }

    unlockFile();
    return success;
}

} // End namespace ALEMBIC_VERSION_NS
} // End namespace Ogawa
} // End namespace Alembic
