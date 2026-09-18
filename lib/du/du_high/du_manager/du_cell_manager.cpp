// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "du_cell_manager.h"
#include "ocudu/support/stage2_trace.h"
#include "converters/asn1_sys_info_packer.h"
#include "converters/scheduler_configuration_helpers.h"
#include "ocudu/du/du_cell_config_validation.h"
#include "ocudu/du/du_high/du_manager/du_configurator.h"
#include "ocudu/mac/mac_cell_manager.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/ran/band_helper.h"

using namespace ocudu;
using namespace odu;

du_cell_manager::du_cell_manager(const du_manager_params& cfg_) :
  cfg(cfg_), logger(ocudulog::fetch_basic_logger("DU-MNG"))
{
}

static void fill_si_scheduler_config(si_scheduling_update_request&        req,
                                     const du_cell_config&                cell_cfg,
                                     const byte_buffer&                   sib1,
                                     span<const bcch_dl_sch_payload_type> si_messages)
{
  const units::bytes                           sib1_len = units::bytes{static_cast<unsigned>(sib1.length())};
  static_vector<units::bytes, MAX_SI_MESSAGES> si_payload_sizes;
  for (const auto& si_msg : si_messages) {
    size_t si_msg_len = si_msg.front().length();
    // If the SI message has multiple segments, check that all segments have the same length.
    if (si_msg.size() > 1) {
      if (!std::all_of(si_msg.begin(), si_msg.end(), [si_msg_len](const byte_buffer& si_msg_) {
            return si_msg_.length() == si_msg_len;
          })) {
        report_error("All segments of an SI message must have the same length.");
      }
    }
    si_payload_sizes.emplace_back(units::bytes{static_cast<unsigned>(si_msg_len)});
  }
  req.si_sched_cfg = make_si_scheduling_info_config(cell_cfg, sib1_len, si_payload_sizes);
}

void du_cell_manager::add_cell(const du_cell_config& cell_cfg)
{
  // Verify that DU cell configuration is valid. Abort application otherwise.
  auto ret = is_du_cell_config_valid(cell_cfg);
  if (not ret.has_value()) {
    report_error("ERROR: Invalid DU Cell Configuration. Cause: {}.\n", ret.error());
  }

  // Generate system information.
  std::vector<bcch_dl_sch_payload_type> bcch_msgs = asn1_packer::pack_all_bcch_dl_sch_msgs(cell_cfg);

  ocudu_assert(bcch_msgs[0].size() == 1, "SIB-1 cannot be segmented");
  const byte_buffer& sib1 = bcch_msgs[0].front();

  span<const bcch_dl_sch_payload_type> si_messages =
      span<const bcch_dl_sch_payload_type>(bcch_msgs).last(bcch_msgs.size() - 1);

  // Generate Scheduler SI scheduling config.
  si_scheduling_update_request si_sched_req;
  si_sched_req.cell_index = to_du_cell_index(cells.size());
  si_sched_req.version    = 0;
  fill_si_scheduler_config(si_sched_req, cell_cfg, sib1, si_messages);

  // Save config.
  du_cell_context& cell = *cells.emplace_back(std::make_unique<du_cell_context>());
  cell.cfg              = cell_cfg;
  cell.state            = du_cell_context::state_t::inactive;
  cell.si_cfg.sib1      = sib1.copy();
  cell.si_cfg.si_messages.assign(si_messages.begin(), si_messages.end());
  cell.si_cfg.si_sched_cfg           = std::move(si_sched_req);
  cell.si_cfg.sib1_contains_hypersfn = cell_cfg.ran.init_bwp.paging.edrx_enabled;
}

