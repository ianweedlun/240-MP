#include "dynamictext.h"
#include <QJsonArray>

namespace InnertubeObjects
{
    // most logic courtesy of https://github.com/Rehike/Rehike
    InnertubeString DynamicText::unattribute() const
    {
        if (content.isEmpty())
            return {};
        if (!commandRuns.isArray() && !styleRuns.isArray())
            return InnertubeString(content);

        using namespace Qt::StringLiterals;
        // Keyed by QString, not QLatin1String: the lookup passes a QString, and
        // older Qt (the CI runner's) has no QString->QLatin1String conversion
        // for QHash::value(), so the _L1-keyed map fails to compile there.
        static const QHash<QString, QFont::Weight> weightMap = {
            { QStringLiteral("FONT_WEIGHT_THIN"), QFont::Thin },
            { QStringLiteral("FONT_WEIGHT_EXTRA_LIGHT"), QFont::ExtraLight },
            { QStringLiteral("FONT_WEIGHT_LIGHT"), QFont::Light },
            { QStringLiteral("FONT_WEIGHT_MEDIUM"), QFont::Medium },
            { QStringLiteral("FONT_WEIGHT_SEMI_BOLD"), QFont::DemiBold },
            { QStringLiteral("FONT_WEIGHT_BOLD"), QFont::Bold },
            { QStringLiteral("FONT_WEIGHT_EXTRA_BOLD"), QFont::ExtraBold },
            { QStringLiteral("FONT_WEIGHT_BLACK"), QFont::Black }
        };

        const QJsonArray commandRunsArr = commandRuns.toArray();
        const QJsonArray styleRunsArr = styleRuns.toArray();

        int previousLength{};
        int previousStart{};
        int textLength{};

        InnertubeString out;

        for (const QJsonValue& styleRun : styleRunsArr)
        {
            int length, startIndex;

            if (const QJsonValue indexValue = styleRun["startIndex"]; !indexValue.isUndefined())
                startIndex = indexValue.toInt();
            else
                continue;

            if (const QJsonValue lengthValue = styleRun["length"]; !lengthValue.isUndefined())
                length = lengthValue.toInt();
            else
                length = content.size();

            InnertubeRun run(content.mid(startIndex, length));
            textLength += run.text.size();

            if (auto commandRun = std::ranges::find_if(commandRunsArr, [=](const QJsonValue& run) {
                    return run["length"].toInt() == length && run["startIndex"].toInt() == startIndex;
                }); commandRun != commandRunsArr.end())
            {
                run.navigationEndpoint = (*commandRun)["onTap"_L1]["innertubeCommand"_L1];
            }

            if (!styleRun["strikethrough"].isUndefined())
                run.strikeOut = true;
            if (styleRun["italic"].toBool())
                run.style = QFont::StyleItalic;
            run.weight = weightMap.value(styleRun["weightLabel"].toString(), QFont::Normal);

            if (previousStart + previousLength < startIndex)
            {
                int plainStart = previousStart + previousLength;
                int plainLength = startIndex - plainStart;
                InnertubeRun plainRun(content.mid(plainStart, plainLength));
                textLength += plainRun.text.size();
                out.runs.append(std::move(plainRun));
            }

            out.runs.append(std::move(run));
            previousStart = startIndex;
            previousLength = length;
        }

        return out;
    }
}
