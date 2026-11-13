#ifndef VCHAN_SUBJECT_H
#define VCHAN_SUBJECT_H

#include <QString>

namespace vchan {

// The three preset recipes. "By network" is the anchor: the network/studio
// field is what turns a preset into a real channel. See the plan doc's locked
// design contract — exactly these three, no fourth.
enum class SubjectKind { Genre, Network, Collection };

// A recipe subject: what a preset channel is built "by".
//   Genre       -> id = genre id (TheTVDB), name = "Action"
//   Network     -> id = TheTVDB network id, name = "AMC"
//   Collection  -> id = collection name, name = "Star Trek"
struct Subject {
    SubjectKind kind;
    QString id;
    QString name;

    bool operator==(const Subject &o) const {
        return kind == o.kind && id == o.id && name == o.name;
    }
    bool operator!=(const Subject &o) const { return !(*this == o); }
};

QString subjectKindName(SubjectKind k);

} // namespace vchan

#endif // VCHAN_SUBJECT_H