expected<du_cell_reconfig_result>
du_cell_manager::handle_cell_reconf_request(const du_cell_param_config_request& req) const
{
  if (!req.nr_cgi.has_value()) {
    logger.warning("DU Cell Reconfiguration request without NR CGI is not supported");
    return make_unexpected(default_error_t{});
  }

  du_cell_index_t cell_index = get_cell_index(req.nr_cgi.value());
  if (cell_index == INVALID_DU_CELL_INDEX) {
    logger.warning("Discarding cell {} changes. Cause: No cell with the provided CGI was found",
                   req.nr_cgi.value().nci);
    return make_unexpected(default_error_t{});
  }
  auto& cell = *cells[cell_index];

  du_cell_config& cell_cfg   = cell.cfg;
  // Validate the entire slice batch before changing any live DU configuration.
  std::vector<rrm_policy_member> requested_members;
  for (const auto& policy : req.rrm_policy_ratio_list) {
    if (policy.resource_type != rrm_policy_ratio_group::resource_type_t::prb or
        not policy.minimum_ratio.has_value() or not policy.maximum_ratio.has_value() or
        policy.dedicated_ratio.value_or(0) > *policy.minimum_ratio or
        *policy.minimum_ratio > *policy.maximum_ratio or *policy.maximum_ratio > 100 or
        policy.policy_members_list.size() != 1) {
      logger.warning("Unsupported or invalid slice policy; no policies changed");
      return make_unexpected(default_error_t{});
    }
    const auto& member = policy.policy_members_list.front();
    if (std::find(requested_members.begin(), requested_members.end(), member) != requested_members.end() or
        std::none_of(cell_cfg.rrm_policy_members.begin(), cell_cfg.rrm_policy_members.end(),
                     [&member](const auto& existing) { return existing.rrc_member == member; })) {
      logger.warning("Unknown or duplicate slice {}; no policies changed", member);
      return make_unexpected(default_error_t{});
    }
    requested_members.push_back(member);
  }
  if (requested_members.size() > MAX_SLICE_RECONF_POLICIES) {
    return make_unexpected(default_error_t{});
  }
  const unsigned current_cell_rbs = cell_cfg.ran.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  for (const auto& expected : req.expected_rrm_policy_ratio_list) {
    if (expected.policy_members_list.size() != 1 or not expected.minimum_ratio.has_value() or
        not expected.maximum_ratio.has_value() or not expected.dedicated_ratio.has_value() or
        *expected.dedicated_ratio > *expected.minimum_ratio or *expected.minimum_ratio > *expected.maximum_ratio or
        *expected.maximum_ratio > 100) {
      logger.warning("Invalid expected slice policy; no policies changed");
      return make_unexpected(default_error_t{});
    }
    const auto& member = expected.policy_members_list.front();
    auto it = std::find_if(cell_cfg.rrm_policy_members.begin(), cell_cfg.rrm_policy_members.end(),
                          [&member](const auto& existing) { return existing.rrc_member == member; });
    if (it == cell_cfg.rrm_policy_members.end() or
        it->rbs.min() != *expected.minimum_ratio * current_cell_rbs / 100 or
        it->rbs.max() != *expected.maximum_ratio * current_cell_rbs / 100 or
        it->rbs.dedicated() != *expected.dedicated_ratio * current_cell_rbs / 100) {
      logger.warning("Stale slice policy {}; no policies changed", member);
      return make_unexpected(default_error_t{});
    }
  }
  unsigned total_min_ratio_rbs = 0;
  const unsigned cell_rbs = cell_cfg.ran.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  for (const auto& existing : cell_cfg.rrm_policy_members) {
    unsigned minimum = existing.rbs.min();
    for (const auto& policy : req.rrm_policy_ratio_list) {
      if (policy.policy_members_list.front() == existing.rrc_member) {
        minimum = (*policy.minimum_ratio * cell_rbs) / 100;
      }
    }
    total_min_ratio_rbs += minimum;
  }
  if (not req.rrm_policy_ratio_list.empty() and total_min_ratio_rbs > cell_rbs) {
    logger.warning("Slice minimum reservations exceed cell resources; no policies changed");
    return make_unexpected(default_error_t{});
  }
  bool            si_updated = false;

  if (req.ssb_pwr_mod.has_value() and req.ssb_pwr_mod.value() != cell_cfg.ran.ssb_cfg.ssb_block_power) {
    // SSB power changed.
    cell_cfg.ran.ssb_cfg.ssb_block_power = req.ssb_pwr_mod.value();
    si_updated                           = true;
  }

  // Update SIB info in cell config if provided.
  if (req.new_sys_info.has_value()) {
    // Ensure si_config exists.
    if (not cell_cfg.si.si_config.has_value()) {
      logger.warning("Cell {} has no SI config, cannot update SIB", cell_index);
    } else {
      sib_type type = get_sib_info_type(req.new_sys_info.value());

      // Find existing entry for this SIB type.
      auto sib_it = std::find_if(cell_cfg.si.si_config->sibs.begin(),
                                 cell_cfg.si.si_config->sibs.end(),
                                 [type](const sib_type_info& sib) { return get_sib_info_type(sib.content) == type; });

      if (sib_it != cell_cfg.si.si_config->sibs.end()) {
        sib_it->content = req.new_sys_info.value();
        // Increment value_tag with wrapping (5-bit field: 0-31).
        sib_it->value_tag = (sib_it->value_tag.value() + 1) % 32;
        si_updated        = true;
        logger.info("Updated SIB{} in cell {} config, new value_tag={}",
                    static_cast<int>(type),
                    cell_index,
                    sib_it->value_tag.value());
      } else {
        logger.warning("Requested SIB{} update in cell {}, but entry not found.", static_cast<int>(type), cell_index);
      }
    }
  }

  const unsigned nof_prbs = cell_rbs;
  du_cell_reconfig_result result;
  result.slice_reconf_req.emplace();
  result.slice_reconf_req->cell_index = cell_index;
  result.slice_reconf_req->stage2_trace_id = req.stage2_trace_id;
  if (req.stage2_trace_id) {
    stage2::emit(fmt::format("\"event\":\"slice_target\",\"trace_id\":{},\"cell_index\":{},\"cell_plmn\":\"{}\","
                             "\"nr_cell_id\":{},\"cell_prbs\":{}",
                             req.stage2_trace_id, static_cast<unsigned>(cell_index), req.nr_cgi->plmn_id.to_string(),
                             req.nr_cgi->nci.value(), nof_prbs));
  }
  for (const auto& policy : req.rrm_policy_ratio_list) {
    const auto& member = policy.policy_members_list.front();
    const rrm_policy_ratio_rb_limits limits{policy.dedicated_ratio.value_or(0) * nof_prbs / 100,
                                            *policy.minimum_ratio * nof_prbs / 100,
                                            *policy.maximum_ratio * nof_prbs / 100};
    // Forward even unchanged policies: the scheduler is the authority for applied state.
    result.slice_reconf_req->rrm_policies.push_back({member, limits});

  }

  if (si_updated) {
    if (req.new_sys_info.has_value()) {
      // Other SIB msg was updated, repack ALL SIBs (SIB1 + SI messages).
      logger.info("Repacking all BCCH-DL-SCH messages for cell {} (SIB update)", cell_index);
      std::vector<bcch_dl_sch_payload_type> bcch_msgs = asn1_packer::pack_all_bcch_dl_sch_msgs(cell_cfg);

      ocudu_assert(bcch_msgs[0].size() == 1, "SIB-1 cannot be segmented");
      cell.si_cfg.sib1 = bcch_msgs[0].front().copy();

      span<const bcch_dl_sch_payload_type> si_messages =
          span<const bcch_dl_sch_payload_type>(bcch_msgs).last(bcch_msgs.size() - 1);
      cell.si_cfg.si_messages.assign(si_messages.begin(), si_messages.end());
    } else {
      // Only SSB power changed, repack only SIB1.
      cell.si_cfg.sib1 = asn1_packer::pack_sib1(cell_cfg);
    }

    // Bump SI version and update SI messages.
    fill_si_scheduler_config(cell.si_cfg.si_sched_cfg, cell_cfg, cell.si_cfg.sib1, cell.si_cfg.si_messages);
    cell.si_cfg.si_sched_cfg.version++;
  }

  result.cell_index           = cell_index;
  result.cu_notif_required    = si_updated;
  result.sched_notif_required = si_updated;
  if (result.slice_reconf_req->rrm_policies.empty()) {
    // No RRM policy changes.
    result.slice_reconf_req.reset();
  }
  return result;
}

