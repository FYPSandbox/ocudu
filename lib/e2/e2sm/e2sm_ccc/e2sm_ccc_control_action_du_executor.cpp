// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "e2sm_ccc_control_action_du_executor.h"
#include "ocudu/support/format/fmt_to_c_str.h"

using namespace asn1::e2ap;
using namespace asn1::e2sm;
using namespace asn1::e2sm_ccc;
using namespace ocudu;

e2sm_ccc_control_action_du_executor_base::e2sm_ccc_control_action_du_executor_base(
    odu::du_configurator& du_configurator_,
    task_executor&        continuation_exec_,
    uint32_t              action_id_) :
  logger(ocudulog::fetch_basic_logger("E2SM-CCC")),
  action_id(action_id_),
  du_param_configurator(du_configurator_),
  continuation_exec(continuation_exec_)
{
}

uint32_t e2sm_ccc_control_action_du_executor_base::get_action_id()
{
  return action_id;
}

std::string e2sm_ccc_control_action_du_executor_base::get_action_name()
{
  return ran_cfg_structure_name;
}

ran_function_definition_ctrl_action_item_s e2sm_ccc_control_action_du_executor_base::get_control_action_definition()
{
  // TODO: ran_function_definition_ctrl_action_item_s is from RC, remove from the e2sm_control_action_executor interface
  return {};
}

static async_task<e2sm_ric_control_response> return_ctrl_failure(const e2sm_ric_control_request& req)
{
  return launch_async([](coro_context<async_task<e2sm_ric_control_response>>& ctx) {
    CORO_BEGIN(ctx);
    e2sm_ric_control_response e2sm_response;
    e2sm_response.success                = false;
    e2sm_response.cause.set_misc().value = cause_misc_e::options::unspecified;
    CORO_RETURN(e2sm_response);
  });
}

