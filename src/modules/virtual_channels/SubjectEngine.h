#ifndef VCHAN_SUBJECT_ENGINE_H
#define VCHAN_SUBJECT_ENGINE_H

#include "MediaServerSource.h"
#include "NetworkResolver.h"
#include "Subject.h"
#include <QVector>

namespace vchan {

// Pure logic for the "backend recipe channels" (Unit B): given a subject
// (genre / network / collection) and the union of items from the enabled
// remote servers, produce the items that belong to that subject, ready for the
// ScheduleGenerator to lay into a rolling schedule.
//
//   Genre / Collection -> the server already filtered by the subject, so the
//                         engine only unions by title across servers.
//   Network            -> the engine unions by title, then keeps only the
//                         shows whose TheTVDB network matches the subject.
//
// Everything here is static and side-effect free (except the resolver, which is
// injected) so it is fully testable with the FakeMediaServer fixtures.
class SubjectEngine {
public:
    // The show identity used for cross-server dedup: the series name when the
    // item is an episode, else the title (a movie). Compared case-insensitively
    // after trimming. This is the "title" the plan dedupes on.
    static QString showKey(const MediaItem &item);

    // Union two item lists, keeping the first occurrence of each show.
    static QVector<MediaItem> unionByTitle(const QVector<MediaItem> &a,
                                           const QVector<MediaItem> &b);

    // Union one item list per server, deduped by show key. An empty server list
    // contributes nothing.
    static QVector<MediaItem> unionAcrossServers(
        const QVector<QVector<MediaItem>> &servers);

    // Does this show belong to the subject?
    //   Genre / Collection -> true (the server already filtered to it).
    //   Network            -> the resolved TheTVDB network matches the subject.
    static bool belongsTo(const MediaItem &item, const Subject &subject,
                          const NetworkResolver &resolver);

    // Filter a unioned item list down to one subject.
    static QVector<MediaItem> itemsForSubject(const QVector<MediaItem> &items,
                                              const Subject &subject,
                                              const NetworkResolver &resolver);

    // Build the scan Request for a subject. Genre/Collection carry the subject
    // value as a server-side filter; Network carries no server filter (the
    // engine resolves it post-union), so it scans broadly and the caller passes
    // the wanted subject name through the Request.subject field.
    static MediaServerSource::Request buildRequest(const Subject &subject);
};

} // namespace vchan

#endif // VCHAN_SUBJECT_ENGINE_H