async_task<bool> du_cell_manager::start(du_cell_index_t cell_index) const
{
  return launch_async([this, cell_index](coro_context<async_task<bool>>& ctx) {
    CORO_BEGIN(ctx);
    if (!has_cell(cell_index)) {
      logger.warning("cell={}: Start called for a cell that does not exist.", fmt::underlying(cell_index));
      CORO_EARLY_RETURN(false);
    }
    if (cells[cell_index]->state != du_cell_context::state_t::inactive) {
      logger.warning("cell={}: Start called for an already active cell.", fmt::underlying(cell_index));
      CORO_EARLY_RETURN(false);
    }

    // Start cell in the MAC.
    CORO_AWAIT(cfg.mac.mgr.get_cell_manager().get_cell_controller(cell_index).start());

    cells[cell_index]->state = du_cell_context::state_t::active;

    CORO_RETURN(true);
  });
}

async_task<void> du_cell_manager::stop(du_cell_index_t cell_index) const
{
  return launch_async([this, cell_index](coro_context<async_task<void>>& ctx) {
    CORO_BEGIN(ctx);

    if (!has_cell(cell_index)) {
      logger.warning("cell={}: Stop called for a cell that does not exist.", fmt::underlying(cell_index));
      CORO_EARLY_RETURN();
    }
    if (cells[cell_index]->state == du_cell_context::state_t::inactive) {
      // Ignore.
      CORO_EARLY_RETURN();
    }
    cells[cell_index]->state = du_cell_context::state_t::inactive;

    // Stop cell in the MAC.
    CORO_AWAIT(cfg.mac.mgr.get_cell_manager().get_cell_controller(cell_index).stop());

    CORO_RETURN();
  });
}

