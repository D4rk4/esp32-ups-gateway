#pragma once
#include "web_service.hpp"
namespace fixture {
extern gateway::Config initial;
extern uint64_t now,clock_step,manual_utc;
extern unsigned writes,commands,cleared_history,syslog_tests,restarts;
extern gateway::ControlStatus control;
extern unsigned ota_begins,ota_writes,ota_ends,ota_aborts,ota_switches,ota_mark_valid,ota_mark_invalid;
extern int ota_running_state;
extern bool ota_running_second;
extern bool ota_state_available,ota_mark_valid_fails;
extern bool ota_available,ota_fail_begin,ota_fail_write,ota_fail_end,ota_fail_switch,ota_fail_verify;
extern size_t ota_verified_length;
extern size_t ota_limit,ota_max_chunk;
extern std::string ota_written;
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
httpd_req_t firmwareRequest(const std::string& body,const std::string& cookie,const std::string& csrf,const std::string& type="application/octet-stream",const std::string& confirm="UPDATE",size_t declared_length=static_cast<size_t>(-1));
}
