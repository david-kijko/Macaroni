// SPDX-License-Identifier: GPL-3.0-or-later
#include "ShareRoutes.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QDateTime>
#include <QFile>
#include <QStandardPaths>
#include <QJsonArray>
#include <QUrl>
#include <algorithm>
#include <memory>

namespace {
constexpr int kFourHours = 4 * 3600;
struct Invite
{
    QString secret;
    qint64 expires;
    int app;
    bool allowControl = false;
    QSet<QString> requests;
    QMap<int, QString> openings; // never act on a slot that has since been reused
};
using Invites = QHash<QString, Invite>; // one active sharing session per Mac
} // namespace

void registerHandsSharing(HttpServer& server, ShareManager& share, const ShareRoutesDeps& deps)
{
    auto invites = std::make_shared<Invites>();
    QObject::connect(&share, &ShareManager::sharingEnded, &server, [invites](const QString& host) {
        if (host.isEmpty())
            invites->clear();
        else
            invites->remove(host);
    });
    auto close = [&share, &deps](Invite& invite) {
        for (auto it = invite.openings.begin(); it != invite.openings.end(); ++it) {
            if (share.opening(it.key()) != it.value()) continue;
            share.setPermissions(it.key(), {});
            if (deps.stopPlayerStream) deps.stopPlayerStream(it.key(), true);
            share.deactivate(it.key(), ShareManager::EndReason::OwnerToggle);
        }
    };
    auto sweep = [invites, close, &share]() {
        const auto now = QDateTime::currentSecsSinceEpoch();
        for (auto it = invites->begin(); it != invites->end();) {
            if (now >= it->expires) {
                close(it.value());
                it = invites->erase(it);
            } else {
                for (auto member = it->openings.begin(); member != it->openings.end();) {
                    if (share.opening(member.key()) != member.value()) {
                        it->requests.remove(member.value());
                        member = it->openings.erase(member);
                    } else
                        ++member;
                }
                ++it;
            }
        }
    };
    server.router()->post("/api/share/room", [=, &share, &server, &deps](const HttpRequest& req) {
        if (!req.isLocal ||
            !HttpServer::cookieFromRequest(req, ShareManager::kPlayerCookie).isEmpty())
            return HttpResponse::error(403, "Owner access required.");
        sweep();
        const auto body = QJsonDocument::fromJson(req.body).object();
        const auto host = body.value("host").toString();
        const auto action = body.value("action").toString();
        auto it = invites->find(host);
        if (action == "open" && it == invites->end()) {
            int app = body.value("app").toInt(-1);
            if (app == -1 && deps.currentOwnerContext) {
                const auto ctx =
                    deps.currentOwnerContext(HttpServer::sessionTokenFromRequest(req), host);
                if (ctx.first == host) app = ctx.second;
            }
            if (!deps.hostAppExists || !deps.hostAppExists(host, app))
                return HttpResponse::error(400, "Choose a host and an app to share.");
            it = invites->insert(host, Invite{ShareManager::inviteSecret(),
                                              QDateTime::currentSecsSinceEpoch() + kFourHours,
                                              app,
                                              false,
                                              {},
                                              {}});
        }
        if (action != "status" && action != "open") {
            if (it == invites->end() || body.value("invitation").toString() != it->secret)
                return HttpResponse::error(409, "The sharing session changed. Reopen Invite.");
            if (action == "end") {
                close(it.value());
                invites->erase(it);
                return HttpResponse::json({{"active", false}});
            }
            if (action == "allow") {
                it->allowControl =
                    it->app != ShareManager::kChatOnlyApp && body.value("value").toBool();
                if (!it->allowControl) {
                    it->requests.clear();
                    for (int slot : it->openings.keys())
                        share.setPermissions(slot, {});
                }
            } else if (action == "control" || action == "remove") {
                const int slot = body.value("slot").toInt(-1);
                if (!it->openings.contains(slot) ||
                    it->openings.value(slot) != body.value("opening").toString())
                    return HttpResponse::error(409, "This participant has left.");
                if (action == "control") {
                    const bool grant = body.value("value").toBool();
                    if (grant && it->app == ShareManager::kChatOnlyApp)
                        return HttpResponse::error(403, "Chat-only invitation.");
                    it->requests.remove(it->openings.value(slot));
                    if (grant)
                        for (int other : it->openings.keys())
                            if (other != slot) share.setPermissions(other, {});
                    ShareManager::Permissions p;
                    p.keyboardMouse = grant;
                    share.setPermissions(slot, p);
                } else {
                    share.setPermissions(slot, {});
                    if (deps.stopPlayerStream) deps.stopPlayerStream(slot, true);
                    share.deactivate(slot, ShareManager::EndReason::OwnerToggle);
                    it->requests.remove(it->openings.value(slot));
                    it->openings.remove(slot);
                }
            } else
                return HttpResponse::error(400, "Unknown room action.");
        }
        if (it == invites->end()) return HttpResponse::json({{"active", false}});
        QJsonArray members;
        for (int slot : it->openings.keys()) {
            members.append(
                QJsonObject{{"slot", slot},
                            {"opening", it->openings.value(slot)},
                            {"name", share.name(slot)},
                            {"control", share.permissions(slot).keyboardMouse},
                            {"requested", it->requests.contains(it->openings.value(slot))}});
        }
        QString origin = qEnvironmentVariable("MACARONI_GUEST_ORIGIN");
        if (origin.isEmpty()) {
            QFile setting(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
                          QStringLiteral("/Macaroni/guest-origin"));
            if (setting.open(QIODevice::ReadOnly))
                origin = QString::fromUtf8(setting.read(1024)).trimmed();
        }
        if (origin.isEmpty() && !server.domain().isEmpty())
            origin = QStringLiteral("https://") + server.domain();
        if (!origin.isEmpty()) {
            const QUrl supplied(origin);
            if (!supplied.isValid() || supplied.scheme() != "https" || supplied.host().isEmpty() ||
                !supplied.userInfo().isEmpty() || supplied.hasFragment() || supplied.hasQuery() ||
                (!supplied.path().isEmpty() && supplied.path() != "/"))
                origin.clear();
        }
        return HttpResponse::json({{"active", true},
                                   {"invitation", it->secret},
                                   {"expires", it->expires},
                                   {"origin", origin},
                                   {"app", it->app},
                                   {"allow_control", it->allowControl},
                                   {"members", members}});
    });
    server.router()->post("/api/share/player/control", [=, &share, &deps](const HttpRequest& req) {
        sweep();
        const auto body = QJsonDocument::fromJson(req.body).object();
        const int slot = share.slotForToken(body.value("token").toString());
        if (!ShareManager::isPlayerSlot(slot) || share.slotForCookie(HttpServer::cookieFromRequest(
                                                     req, ShareManager::kPlayerCookie)) != slot)
            return HttpResponse::error(403, "Invitation expired.");
        auto it = invites->find(share.hostForSlot(slot));
        if (it == invites->end() || !it->openings.contains(slot) ||
            it->app == ShareManager::kChatOnlyApp)
            return HttpResponse::json({{"available", false}});
        const QString action = body.value("action").toString();
        const QString opening = it->openings.value(slot);
        const bool holding = share.permissions(slot).keyboardMouse;
        if (action == "request") {
            if (!it->allowControl) return HttpResponse::error(403, "Guest requests are locked.");
            if (!holding) {
                if (!share.roomGuestControls(it.key()) &&
                    !(deps.ownerWatching && deps.ownerWatching(it.key()))) {
                    ShareManager::Permissions p;
                    p.keyboardMouse = true;
                    share.setPermissions(slot, p);
                } else
                    it->requests.insert(opening);
            }
        } else if (action == "release" || action == "give") {
            if (!holding)
                return HttpResponse::error(403,
                                           "Only the current controller can hand back control.");
            int next = -1;
            if (action == "give") {
                next = it->openings.key(body.value("to").toString(), -1);
                if (next < 0 || next == slot) return HttpResponse::error(409, "Participant left.");
            }
            share.setPermissions(slot, {});
            if (next >= 0) {
                ShareManager::Permissions p;
                p.keyboardMouse = true;
                share.setPermissions(next, p);
                it->requests.remove(it->openings.value(next));
            }
        } else if (action != "status")
            return HttpResponse::error(400, "Unknown control action.");
        QJsonArray others;
        if (share.permissions(slot).keyboardMouse)
            for (int other : it->openings.keys()) {
                if (other != slot)
                    others.append(QJsonObject{
                        {"opening", it->openings.value(other)},
                        {"name", share.name(other)},
                        {"requested", it->requests.contains(it->openings.value(other))}});
            }
        return HttpResponse::json({{"available", true},
                                   {"allowed", it->allowControl},
                                   {"holding", share.permissions(slot).keyboardMouse},
                                   {"pending", it->requests.contains(opening)},
                                   {"others", others}});
    });
    server.router()->post("/api/share/player/depart", [=, &share, &deps](const HttpRequest& req) {
        sweep();
        const auto body = QJsonDocument::fromJson(req.body).object();
        const int slot = share.slotForToken(body.value("token").toString());
        if (!share.isRoomGuest(slot) || share.slotForCookie(HttpServer::cookieFromRequest(
                                            req, ShareManager::kPlayerCookie)) != slot)
            return HttpResponse::error(403, "Invitation expired.");
        auto room = invites->find(share.hostForSlot(slot));
        if (room == invites->end() || !room->openings.contains(slot))
            return HttpResponse::error(403, "Room ended.");
        room->requests.remove(room->openings.take(slot));
        share.setPermissions(slot, {});
        if (deps.stopPlayerStream) deps.stopPlayerStream(slot, true);
        share.deactivate(slot, ShareManager::EndReason::OwnerToggle);
        auto response = HttpResponse::json({{"left", true}});
        response.headers["Set-Cookie"] =
            QStringLiteral("mw_player=; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=0");
        return response;
    });
    server.router()->post("/api/share/player/claim", [=, &share](const HttpRequest& req) {
        sweep();
        const auto body = QJsonDocument::fromJson(req.body).object();
        const auto secret = body.value("invite").toString();
        auto it = invites->begin();
        while (it != invites->end() && it->secret != secret)
            ++it;
        if (it == invites->end())
            return HttpResponse::error(403, "Invitation expired or sharing ended.");
        const QString name = body.value("name").toString().trimmed();
        if (name.isEmpty() || name.size() > 60 ||
            std::any_of(name.begin(), name.end(),
                        [](QChar c) { return c.category() == QChar::Other_Control; }))
            return HttpResponse::error(400, "Enter a name of up to 60 characters.");
        const QString cookie = HttpServer::cookieFromRequest(req, ShareManager::kPlayerCookie);
        const int existing = share.slotForCookie(cookie);
        if (it->openings.contains(existing)) {
            QString token, pin;
            if (share.secrets(existing, token, pin)) return HttpResponse::json({{"token", token}});
        }
        if (ShareManager::isPlayerSlot(existing))
            return HttpResponse::error(409, "This browser already has a different guest session.");
        int slot = -1;
        for (const auto& row : share.status())
            if (row.state == ShareManager::State::Off) {
                slot = row.slot;
                break;
            }
        if (slot < 0) return HttpResponse::error(409, "All three guest places are in use.");
        QString token, pin;
        ShareManager::SlotStatus st;
        if (!share.activate(slot, it.key(), it->app, kFourHours, token, pin, st, it->expires))
            return HttpResponse::error(409, "Could not join the room.");
        share.setName(slot, name);
        // The fragment invitation authorizes this one guest. Bind its fresh
        // worker slot to the browser through the existing device-cookie path.
        const auto claim = share.redeemPin(token, pin, st.opening, req.headers.value("user-agent"));
        if (claim.result != ShareManager::PinResult::Ok) {
            share.deactivate(slot, ShareManager::EndReason::OwnerToggle);
            return HttpResponse::error(409, "Could not bind the guest session.");
        }
        it->openings.insert(slot, share.opening(slot));
        auto response = HttpResponse::json({{"token", token}});
        response.headers["Set-Cookie"] =
            QStringLiteral("%1=%2; HttpOnly; Secure; Path=/; SameSite=Strict; Max-Age=%3")
                .arg(QLatin1String(ShareManager::kPlayerCookie), claim.cookie)
                .arg(it->expires - QDateTime::currentSecsSinceEpoch());
        return response;
    });
}
