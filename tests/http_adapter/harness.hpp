#pragma once
#include "web_service.hpp"
namespace fixture {
extern gateway::Config initial;
extern uint64_t now,clock_step,manual_utc;
extern unsigned writes,commands,cleared_history,syslog_tests,restarts;
extern unsigned fail_error_allocations;
extern unsigned fail_config_allocations;
extern size_t history_limit;
extern ups::Snapshot snapshot;
extern ups::Command last_command;
extern ups::CommandArgs last_args;
extern std::vector<gateway::Event> events;
extern httpd_config_t server_config;
extern std::vector<httpd_uri_t> registered;
httpd_req_t request(httpd_method_t method,const std::string& target,const std::string& body="",const std::string& cookie="",const std::string& csrf="",bool activity=false);
}
