#pragma once
#include <mutex>
#include <string>
#include "record-types.hh"
#include "sclasses.hh"
#include "notifiers.hh"
#include "sol/sol.hpp"
#include "nlohmann/json.hpp"
#include <fmt/chrono.h>
#include <fmt/ranges.h>
#include "sqlwriter.hh"
using namespace std;

extern sol::state g_lua;

void initLua();

struct CheckResult
{
  CheckResult() {}
  CheckResult(const char* reason) : d_reason(reason) {}
  CheckResult(const std::string& reason) : d_reason(reason) {}
  std::string d_reason;
};

extern std::vector<std::shared_ptr<Notifier>> g_notifiers;

class Checker
{
public:
  Checker(sol::table& data, int minFailures = -1) 
  {
    if(minFailures >= 0)
      d_minfailures = minFailures;
    d_minfailures = data.get_or("minFailures", d_minfailures);
    d_failurewin =  data.get_or("failureWindow", d_failurewin);

    data["subject"] = sol::lua_nil;
    data["minFailures"] = sol::lua_nil;
    data["failureWindow"] = sol::lua_nil;
    // bake in
    notifiers = g_notifiers;
  }
  Checker(const Checker&) = delete;
  virtual CheckResult perform() = 0;
  virtual std::string getDescription() = 0;
  virtual std::string getCheckerName() = 0;
  std::map<std::string, SQLiteWriter::var_t> d_attributes;
  std::map<std::string, std::map<std::string, SQLiteWriter::var_t>> d_results;

  CheckResult getStatus() 
  {
    std::lock_guard<std::mutex> l(d_m);
    return d_status;
  }
  void setStatus(const CheckResult& cr) 
  {
    std::lock_guard<std::mutex> l(d_m);
    d_status = cr; 
  }
  
  int d_minfailures=1;
  int d_failurewin = 120;

  std::vector<std::shared_ptr<Notifier>> notifiers;

private:
  CheckResult d_status;
  std::mutex d_m;
};

// sets alert status if there have been more than x alerts in y seconds
struct CheckResultFilter
{
  explicit CheckResultFilter(int maxseconds=3600) : d_maxseconds(maxseconds) {}
  void reportResult(Checker* source, const std::string& cr, time_t t)
  {
    d_reports[source][cr].insert(t);
  }
  void reportResult(Checker* source, const std::string& cr)
  {
    reportResult(source, cr, time(nullptr));
  }

  std::set<pair<Checker*, std::string>> getFilteredResults();

  map<Checker*, map<std::string, std::set<time_t>> > d_reports;
  
  int d_maxseconds;
};



class DNSChecker : public Checker
{
public:
  DNSChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "dns"; }
  std::string getDescription() override
  {
    return fmt::format("DNS check, server {}, qname {}, qtype {}, acceptable: {}",
                       d_nsip.toStringWithPort(), d_qname.toString(), toString(d_qtype), d_acceptable);
  }
private:
  ComboAddress d_nsip;
  DNSName d_qname;
  DNSType d_qtype;
  std::set<std::string> d_acceptable;
  bool d_rd = true;
};

class RRSIGChecker : public Checker
{
public:
  RRSIGChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "rrsig"; }
  std::string getDescription() override
  {
    return fmt::format("RRSIG check, server {}, qname {}, qtype {}, minDays: {}",
                       d_nsip.toStringWithPort(), d_qname.toString(), toString(d_qtype), d_minDays);
  }

private:
  ComboAddress d_nsip;
  DNSName d_qname;
  DNSType d_qtype;
  int d_minDays=0;
};


class DNSSOAChecker : public Checker
{
public:
  DNSSOAChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "dnssoa"; }
  std::string getDescription() override
  {
    std::vector<string> servers;
    for(const auto& s : d_servers) servers.push_back(s.toStringWithPort());
    return fmt::format("DNS SOA check, servers {}, domain {}",
                       servers, d_domain.toString());
  }

private:
  DNSName d_domain;
  std::set<ComboAddress> d_servers;
};


class TCPPortClosedChecker : public Checker
{
public:
  TCPPortClosedChecker(const std::set<std::string>& servers,
             const std::set<int>& ports);
  TCPPortClosedChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "tcpportclosed"; }
  std::string getDescription() override
  {
    std::vector<string> servers;
    for(const auto& s : d_servers) servers.push_back(s.toString());
    return fmt::format("TCP closed check, servers {}, ports {}",
                       servers, d_ports);
  }

  
private:
  std::set<ComboAddress> d_servers;
  std::set<int> d_ports;
};


class PINGChecker : public Checker
{
public:
  PINGChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "ping"; }
  std::string getDescription() override
  {
    std::vector<string> servers;
    for(const auto& s : d_servers) servers.push_back(s.toString());
    return fmt::format("PING check, servers {}", servers);

  }

private:
  std::set<ComboAddress> d_servers;
};


class HTTPSChecker : public Checker
{
public:
  HTTPSChecker(sol::table data);
  ~HTTPSChecker()
  {
  }
  CheckResult perform() override;
  std::string getCheckerName() override { return "https"; }
  std::string getDescription() override
  {
    return fmt::format("HTTPS check, URL {}, method {}",
                       d_url, d_method); // XX needs more
  }

private:
  std::string d_url;
  int d_maxAgeMinutes = 0;
  unsigned int d_minBytes = 0;
  unsigned int d_minCertDays = 14;
  std::optional<ComboAddress> d_serverIP;
  std::string d_method;
};

class HTTPRedirChecker : public Checker
{
public:
  HTTPRedirChecker(sol::table data);
  CheckResult perform() override;
  std::string getCheckerName() override { return "redir"; }
  std::string getDescription() override
  {
    return fmt::format("HTTP(s) redir check, from {}, to{}",
                       d_fromhostpart+d_frompath, d_tourl);
  }

private:
  std::string d_fromhostpart, d_frompath, d_tourl;
};

extern std::vector<std::unique_ptr<Checker>> g_checkers;

void checkLuaTable(sol::table data,
                   const std::set<std::string>& mandatory,
                   const std::set<std::string>& opt = std::set<std::string>());

void startWebService();
void giveToWebService(const std::set<pair<Checker*, std::string>>&);
