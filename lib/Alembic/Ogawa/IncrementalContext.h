#ifndef Alembic_Ogawa_IncrementalContext_h
#define Alembic_Ogawa_IncrementalContext_h

#include <Alembic/Ogawa/Foundation.h>
#include <Alembic/Ogawa/OData.h>

#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

class IncrementalContext;
typedef Alembic::Util::shared_ptr< IncrementalContext >
    IncrementalContextPtr;

struct IncrementalChange
{
    std::vector< Alembic::Util::uint64_t > path;
    Alembic::Util::uint64_t index;
    ODataPtr data;
    bool add;
};

class IncrementalContext
{
public:
    static IncrementalContextPtr open( const std::string & iFileName );

    ~IncrementalContext();

    ODataPtr createData( Alembic::Util::uint64_t iSize,
                         const void * iData );

    void replaceData( const std::vector< Alembic::Util::uint64_t > & iPath,
                      Alembic::Util::uint64_t iIndex,
                      ODataPtr iData );

    void addData( const std::vector< Alembic::Util::uint64_t > & iPath,
                  Alembic::Util::uint64_t iIndex,
                  ODataPtr iData );

    bool loadGroup( Alembic::Util::uint64_t iRef,
                    std::vector< Alembic::Util::uint64_t > & oChildren,
                    std::string & oError );

    bool commit( std::string & oError );

    bool hasChanges() const;
    bool isValid() const;
    bool wasRecovered() const;
    Alembic::Util::uint64_t getRootRef() const;
    const std::string & getError() const;

private:
    IncrementalContext( const std::string & iFileName );

    bool openFile( const std::string & iFileName );
    bool refreshLocked( std::string & oError );
    bool repairLocked( std::string & oError );
    bool convertToV2Locked( std::string & oError );
    bool appendDataLocked( ODataPtr iData,
                           Alembic::Util::uint64_t & oRef,
                           std::string & oError );
    bool appendGroupLocked(
        const std::vector< Alembic::Util::uint64_t > & iChildren,
        Alembic::Util::uint64_t & oRef, std::string & oError );
    bool applyChangeLocked( const IncrementalChange & iChange,
                            Alembic::Util::uint64_t iDataRef,
                            Alembic::Util::uint64_t & ioRoot,
                            std::string & oError );
    bool applyChangeRecursiveLocked(
        Alembic::Util::uint64_t iGroupRef,
        const std::vector< Alembic::Util::uint64_t > & iPath,
        std::size_t iDepth,
        const IncrementalChange & iChange,
        Alembic::Util::uint64_t iDataRef,
        Alembic::Util::uint64_t & oGroupRef,
        std::string & oError );
    bool writeCommitLocked( Alembic::Util::uint64_t iRoot,
                            Alembic::Util::uint64_t iCommittedEnd,
                            Alembic::Util::uint64_t iSequence,
                            std::string & oError );

    bool readAt( Alembic::Util::uint64_t iPos,
                 Alembic::Util::uint64_t iSize, void * oBuf );
    bool writeAt( Alembic::Util::uint64_t iPos,
                  Alembic::Util::uint64_t iSize, const void * iBuf );
    bool appendBytes( Alembic::Util::uint64_t iSize, const void * iBuf );
    Alembic::Util::uint64_t fileSize();
    bool syncFile();
    bool truncateFile( Alembic::Util::uint64_t iSize );
    void lockFile();
    void unlockFile();

    std::string mFileName;
    FILE * mFile;
    std::mutex mMutex;
    std::vector< IncrementalChange > mChanges;
    bool mValid;
    bool mRecovered;
    bool mIsV2;
    std::string mError;

    Alembic::Util::uint64_t mRootRef;
    Alembic::Util::uint64_t mCommittedEnd;
    Alembic::Util::uint64_t mSequence;
    Alembic::Util::uint64_t mControlPos;
    Alembic::Util::uint64_t mBaseRoot;
    Alembic::Util::uint64_t mBaseEnd;
    Alembic::Util::uint64_t mBaseHash1;
    Alembic::Util::uint64_t mBaseHash2;
};

} // End namespace ALEMBIC_VERSION_NS

using namespace ALEMBIC_VERSION_NS;

} // End namespace Ogawa
} // End namespace Alembic

#endif
