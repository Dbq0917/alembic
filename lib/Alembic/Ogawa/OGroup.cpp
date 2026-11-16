//-*****************************************************************************
//
// Copyright (c) 2013,
//  Sony Pictures Imageworks Inc. and
//  Industrial Light & Magic, a division of Lucasfilm Entertainment Company Ltd.
//
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
// *       Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
// *       Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
// *       Neither the name of Industrial Light & Magic nor the names of
// its contributors may be used to endorse or promote products derived
// from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
//-*****************************************************************************

#include <Alembic/Ogawa/OGroup.h>
#include <Alembic/Ogawa/OArchive.h>
#include <Alembic/Ogawa/OData.h>
#include <Alembic/Ogawa/OStream.h>
#include <Alembic/Ogawa/IncrementalContext.h>
#include <Alembic/Ogawa/IncrementalFormat.h>

#include <map>
#include <stdexcept>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

typedef std::pair< OGroupPtr, Alembic::Util::uint64_t > ParentPair;
typedef std::vector< ParentPair > ParentPairVec;

class OGroup::PrivateData
{
public:
    PrivateData() {};
    ~PrivateData() {};

    OStreamPtr stream;

    // used before freeze
    ParentPairVec parents;

    // used before and after freeze
    std::vector<Alembic::Util::uint64_t> childVec;

    std::map< Alembic::Util::uint64_t,
              Alembic::Util::weak_ptr< OGroup > > childGroups;

    IncrementalContextPtr incremental;
    std::vector< Alembic::Util::uint64_t > path;

    // set after freeze
    Alembic::Util::uint64_t pos;
};

OGroup::OGroup(OGroupPtr iParent, Alembic::Util::uint64_t iIndex)
    : mData(new OGroup::PrivateData())
{
    mData->stream = iParent->mData->stream;
    mData->parents.push_back( ParentPair(iParent, iIndex) );
    mData->pos = INVALID_GROUP;
    mData->incremental = iParent->mData->incremental;
    mData->path = iParent->mData->path;
    mData->path.push_back( iIndex );
}

OGroup::OGroup(OStreamPtr iStream)
    : mData(new OGroup::PrivateData())
{
    mData->stream = iStream;
    mData->parents.push_back(ParentPair(OGroupPtr(), 0));
    mData->pos = INVALID_GROUP;
}

OGroup::OGroup(IncrementalContextPtr iContext,
               Alembic::Util::uint64_t iSourceRef,
               const std::vector< Alembic::Util::uint64_t > & iPath)
    : mData(new OGroup::PrivateData())
{
    mData->incremental = iContext;
    mData->path = iPath;
    mData->pos = INVALID_GROUP;
    loadIncrementalChildren( iSourceRef );
}

void OGroup::loadIncrementalChildren(Alembic::Util::uint64_t iSourceRef)
{
    std::string error;
    if ( !mData->incremental->loadGroup( iSourceRef, mData->childVec,
                                         error ) )
    {
        throw std::runtime_error( error );
    }
}

OGroup::~OGroup()
{
    freeze();
}

OGroupPtr OGroup::addGroup()
{
    if (mData->incremental)
    {
        return OGroupPtr();
    }

    OGroupPtr child;
    if (!isFrozen())
    {
        mData->childVec.push_back(0);
        child.reset(new OGroup(shared_from_this(), mData->childVec.size() - 1));
        mData->childGroups[mData->childVec.size() - 1] = child;
    }
    return child;
}

OGroupPtr OGroup::getGroup(Alembic::Util::uint64_t iIndex)
{
    if ( mData->incremental )
    {
        if ( !isChildGroup( iIndex ) )
        {
            return OGroupPtr();
        }

        std::vector< Alembic::Util::uint64_t > childPath = mData->path;
        childPath.push_back( iIndex );
        return OGroupPtr( new OGroup( mData->incremental,
                                      mData->childVec[iIndex],
                                      childPath ) );
    }

    std::map< Alembic::Util::uint64_t,
              Alembic::Util::weak_ptr< OGroup > >::iterator found =
        mData->childGroups.find( iIndex );
    if ( found != mData->childGroups.end() )
    {
        return found->second.lock();
    }
    return OGroupPtr();
}

