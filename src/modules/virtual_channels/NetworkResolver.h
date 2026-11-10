#ifndef VCHAN_NETWORK_RESOLVER_H
#define VCHAN_NETWORK_RESOLVER_H

#include "ScheduleGenerator.h"

namespace vchan {

// Resolves a show to its TheTVDB network id. The grouping key for the "by
// network" recipe is the TheTVDB network, never the media-server studio field
// (that field is ignored for grouping per the locked design contract).
//
// The interface is injectable so the engine can be tested with a mock resolver
// and the production code can use the real TheTVDB HTTP resolver.
class NetworkResolver {
public:
    virtual ~NetworkResolver() = default;

    // TheTVDB network id for a show, or an empty string if it cannot be
    // resolved. Callers treat empty as "no network".
    virtual QString networkIdFor(const MediaItem &item) const = 0;
};

} // namespace vchan

#endif // VCHAN_NETWORK_RESOLVER_H
