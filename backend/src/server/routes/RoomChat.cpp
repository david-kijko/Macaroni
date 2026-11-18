// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShareRoutes.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QDateTime>
#include <QJsonArray>
#include <QSet>
#include <memory>

namespace {
using Target = QPair<QString, int>; // invitation's host and app
struct Message
{
    QSet<QString> audience; // the openings that existed when this message was sent
    QJsonObject item;
};
struct Member
{
    qint64 seen = 0;
    qint64 posted = 0;
    QString name;
};
struct Room
{
    QList<Message> messages;
    QHash<QString, Member> members;
};
} // namespace

void registerRoomChat(HttpServer& server, ShareManager& share, const ShareRoutesDeps& deps)
{
    auto rooms = std::make_shared<QHash<Target, Room>>();
    auto chat = [rooms, &share, &deps](const HttpRequest& req, bool guest) -> HttpResponse {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        Target target;
        QString actor = QStringLiteral("owner");
        QString label = QStringLiteral("Host");
        if (guest) {
            const int slot = share.slotForToken(body.value("token").toString());
            if (!ShareManager::isPlayerSlot(slot) ||
                share.slotForCookie(
                    HttpServer::cookieFromRequest(req, ShareManager::kPlayerCookie)) != slot)
                return HttpResponse::error(403, "This invitation is closed or needs its PIN.");
            target = {share.hostForSlot(slot), share.appForSlot(slot)};
            actor = share.opening(slot);
            label = share.name(slot);
            if (label.isEmpty())
                label = QStringLiteral("Guest %1").arg(slot - ShareManager::kFirstSlot + 1);
        } else {
            if (!req.isLocal ||
                !HttpServer::cookieFromRequest(req, ShareManager::kPlayerCookie).isEmpty())
                return HttpResponse::error(403, "Owner access required.");
            target = {body.value("host").toString(), body.value("app").toInt(-1)};
            if (target.first.isEmpty()) return HttpResponse::error(400, "Choose a host first.");
            if (target.second == -1 && deps.currentOwnerContext) {
                const auto ctx = deps.currentOwnerContext(HttpServer::sessionTokenFromRequest(req),
                                                          target.first);
                if (ctx.first == target.first) target.second = ctx.second;
            }
        }

        QHash<Target, QSet<QString>> active;
        for (const auto& st : share.status()) {
            if (st.state != ShareManager::State::Off)
                active[{share.hostForSlot(st.slot), share.appForSlot(st.slot)}].insert(st.opening);
        }
        for (auto it = rooms->begin(); it != rooms->end();) {
            if (!active.contains(it.key()))
                it = rooms->erase(it);
            else
                ++it;
        }
        const auto audience = active.value(target);
        if (audience.isEmpty())
            return HttpResponse::error(409, "Open an invitation to start room chat.");
        Room& room = (*rooms)[target];
        room.messages.removeIf([&](const Message& m) { return !audience.intersects(m.audience); });
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto i = room.members.begin(); i != room.members.end();) {
            if (now - i->seen > 15000 || (i.key() != "owner" && !audience.contains(i.key())))
                i = room.members.erase(i);
            else
                ++i;
        }
        Member& member = room.members[actor];
        member.seen = now;
        member.name = label;
        if (body.contains("text")) {
            const QString text = body.value("text").toString().trimmed();
            if (text.isEmpty() || text.size() > 1000)
                return HttpResponse::error(400, "Use 1 to 1000 characters.");
            if (now - member.posted < 700)
                return HttpResponse::error(429, "Wait a moment before sending again.");
            member.posted = now;
            room.messages.append({audience, {{"name", label}, {"text", text}, {"time", now}}});
            if (room.messages.size() > 80) room.messages.removeFirst();
        }
        QJsonArray messages, participants;
        for (const auto& m : room.messages)
            if (!guest || m.audience.contains(actor)) messages.append(m.item);
        for (const Member& m : room.members)
            participants.append(m.name);
        return HttpResponse::json({{"messages", messages}, {"participants", participants}});
    };
    server.router()->post("/api/share/chat",
                          [chat](const HttpRequest& req) { return chat(req, false); });
    server.router()->post("/api/share/player/chat",
                          [chat](const HttpRequest& req) { return chat(req, true); });
}