ODataPtr OGroup::createData(Alembic::Util::uint64_t iSize, const void * iData)
{
    if (mData->incremental)
    {
        return mData->incremental->createData( iSize, iData );
    }

    ODataPtr child;
    if (isFrozen())
    {
        return child;
    }

    if (iSize == 0)
    {
        mData->childVec.push_back(EMPTY_DATA);
        child.reset(new OData());
        return child;
    }

    Alembic::Util::uint64_t pos = mData->stream->getAndSeekEndPos();

    Alembic::Util::uint64_t size = iSize;
    mData->stream->write(&size, 8);
    mData->stream->write(iData, iSize);

    child.reset(new OData(mData->stream, pos, iSize));

    return child;
}

ODataPtr OGroup::addData(Alembic::Util::uint64_t iSize, const void * iData)
{
    if ( mData->incremental )
    {
        ODataPtr child = mData->incremental->createData( iSize, iData );
        addData( child );
        return child;
    }

    ODataPtr child = OGroup::createData(iSize, iData);
    if (child)
    {
        // flip top bit for data so we can easily distinguish between it and
        // a group
        mData->childVec.push_back(child->getPos() | 0x8000000000000000ULL);
    }
    return child;
}

ODataPtr OGroup::createData(Alembic::Util::uint64_t iNumData,
                            const Alembic::Util::uint64_t * iSizes,
                            const void ** iDatas)
{
    if (mData->incremental)
    {
        Alembic::Util::uint64_t totalSize = 0;
        for (Alembic::Util::uint64_t i = 0; i < iNumData; ++i)
        {
            totalSize += iSizes[i];
        }

        std::vector< char > data;
        data.reserve( static_cast< std::size_t >( totalSize ) );
        for (Alembic::Util::uint64_t i = 0; i < iNumData; ++i)
        {
            if ( iSizes[i] != 0 )
            {
                const char * src =
                    static_cast< const char * >( iDatas[i] );
                data.insert( data.end(), src,
                             src + static_cast< std::size_t >( iSizes[i] ) );
            }
        }
        return mData->incremental->createData(
            totalSize, data.empty() ? NULL : &data[0] );
    }

    ODataPtr child;
    if (isFrozen())
    {
        return child;
    }

    Alembic::Util::uint64_t totalSize = 0;
    for (Alembic::Util::uint64_t i = 0; i < iNumData; ++i)
    {
        totalSize += iSizes[i];
    }

    if (totalSize == 0)
    {
        mData->childVec.push_back(EMPTY_DATA);
        child.reset(new OData());
        return child;
    }

    Alembic::Util::uint64_t pos = mData->stream->getAndSeekEndPos();

    mData->stream->write(&totalSize, 8);
    for (Alembic::Util::uint64_t i = 0; i < iNumData; ++i)
    {
        Alembic::Util::uint64_t size = iSizes[i];
        if (size != 0)
        {
            mData->stream->write(iDatas[i], size);
        }
    }

    child.reset(new OData(mData->stream, pos, totalSize));

    return child;
}

ODataPtr OGroup::addData(Alembic::Util::uint64_t iNumData,
                         const Alembic::Util::uint64_t * iSizes,
                         const void ** iDatas)
{
    ODataPtr child = createData(iNumData, iSizes, iDatas);
    if (child)
    {
        // flip top bit for data so we can easily distinguish between it and
        // a group
        mData->childVec.push_back(child->getPos() | 0x8000000000000000ULL);
    }
    return child;
}

void OGroup::addData(ODataPtr iData)
{
    if ( mData->incremental )
    {
        mData->incremental->addData( mData->path, mData->childVec.size(),
                                     iData );
        if ( iData->getSize() == 0 )
        {
            mData->childVec.push_back( EMPTY_DATA );
        }
        else
        {
            mData->childVec.push_back(
                OGAWA_V2_DATA_FLAG | OGAWA_V2_RECORD_FLAG );
        }
        return;
    }

    if (!isFrozen())
    {
        mData->childVec.push_back(iData->getPos() | 0x8000000000000000ULL);
    }
}

