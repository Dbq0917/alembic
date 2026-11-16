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

#include <Alembic/Ogawa/OArchive.h>
#include <Alembic/Ogawa/OStream.h>
#include <Alembic/Ogawa/OGroup.h>
#include <Alembic/Ogawa/IncrementalContext.h>

#include <stdexcept>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

OArchive::OArchive(const std::string & iFileName) :
    mStream(new OStream(iFileName))
{
    mGroup.reset(new OGroup(mStream));
}

OArchive::OArchive(const std::string & iFileName, OpenMode iMode)
{
    if ( iMode == kTruncate )
    {
        mStream.reset( new OStream( iFileName ) );
        mGroup.reset( new OGroup( mStream ) );
        return;
    }

    try
    {
        mIncremental = IncrementalContext::open( iFileName );
        mGroup.reset( new OGroup( mIncremental,
                                  mIncremental->getRootRef(),
                                  std::vector< Alembic::Util::uint64_t >() ) );
    }
    catch ( const std::exception & e )
    {
        mError = e.what();
        mIncremental.reset();
        mGroup.reset();
    }
}

OArchive::OArchive(std::ostream * iStream) :
    mStream(new OStream(iStream)), mGroup(new OGroup(mStream))
{
}

OArchive::~OArchive()
{
    if ( mIncremental && mIncremental->hasChanges() )
    {
        std::string error;
        mIncremental->commit( error );
    }
}

bool OArchive::isValid()
{
    if ( mIncremental )
    {
        return mIncremental->isValid();
    }
    return mStream->isValid();
}

OGroupPtr OArchive::getGroup()
{
    return mGroup;
}

bool OArchive::commit()
{
    if ( !mIncremental )
    {
        mError = "Archive is not open in incremental mode.";
        return false;
    }
    return mIncremental->commit( mError );
}

bool OArchive::hasPendingChanges() const
{
    return mIncremental && mIncremental->hasChanges();
}

bool OArchive::isIncremental() const
{
    return static_cast< bool >( mIncremental );
}

bool OArchive::wasRecovered() const
{
    return mIncremental && mIncremental->wasRecovered();
}

const std::string & OArchive::getError() const
{
    if ( !mError.empty() )
    {
        return mError;
    }
    if ( mIncremental )
    {
        return mIncremental->getError();
    }
    return mError;
}

} // End namespace ALEMBIC_VERSION_NS
} // End namespace Ogawa
} // End namespace Alembic