static odu::du_param_config_request convert_to_du_config_request(const e2sm_ric_control_request& e2sm_ccc_req)
{
  odu::du_param_config_request du_request;
  const auto&                  e2_ctrl_msg       = std::get<ric_ctrl_msg_s>(e2sm_ccc_req.request_ctrl_msg);
  const auto&                  e2_cell_ctrl_list = e2_ctrl_msg.ctrl_msg_format.ctrl_msg_format2().list_of_cells_ctrl;
  for (auto const& e2_cell_ctrl : e2_cell_ctrl_list) {
    auto&       cell_cfg    = du_request.cells.emplace_back();
    cell_cfg.stage2_trace_id = e2sm_ccc_req.stage2_trace_id;
    const auto& e2_plmn_id  = e2_cell_ctrl.cell_global_id.nr_cgi().plmn_id;
    std::string plmn_str    = e2_plmn_id.mcc.to_string() + e2_plmn_id.mnc.to_string();
    auto        plmn_id_exp = plmn_identity::parse(plmn_str);
    if (plmn_id_exp.has_value()) {
      cell_cfg.nr_cgi.emplace();
      cell_cfg.nr_cgi->plmn_id = plmn_id_exp.value();
    }
    auto nr_cell_identity_exp = nr_cell_identity::create(e2_cell_ctrl.cell_global_id.nr_cgi().nr_cell_id.to_number());
    if (nr_cell_identity_exp.has_value()) {
      if (!cell_cfg.nr_cgi.has_value()) {
        cell_cfg.nr_cgi.emplace();
      }
      cell_cfg.nr_cgi->nci = nr_cell_identity_exp.value();
    }

    for (auto const& e2_cfg_struct : e2_cell_ctrl.list_of_cfg_structures) {
      rrm_policy_ratio_group rrm_policy;
      // Note: Here we use only new values.
      auto const& ran_cfg_structure = e2_cfg_struct.new_values_of_attributes.ran_cfg_structure;
      if (ran_cfg_structure.type() == e2_sm_ccc_ran_cfg_structure_c::types::o_rrm_policy_ratio) {
        // Translate resource type.
        if (ran_cfg_structure.o_rrm_policy_ratio().res_type_present) {
          if (ran_cfg_structure.o_rrm_policy_ratio().res_type.value == res_type_e::prb_dl) {
            rrm_policy.resource_type = rrm_policy_ratio_group::resource_type_t::prb_dl;
          } else if (ran_cfg_structure.o_rrm_policy_ratio().res_type.value == res_type_e::prb_ul) {
            rrm_policy.resource_type = rrm_policy_ratio_group::resource_type_t::prb_ul;
          }
        } else {
          rrm_policy.resource_type = rrm_policy_ratio_group::resource_type_t::prb;
        }
        // Convert RRM Policy Member List.
        if (ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_member_list.size()) {
          for (const auto& e2_member : ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_member_list) {
            auto&       member          = rrm_policy.policy_members_list.emplace_back();
            std::string mem_plmn_str    = e2_member.plmn_id.mcc.to_string() + e2_member.plmn_id.mnc.to_string();
            auto        mem_plmn_id_exp = plmn_identity::parse(mem_plmn_str);
            if (mem_plmn_id_exp.has_value()) {
              member.plmn_id = mem_plmn_id_exp.value();
            }
            if (e2_member.snssai_present) {
              if (e2_member.snssai.sd_present) {
                member.s_nssai.sd = slice_differentiator::create(e2_member.snssai.sd.to_number()).value();
              }
              if (e2_member.snssai.sst_present) {
                member.s_nssai.sst = slice_service_type{static_cast<uint8_t>(e2_member.snssai.sst)};
              }
            }
          }
        }
        // Convert Min Ratio.
        if (ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_min_ratio_present) {
          rrm_policy.minimum_ratio = ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_min_ratio;
        }
        // Convert Max Ratio.
        if (ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_max_ratio_present) {
          rrm_policy.maximum_ratio = ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_max_ratio;
        }
        // Convert Dedicated Ratio.
        if (ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_ded_ratio_present) {
          rrm_policy.dedicated_ratio = ran_cfg_structure.o_rrm_policy_ratio().rrm_policy_ded_ratio;
        }
      }
      // This implementation has shared DL/UL limits. Validation requires an identical
      // DL/UL pair; collapse it into one cell policy, never silently change one direction.
      if (rrm_policy.resource_type == rrm_policy_ratio_group::resource_type_t::prb_dl) {
        rrm_policy.resource_type = rrm_policy_ratio_group::resource_type_t::prb;
        cell_cfg.rrm_policy_ratio_list.emplace_back(rrm_policy);
        const auto& old = e2_cfg_struct.old_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio();
        rrm_policy.minimum_ratio = old.rrm_policy_min_ratio;
        rrm_policy.maximum_ratio = old.rrm_policy_max_ratio;
        rrm_policy.dedicated_ratio = old.rrm_policy_ded_ratio;
        cell_cfg.expected_rrm_policy_ratio_list.emplace_back(rrm_policy);
      }
    }
  }
  return du_request;
}

static const char* resource_type_to_string(rrm_policy_ratio_group::resource_type_t type)
{
  switch (type) {
    case rrm_policy_ratio_group::resource_type_t::prb:
      return "PRB";
    case rrm_policy_ratio_group::resource_type_t::prb_ul:
      return "PRB-UL";
    case rrm_policy_ratio_group::resource_type_t::prb_dl:
      return "PRB-DL";
    default:
      return "UNKNOWN";
  }
}

static void log_du_config_request(const ocudulog::basic_logger& logger, const odu::du_param_config_request& req)
{
  fmt::memory_buffer log_buffer;
  for (const auto& cell_cfg : req.cells) {
    fmt::format_to(std::back_inserter(log_buffer),
                   "E2SM-CCC: O-RRMPolicyRatio Control Request for NR-CGI=[plmn: {}, nci: {}]\n",
                   cell_cfg.nr_cgi.has_value() ? cell_cfg.nr_cgi.value().plmn_id.to_string() : "na",
                   cell_cfg.nr_cgi.has_value() ? cell_cfg.nr_cgi.value().nci.value() : 0U);
    fmt::format_to(std::back_inserter(log_buffer), "RRM Policy Ratio Group:\n");
    for (const auto& rrm_policy_ratio : cell_cfg.rrm_policy_ratio_list) {
      fmt::format_to(std::back_inserter(log_buffer), " RRM Policy:\n");
      fmt::format_to(std::back_inserter(log_buffer),
                     "  Resource Type: {}\n",
                     resource_type_to_string(rrm_policy_ratio.resource_type));
      fmt::format_to(std::back_inserter(log_buffer), "  Min PRB Policy Ratio: {}\n", *rrm_policy_ratio.minimum_ratio);
      fmt::format_to(std::back_inserter(log_buffer), "  Max PRB Policy Ratio: {}\n", *rrm_policy_ratio.maximum_ratio);
      fmt::format_to(
          std::back_inserter(log_buffer), "  Dedicated PRB Policy Ratio: {}\n", *rrm_policy_ratio.dedicated_ratio);
      fmt::format_to(std::back_inserter(log_buffer), "  RRM Policy Member List:\n");
      for (const auto& policy_member : rrm_policy_ratio.policy_members_list) {
        fmt::format_to(std::back_inserter(log_buffer),
                       "  - PLMN:{}, SST:{}, SD:{}\n",
                       policy_member.plmn_id.to_string(),
                       policy_member.s_nssai.sst.value(),
                       policy_member.s_nssai.sd.value());
      }
    }
  }

  logger.info("{}", to_c_str(log_buffer));
  log_buffer.clear();
}

