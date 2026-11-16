#ifndef Alembic_Ogawa_Validator_h
#define Alembic_Ogawa_Validator_h

#include <Alembic/Util/Export.h>
#include <Alembic/Ogawa/Foundation.h>

#include <string>

namespace Alembic {
namespace Ogawa {
namespace ALEMBIC_VERSION_NS {

struct ALEMBIC_EXPORT ValidationResult
{
    ValidationResult() : valid( false ), recovered( false ),
                         committedSize( 0 ) {}

    bool valid;
    bool recovered;
    Alembic::Util::uint64_t committedSize;
    std::string error;
};

// Validate the complete reachable Ogawa object graph.  v2 archives also
// verify the immutable base digest and every reachable checksummed record.
ALEMBIC_EXPORT ValidationResult ValidateArchive(
    const std::string & iFileName );

} // End namespace ALEMBIC_VERSION_NS

using namespace ALEMBIC_VERSION_NS;

} // End namespace Ogawa
} // End namespace Alembic

#endif