async_task<void> du_cell_manager::stop_all() const
{
  return launch_async([this, i = 0U](coro_context<async_task<void>>& ctx) mutable {
    CORO_BEGIN(ctx);

    for (; i != cells.size(); ++i) {
      if (cells[i] != nullptr and cells[i]->state == du_cell_context::state_t::active) {
        cells[i]->state = du_cell_context::state_t::inactive;

        CORO_AWAIT(cfg.mac.mgr.get_cell_manager().get_cell_controller(to_du_cell_index(i)).stop());
      }
    }

    CORO_RETURN();
  });
}

void du_cell_manager::remove_all_cells()
{
  for (unsigned i = 0; i != cells.size(); ++i) {
    ocudu_assert(cells[i] != nullptr, "Cell {} is null", i);
    ocudu_assert(cells[i]->state != du_cell_context::state_t::active, "Cell {} is still active", i);
    cfg.mac.mgr.get_cell_manager().remove_cell(to_du_cell_index(i));
  }
  cells.clear();
}

du_cell_index_t du_cell_manager::get_cell_index(nr_cell_global_id_t nr_cgi) const
{
  du_cell_index_t cell_index = du_cell_index_t::INVALID_DU_CELL_INDEX;
  for (unsigned i = 0, e = nof_cells(); i != e; ++i) {
    const du_cell_config& cell_it = get_cell_cfg(to_du_cell_index(i));
    if (cell_it.nr_cgi == nr_cgi) {
      cell_index = to_du_cell_index(i);
      break;
    }
  }
  return cell_index;
}

du_cell_index_t du_cell_manager::get_cell_index(pci_t pci) const
{
  du_cell_index_t cell_index = du_cell_index_t::INVALID_DU_CELL_INDEX;
  for (unsigned i = 0, e = nof_cells(); i != e; ++i) {
    const du_cell_config& cell_it = get_cell_cfg(to_du_cell_index(i));
    if (cell_it.ran.pci == pci) {
      cell_index = to_du_cell_index(i);
      break;
    }
  }
  return cell_index;
}

void du_cell_manager::commit_slice_config(const du_cell_slice_reconfig_request& request)
{
  auto& policies = cells[request.cell_index]->cfg.rrm_policy_members;
  for (const auto& applied : request.rrm_policies) {
    for (auto& policy : policies) {
      if (policy.rrc_member == applied.rrc_member) policy.rbs = applied.rbs;
    }
  }
}