static e2sm_ric_control_response convert_to_e2sm_response(const e2sm_ric_control_request&      e2sm_ccc_req,
                                                          const odu::du_param_config_request&  du_config_req_,
                                                          const odu::du_param_config_response& du_response_)
{
  const auto& e2_ctrl_req       = std::get<ric_ctrl_msg_s>(e2sm_ccc_req.request_ctrl_msg);
  const auto& e2_cell_ctrl_list = e2_ctrl_req.ctrl_msg_format.ctrl_msg_format2().list_of_cells_ctrl;

  e2sm_ric_control_response e2sm_response   = {};
  e2sm_response.service_model               = e2sm_service_model_t::CCC;
  e2sm_response.success                     = du_response_.success;
  e2sm_response.ric_call_process_id_present = false;
  e2sm_response.ric_ctrl_outcome_present    = true;

  if (not du_response_.success) {
    e2sm_response.cause.set_misc().value = cause_misc_e::options::unspecified;
  }

  ctrl_outcome_format_c ctrl_outcome;
  auto&                 ctrl_outcome_f2 = ctrl_outcome.set_ctrl_outcome_format2();
  ctrl_outcome_f2.rx_timestamp_present  = false;
  ctrl_outcome_f2.list_of_cells_for_ctrl_outcome.resize(e2_cell_ctrl_list.size());
  auto& outcome_cell_ctrl_list = ctrl_outcome_f2.list_of_cells_for_ctrl_outcome;
  for (unsigned i = 0, e = e2_cell_ctrl_list.size(); i < e; i++) {
    outcome_cell_ctrl_list[i].cell_global_id = e2_cell_ctrl_list[i].cell_global_id;

    if (du_response_.success) {
      // If success then all to accepted.
      for (const auto& req_struct : e2_cell_ctrl_list[i].list_of_cfg_structures) {
        cfg_structure_accepted_s cfg_structure_accepted;
        cfg_structure_accepted.ran_cfg_structure_name       = req_struct.ran_cfg_structure_name;
        cfg_structure_accepted.old_values_of_attributes     = req_struct.old_values_of_attributes;
        cfg_structure_accepted.current_values_of_attributes = req_struct.new_values_of_attributes;
        cfg_structure_accepted.applied_timestamp_present    = false;
        outcome_cell_ctrl_list[i].ran_cfg_structures_accepted_list.push_back(cfg_structure_accepted);
      }
    } else {
      for (const auto& req_struct : e2_cell_ctrl_list[i].list_of_cfg_structures) {
        cfg_structure_failed_s cfg_structure_failed;
        cfg_structure_failed.ran_cfg_structure_name         = req_struct.ran_cfg_structure_name;
        cfg_structure_failed.old_values_of_attributes       = req_struct.old_values_of_attributes;
        cfg_structure_failed.requested_values_of_attributes = req_struct.new_values_of_attributes;
        cfg_structure_failed.cause                          = cause_opts::unspecified;
        outcome_cell_ctrl_list[i].ran_cfg_structures_failed_list.push_back(cfg_structure_failed);
      }
    }
  }
  e2sm_response.ric_ctrl_outcome = ctrl_outcome;

  return e2sm_response;
}