void OGroup::addGroup(OGroupPtr iGroup)
{
    if ( mData->incremental )
    {
        return;
    }

    if (!isFrozen())
    {
        if (iGroup->isFrozen())
        {
            mData->childVec.push_back(iGroup->mData->pos);
        }
        else
        {
            mData->childVec.push_back(EMPTY_GROUP);
            iGroup->mData->parents.push_back(
                ParentPair(shared_from_this(), mData->childVec.size() - 1));
        }
    }
}

void OGroup::addEmptyGroup()
{
    if ( mData->incremental )
    {
        return;
    }

    if (!isFrozen())
    {
        mData->childVec.push_back(EMPTY_GROUP);
    }
}

void OGroup::addEmptyData()
{
    if ( mData->incremental )
    {
        ODataPtr empty = mData->incremental->createData( 0, NULL );
        addData( empty );
        return;
    }

    if (!isFrozen())
    {
        mData->childVec.push_back(EMPTY_DATA);
    }
}

// no more children can be added, commit to the stream
void OGroup::freeze()
{
    if ( mData->incremental )
    {
        return;
    }

    // bail if we've already done this work
    if (isFrozen())
    {
        return;
    }

    // we ended up not adding any children, so no need to commit this group
    // to disk, use empty group instead
    if (mData->childVec.empty())
    {
        mData->pos = 0;
    }
    else
    {
        mData->pos = mData->stream->getAndSeekEndPos();
        Alembic::Util::uint64_t size = mData->childVec.size();
        mData->stream->write(&size, 8);
        mData->stream->write(&mData->childVec.front(), size*8);
    }

    // go through and update each of the parents
    ParentPairVec::iterator it;
    for(it = mData->parents.begin(); it != mData->parents.end(); ++it)
    {
        // special group owned by the archive
        if (!it->first && it->second == 0)
        {
            mData->stream->seek(8);
            mData->stream->write(&mData->pos, 8);
            continue;
        }
        else if (it->first->isFrozen())
        {
            mData->stream->seek(it->first->mData->pos + (it->second + 1) * 8);
            mData->stream->write(&mData->pos, 8);
        }
        it->first->mData->childVec[it->second] = mData->pos;
    }

    mData->parents.clear();

}

bool OGroup::isFrozen()
{
    return mData->pos != INVALID_GROUP;
}

Alembic::Util::uint64_t OGroup::getNumChildren() const
{
    return mData->childVec.size();
}

bool OGroup::isChildGroup(Alembic::Util::uint64_t iIndex) const
{
    return (iIndex < mData->childVec.size() &&
            (mData->childVec[iIndex] & EMPTY_DATA) == 0);
}

bool OGroup::isChildData(Alembic::Util::uint64_t iIndex) const
{
    return (iIndex < mData->childVec.size() &&
            (mData->childVec[iIndex] & EMPTY_DATA) != 0);
}

bool OGroup::isChildEmptyGroup(Alembic::Util::uint64_t iIndex) const
{
    return (iIndex < mData->childVec.size() &&
            mData->childVec[iIndex] == EMPTY_GROUP);
}

bool OGroup::isChildEmptyData(Alembic::Util::uint64_t iIndex) const
{
    return (iIndex < mData->childVec.size() &&
        mData->childVec[iIndex] == EMPTY_DATA);
}

void OGroup::replaceData(Alembic::Util::uint64_t iIndex, ODataPtr iData)
{
    if ( mData->incremental )
    {
        if ( !isChildData( iIndex ) )
        {
            return;
        }

        mData->incremental->replaceData( mData->path, iIndex, iData );
        if ( iData->getSize() == 0 )
        {
            mData->childVec[iIndex] = EMPTY_DATA;
        }
        else
        {
            mData->childVec[iIndex] =
                OGAWA_V2_DATA_FLAG | OGAWA_V2_RECORD_FLAG;
        }
        return;
    }

    if (!isChildData(iIndex))
    {
        return;
    }

    Alembic::Util::uint64_t pos = iData->getPos() | 0x8000000000000000ULL;
    if (isFrozen())
    {
        mData->stream->seek(mData->pos + (iIndex + 1) * 8);
        mData->stream->write(&pos, 8);
    }
    mData->childVec[iIndex] = pos;
}

} // End namespace ALEMBIC_VERSION_NS
} // End namespace Ogawa
} // End namespace Alembic
