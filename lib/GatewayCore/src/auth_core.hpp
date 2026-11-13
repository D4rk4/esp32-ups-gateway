#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace gateway { struct Config; namespace auth {
enum class Role : uint8_t { None=0, Viewer=1, Operator=2, Admin=3 };
const char* roleName(Role role);
struct Account { bool enabled;Role role;std::string_view username,password; };
struct Session {
  std::string token,csrf,username;
  Role role{Role::None};
  uint64_t last_activity_ms{0};
};
struct Callbacks {
  void* context;
  uint64_t (*clock)(void*);
  bool (*token)(void*,std::string&);
};
enum class LoginState { Ok, InvalidCredentials, Throttled, Capacity, TokenFailure };
struct LoginResult { LoginState state{LoginState::InvalidCredentials};uint32_t retry_after_s{0}; };
// All storage has a fixed capacity. Callers serialize access; the clock is
// monotonic milliseconds and tokens must contain 32 random bytes as hex.
class SessionStore {
 public:
  static constexpr size_t kCapacity=6;
  static constexpr size_t kThrottleCapacity=8;
  SessionStore(uint64_t idle_timeout_ms,Callbacks callbacks);
  LoginResult login(const std::array<Account,3>& accounts,std::string_view username,
                    std::string_view password,const std::string& peer,Session& result);
  const Session* find(const std::string& token,bool user_activity=false);
  bool csrfValid(const std::string& token,const std::string& csrf);
  void logout(const std::string& token);
  void invalidateAll();
 private:
  struct Failure { std::string peer;uint64_t since{0};uint8_t count{0}; };
  uint64_t now() const;
  void expire(uint64_t timestamp);
  uint32_t retryAfter(const std::string& peer,uint64_t timestamp);
  void recordFailure(const std::string& peer,uint64_t timestamp);
  uint64_t timeout_ms_;
  Callbacks callbacks_;
  std::array<Session,kCapacity> sessions_{};
  std::array<Failure,kThrottleCapacity> failures_{};
  uint64_t global_since_{0};uint8_t global_failures_{0};
};
enum class Method { Get, Post };
enum class Endpoint {
  Root,Session,Login,Logout,Status,Config,ConfigExport,Events,EventsCsv,
  Capabilities,Alarms,History,HistoryCsv,Time,Schedules,Notifications,About,
  Command,TrapTest,SyslogTest,Reboot,LogsClear,ResetConfig,Firmware
};
struct Route {
  Method method;const char* path;Endpoint id;Role minimum;bool csrf;size_t max_body;
};
const std::array<Route,27>& routes();
const Route* routeFor(Method method,const std::string& path);
enum class Access { Allowed, Unauthenticated, Forbidden, InvalidCsrf };
Access authorize(const Route& route,const Session* session,bool csrf_valid);
struct Query { std::string key,value; };
struct RequestTarget {
  std::string path;std::array<Query,8> query{};size_t count{0};
  const std::string* get(const std::string& key) const;
};
bool parseTarget(const std::string& uri,RequestTarget& result,std::string& error);
bool isSingleJsonObject(const std::string& body);
std::string extractSessionCookie(const std::string& cookie);
std::string csvCell(std::string_view text);
bool powerConfirmationRequired(const Config& saved,const Config& candidate);
}}