e2sm_ccc_control_o_rrm_policy_ratio_executor::e2sm_ccc_control_o_rrm_policy_ratio_executor(
    odu::du_configurator& du_configurator_,
    task_executor&        continuation_exec_) :
  e2sm_ccc_control_action_du_executor_base(du_configurator_, continuation_exec_, 6)
{
  // RAN Configuration Structure description:
  ran_cfg_structure_name = "O-RRMPolicyRatio";
  attributes.emplace_back("resourceType");
  attributes.emplace_back("rRMPolicyMemberList");
  attributes.emplace_back("rRMPolicyMaxRatio");
  attributes.emplace_back("rRMPolicyMinRatio");
  attributes.emplace_back("rRMPolicyDedicatedRatio");
}

bool e2sm_ccc_control_o_rrm_policy_ratio_executor::ric_control_action_supported(const e2sm_ric_control_request& req)
{
  const auto& ctrl_hdr = std::get<ric_ctrl_hdr_s>(req.request_ctrl_hdr);
  const auto& ctrl_msg = std::get<ric_ctrl_msg_s>(req.request_ctrl_msg);

  int64_t ric_style_type = ctrl_hdr.ctrl_hdr_format.ctrl_hdr_format1().ric_style_type;
  if (ric_style_type != 2) {
    return false;
  }

  if (ctrl_msg.ctrl_msg_format.type() != ctrl_msg_format_c::types::ctrl_msg_format2) {
    return false;
  }

  const auto& cells = ctrl_msg.ctrl_msg_format.ctrl_msg_format2().list_of_cells_ctrl;
  // Atomicity is supported within one cell, not across cells.
  if (cells.size() != 1 or cells[0].cell_global_id.type() != cell_global_id_c::types::nr_cgi) {
    return false;
  }
  const auto& cgi = cells[0].cell_global_id.nr_cgi();
  if (not plmn_identity::parse(cgi.plmn_id.mcc.to_string() + cgi.plmn_id.mnc.to_string()).has_value()) {
    return false;
  }
  const auto& structures = cells[0].list_of_cfg_structures;
  if (structures.size() == 0 or structures.size() > 2 * MAX_SLICE_RECONF_POLICIES) {
    return false;
  }
  for (const auto& structure : structures) {
    if (structure.ran_cfg_structure_name.to_string() != ran_cfg_structure_name or
        structure.new_values_of_attributes.ran_cfg_structure.type() !=
            e2_sm_ccc_ran_cfg_structure_c::types::o_rrm_policy_ratio) {
      return false;
    }
    const auto& policy = structure.new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio();
    if (not policy.res_type_present or
        (policy.res_type != res_type_opts::prb_dl and policy.res_type != res_type_opts::prb_ul) or
        not policy.rrm_policy_min_ratio_present or not policy.rrm_policy_max_ratio_present or
        not policy.rrm_policy_ded_ratio_present or policy.rrm_policy_member_list.size() != 1 or
        policy.rrm_policy_ded_ratio < 0 or policy.rrm_policy_ded_ratio > policy.rrm_policy_min_ratio or
        policy.rrm_policy_min_ratio > policy.rrm_policy_max_ratio or policy.rrm_policy_max_ratio > 100) {
      return false;
    }
    if (structure.old_values_of_attributes.ran_cfg_structure.type() !=
        e2_sm_ccc_ran_cfg_structure_c::types::o_rrm_policy_ratio) {
      return false;
    }
    const auto& old = structure.old_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio();
    nlohmann::ordered_json old_json = old;
    nlohmann::ordered_json new_json = policy;
    for (const char* key : {"rRMPolicyMinRatio", "rRMPolicyMaxRatio", "rRMPolicyDedicatedRatio"}) {
      old_json.erase(key);
      new_json.erase(key);
    }
    if (old_json != new_json or not old.rrm_policy_min_ratio_present or not old.rrm_policy_max_ratio_present or
        not old.rrm_policy_ded_ratio_present or old.rrm_policy_ded_ratio < 0 or
        old.rrm_policy_ded_ratio > old.rrm_policy_min_ratio or
        old.rrm_policy_min_ratio > old.rrm_policy_max_ratio or old.rrm_policy_max_ratio > 100) {
      return false;
    }
    const auto& member = policy.rrm_policy_member_list[0];
    if (not member.plmn_id_present or not member.snssai_present or not member.snssai.sst_present or
        member.snssai.sst < 0 or member.snssai.sst > 255 or
        not plmn_identity::parse(member.plmn_id.mcc.to_string() + member.plmn_id.mnc.to_string()).has_value() or
        (member.snssai.sd_present and not slice_differentiator::create(member.snssai.sd.to_number()).has_value())) {
      return false;
    }
    // Exactly one instance in each direction, with identical ratios and identity.
    unsigned same_direction = 0;
    unsigned other_direction = 0;
    for (const auto& other : structures) {
      if (other.new_values_of_attributes.ran_cfg_structure.type() !=
          e2_sm_ccc_ran_cfg_structure_c::types::o_rrm_policy_ratio) {
        return false;
      }
      const auto& candidate = other.new_values_of_attributes.ran_cfg_structure.o_rrm_policy_ratio();
      if (candidate.rrm_policy_member_list.size() != 1) {
        return false;
      }
      const auto& m = candidate.rrm_policy_member_list[0];
      if (m.plmn_id.mcc.to_string() != member.plmn_id.mcc.to_string() or
          m.plmn_id.mnc.to_string() != member.plmn_id.mnc.to_string() or m.snssai.sst != member.snssai.sst or
          m.snssai.sd_present != member.snssai.sd_present or
          (m.snssai.sd_present and m.snssai.sd.to_number() != member.snssai.sd.to_number())) {
        continue;
      }
      if (candidate.rrm_policy_min_ratio != policy.rrm_policy_min_ratio or
          candidate.rrm_policy_max_ratio != policy.rrm_policy_max_ratio or
          candidate.rrm_policy_ded_ratio != policy.rrm_policy_ded_ratio) {
        return false;
      }
      const auto& candidate_old = other.old_values_of_attributes.ran_cfg_structure;
      if (candidate_old.type() != e2_sm_ccc_ran_cfg_structure_c::types::o_rrm_policy_ratio or
          candidate_old.o_rrm_policy_ratio().rrm_policy_min_ratio != old.rrm_policy_min_ratio or
          candidate_old.o_rrm_policy_ratio().rrm_policy_max_ratio != old.rrm_policy_max_ratio or
          candidate_old.o_rrm_policy_ratio().rrm_policy_ded_ratio != old.rrm_policy_ded_ratio) {
        return false;
      }
      candidate.res_type == policy.res_type ? ++same_direction : ++other_direction;
    }
    if (same_direction != 1 or other_direction != 1) {
      return false;
    }
  }
  return true;
}

