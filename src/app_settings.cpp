#include "appserver.hpp"
#include "app_util.hpp"
#include "log.hpp"
#include "session.hpp"

namespace im {

void AppServer::onSettingsGet(const std::shared_ptr<Session>& session, const json& data) {
  int allowTemp = 0, searchable = 0, allowAdd = 0;
  const int rc = userStore_->fetchSettings(session->userId(), &allowTemp, &searchable, &allowAdd);
  if (rc != 0) {
    session->reply(makeResult("settings.get_res", 500, "服务器内部错误", json::object()));
    return;
  }
  session->reply(makeResult("settings.get_res", 0, "ok",
      json{{"allowTempChat", allowTemp == 1},
           {"searchable", searchable == 1},
           {"allowAdd", allowAdd == 1}}));
}

void AppServer::onSettingsSet(const std::shared_ptr<Session>& session, const json& data) {
  const int allowTemp = data.contains("allowTempChat") ? (data["allowTempChat"].get<bool>() ? 1 : 0) : -1;
  const int searchable = data.contains("searchable") ? (data["searchable"].get<bool>() ? 1 : 0) : -1;
  const int allowAdd = data.contains("allowAdd") ? (data["allowAdd"].get<bool>() ? 1 : 0) : -1;
  userStore_->updateSettings(session->userId(), allowTemp, searchable, allowAdd);
  session->reply(makeResult("settings.set_res", 0, "ok", json::object()));
}

} // namespace im