async_task<e2sm_ric_control_response>
e2sm_ccc_control_o_rrm_policy_ratio_executor::execute_ric_control_action(const e2sm_ric_control_request& req)
{
  odu::du_param_config_request du_ctrl_config_req = convert_to_du_config_request(req);

  for (const auto& cell_cfg : du_ctrl_config_req.cells) {
    // If empty request, return failure.
    if (cell_cfg.rrm_policy_ratio_list.empty()) {
      return return_ctrl_failure(req);
    }
    // If any policy is missing members, return failure.
    for (const auto& policy : cell_cfg.rrm_policy_ratio_list) {
      if (policy.policy_members_list.empty()) {
        return return_ctrl_failure(req);
      }
    }
    // If PRB quota not provided, return failure.
    for (const auto& policy : cell_cfg.rrm_policy_ratio_list) {
      if (!policy.minimum_ratio.has_value()) {
        return return_ctrl_failure(req);
      }
      if (!policy.maximum_ratio.has_value()) {
        return return_ctrl_failure(req);
      }
      if (!policy.dedicated_ratio.has_value()) {
        return return_ctrl_failure(req);
      }
    }
  }
  // Log received control request.
  log_du_config_request(logger, du_ctrl_config_req);

  return launch_async(
      [this, req, ctrl_config = std::move(du_ctrl_config_req), ctrl_response = odu::du_param_config_response{}](
          coro_context<async_task<e2sm_ric_control_response>>& ctx) mutable {
        CORO_BEGIN(ctx);
        // Use the non-blocking variant to avoid stalling the E2 agent's execution context (and therefore its ability
        // to keep receiving/processing further RIC Control Requests) while the DU manager applies the configuration.
        // The blocking "handle_sync_operator_config" must not be called from here.
        // Note: "req" is captured by value, not by reference, because the coroutine genuinely suspends here, and the
        // caller-owned request object is not guaranteed to outlive the suspension.
        CORO_AWAIT_VALUE(ctrl_response, du_param_configurator.handle_operator_config(ctrl_config, continuation_exec));
        CORO_RETURN(convert_to_e2sm_response(req, ctrl_config, ctrl_response));
      });
}
