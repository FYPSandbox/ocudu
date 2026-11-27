// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "inter_slice_scheduler.h"
#include "ocudu/support/stage2_trace.h"
#include "../config/time_domain_mapper.h"
#include "../policy/scheduler_policy_factory.h"
#include "../logging/cell_metrics_handler.h"
#include "../support/mcs_tbs_calculator.h"
#include "../ue_context/ue_cell.h"
#include "../ue_scheduling/grant_params_selector.h"
#include "../ue_scheduling/ue_cell_grid_allocator.h"
#include "ocudu/ocudulog/ocudulog.h"

using namespace ocudu;

inter_slice_scheduler::inter_slice_scheduler(const cell_configuration& cell_cfg_,
                                             ue_repository&            ues_,
                                             cell_metrics_handler&     metrics_) :
  metrics(metrics_),
  cell_cfg(cell_cfg_),
  logger(ocudulog::fetch_basic_logger("SCHED")),
  ues(ues_)
{
  // Create a number of slices equal to the number of configured RRM Policy members + 1 (default SRB slice) + 1 (default
  // DRB slice).
  unsigned capacity = cell_cfg.rrm_policy_members.size() + 2;
  dl_prio_queue.reserve(capacity);
  // We use a larger capacity for the UL priority queue to account for the maximum number of PUSCH TD resources.
  ul_prio_queue.reserve(capacity * pusch_constants::MAX_NOF_PUSCH_TD_RES_ALLOCS);
  slices.reserve(capacity);

  // NOTE: We assume nof. CRBs in a cell for both DL and UL are same.
  const unsigned cell_max_rbs = cell_cfg.params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();

  // Create RAN slice instances.
  // > Default SRB slice.
  // NOTE: We set \c min_prb for default SRB slice to maximum nof. PRBs of a UE carrier to give maximum priority to this
  // slice.
  // TODO: Once test mode does not depend on SRB1, use a more suitable policy for SRBs.
  slices.emplace_back(
      SRB_RAN_SLICE_ID,
      cell_cfg,
      slice_rrm_policy_config{.rbs = {cell_max_rbs, cell_max_rbs}, .priority = slice_rrm_policy_config::max_priority},
      create_scheduler_strategy(cell_cfg.expert_cfg.ue.policy_cfg, cell_cfg),
      ues);
  // > Default DRB slice.
  slices.emplace_back(DEFAULT_DRB_RAN_SLICE_ID,
                      cell_cfg,
                      slice_rrm_policy_config{.rbs = {0, cell_max_rbs}},
                      create_scheduler_strategy(cell_cfg.expert_cfg.ue.policy_cfg, cell_cfg),
                      ues);
  // NOTE: RAN slice IDs 0 and 1 are reserved for default SRB and default DRB slice respectively.
  ran_slice_id_t id_count{2};
  // Configured RRM policy members.
  for (const slice_rrm_policy_config& rrm : cell_cfg.rrm_policy_members) {
    auto rrm_adjusted = rrm;
    // Adjust maximum PRBs per slice based on the number of PRBs in a cell.
    rrm_adjusted.rbs      = {std::min(rrm_adjusted.rbs.dedicated(), cell_max_rbs),
                             std::min(rrm_adjusted.rbs.min(), cell_max_rbs),
                             std::min(rrm_adjusted.rbs.max(), cell_max_rbs)};
    rrm_adjusted.priority = std::min(rrm.priority, slice_rrm_policy_config::max_priority);
    // Create custom RAN slice based on the RRM policy.
    slices.emplace_back(
        id_count, cell_cfg, rrm_adjusted, create_scheduler_strategy(rrm.policy_sched_cfg, cell_cfg), ues);
    ++id_count;
  }
}

void inter_slice_scheduler::slot_indication(slot_point slot_tx, const cell_resource_allocator& res_grid)
{
  // Update the context of each slice.
  if (not current_slot.valid()) {
    // First call.
    current_slot = slot_tx - 1;
  }
  while (current_slot != slot_tx) {
    ++current_slot;
    for (auto& slice : slices) {
      slice.inst.slot_indication(current_slot);
    }
  }

  // Snapshot per-slice queues before any slice grants. This is deliberately
  // separate from PRB-used counters and from new offered-traffic demand.
  for (size_t slice_index = 2; slice_index < slices.size(); ++slice_index) {
    uint64_t dl_bytes = 0;
    uint64_t ul_bytes = 0;
    for (const auto& user : slices[slice_index].inst.get_ues()) {
      dl_bytes += user.pending_dl_newtx_bytes();
      ul_bytes += user.pending_ul_newtx_bytes();
    }
    metrics.handle_slice_pending_bytes(slice_index - 2,
                                       dl_bytes,
                                       ul_bytes,
                                       cell_cfg.is_dl_enabled(slot_tx),
                                       cell_cfg.is_ul_enabled(slot_tx));
  }
  sample_queue_service_need(slot_tx, res_grid);
  collect_offered_arrivals(slot_tx);

  // Clear the priority queues.
  dl_prio_queue.clear();
  ul_prio_queue.clear();

  if (not cell_cfg.is_dl_enabled(slot_tx)) {
    // If PDCCH is inactive in this slot, we don't generate any slice candidates.
    return;
  }

  // Recompute the priority queues.
  const bool pdsch_enabled =
      cell_cfg.expert_cfg.ue.enable_csi_rs_pdsch_multiplexing or res_grid[0].result.dl.csi_rs.empty();
  for (const auto& slice : slices) {
    if (not pdsch_enabled or slice.inst.pdsch_rb_count >= slice.inst.cfg.rbs.max()) {
      // PDSCH is disabled or slice already reached max RBs. We can skip this slice.
      continue;
    }

    interval<unsigned> rb_lims;
    // When minRB > 0, minRB != maxRB, sliceRBs < min_RB, we create two candidates. One with limit set to minRB
    // with high priority, and another one with limit set to maxRB with normal priority.
    if (slice.inst.pdsch_rb_count < slice.inst.cfg.rbs.min() and slice.inst.cfg.rbs.min() > 0 and
        slice.inst.cfg.rbs.shared() > 0) {
      // NOTE: slice.inst.pdsch_rb_count is always 0 after running slot_indication() for each slice at the beginning
      // of this function; this is at least valid as long as k0 = 0. Even though pdsch_rb_count = 0, we still keep it
      // in the rb_lim computation for it to be ready when we support k0 != 0.
      rb_lims         = {slice.inst.pdsch_rb_count, slice.inst.cfg.rbs.min()};
      const auto prio = slice.get_prio(true, slot_tx, slot_tx, rb_lims.stop());
      dl_prio_queue.push(slice_candidate_context{slice.inst.id, prio, rb_lims, slot_tx});
      rb_lims = {slice.inst.cfg.rbs.min(), slice.inst.cfg.rbs.max()};
    } else {
      rb_lims = {slice.inst.pdsch_rb_count, slice.inst.cfg.rbs.max()};
    }
    const auto prio = slice.get_prio(true, slot_tx, slot_tx, rb_lims.stop());
    dl_prio_queue.push(slice_candidate_context{slice.inst.id, prio, rb_lims, slot_tx});
  }
  dl_prio_queue.sort();

  // TODO: Revisit when PUSCH time domain resource list is also defined in UE dedicated configuration.
  span<const pusch_time_domain_resource_allocation> pusch_time_domain_list =
      cell_cfg.params.ul_cfg_common.init_ul_bwp.pusch_cfg_common.value().pusch_td_alloc_list;
  for (const auto& slice : slices) {
    std::optional<unsigned> allocated_k2;
    for (const uint8_t pusch_td_res_idx : cell_cfg.init_bwp.ul.td_mapper().pusch_td_res_indices(slot_tx.count())) {
      unsigned pusch_delay = pusch_time_domain_list[pusch_td_res_idx].k2 + cell_cfg.ntn_cs_koffset;
      const cell_slot_resource_allocator& pusch_alloc = res_grid[pusch_delay];
      slot_point                          pusch_slot  = slot_tx + pusch_delay;

      unsigned pusch_rb_count = slice.inst.nof_pusch_rbs_allocated(pusch_slot);
      if (pusch_rb_count >= slice.inst.cfg.rbs.max()) {
        // Slice reached max RBs.
        continue;
      }

      if (allocated_k2.has_value() and allocated_k2.value() == pusch_time_domain_list[pusch_td_res_idx].k2) {
        // There is already a slice candidate for the same k2. Adding another candidate for the same k2 and different
        // symbols would result in repeating the same slice candidates several times.
        continue;
      }

      const crb_bitmap pusch_used_crbs =
          pusch_alloc.ul_res_grid.used_crbs(cell_cfg.params.ul_cfg_common.init_ul_bwp.generic_params.scs,
                                            cell_cfg.params.ul_cfg_common.init_ul_bwp.generic_params.crbs,
                                            pusch_time_domain_list[pusch_td_res_idx].symbols);
      if (pusch_used_crbs.all()) {
        // No more RBs left to allocated so skip adding slice candidate.
        continue;
      }

      // Scale slice RB limits to account for RBs already occupied by other UL channels (e.g. PUCCH) before PUSCH
      // scheduling. Without scaling, the overhead falls entirely on the last-scheduled slice, breaking the
      // dedicated-PRB ratio between slices.
      const unsigned cell_nof_rbs    = cell_cfg.params.ul_cfg_common.init_ul_bwp.generic_params.crbs.length();
      const unsigned pusch_avail_rbs = cell_nof_rbs - pusch_used_crbs.count();
      // TODO: Refactor to use slice ratios from config to avoid conversation from ratio to PRBs and again to ratio.
      const unsigned scaled_max = slice.inst.cfg.rbs.max() * pusch_avail_rbs / cell_nof_rbs;
      if (scaled_max <= pusch_rb_count) {
        // Slice already has at or above its proportional share of available RBs; skip.
        continue;
      }

      interval<unsigned> rb_lims;
      // When minRB > 0, minRB != maxRB, sliceRBs < min_RB, we create two candidates. One with limit set to minRB
      // with high priority, and another one with limit set to maxRB with normal priority.
      if (slice.inst.cfg.rbs.min() > 0 and slice.inst.cfg.rbs.shared() > 0 and
          pusch_rb_count < slice.inst.cfg.rbs.min()) {
        // NOTE: keep pusch_rb_count in the rb_lims computation below; note that, after running slot_indication for
        // each slice (see above), pusch_rb_count is 0 only for k2 = min_k2; but in UL, we have other k2 values for
        // which pusch_rb_count wouldn't be 0.
        const unsigned scaled_min = slice.inst.cfg.rbs.min() * pusch_avail_rbs / cell_nof_rbs;
        rb_lims                   = {pusch_rb_count, scaled_min};
        const auto prio           = slice.get_prio(false, slot_tx, pusch_slot, rb_lims.stop());
        ul_prio_queue.push(slice_candidate_context{slice.inst.id, prio, rb_lims, pusch_slot});
        rb_lims = {scaled_min, scaled_max};
      } else {
        rb_lims = {pusch_rb_count, scaled_max};
      }
      const auto prio = slice.get_prio(false, slot_tx, pusch_slot, rb_lims.stop());
      allocated_k2.emplace(pusch_time_domain_list[pusch_td_res_idx].k2);
      ul_prio_queue.push(slice_candidate_context{slice.inst.id, prio, rb_lims, pusch_slot});
    }
  }
  ul_prio_queue.sort();
}

/// FYP: spectral efficiency of the UE's wideband CQI, read in the CQI table the UE reports with (the same report the
/// scheduler uses, see ue_cell::get_aggregation_level) and clamped to the peer domain. Empty if no CQI is available
/// or the table is not one of tables 1..3.
static std::optional<double> offered_dl_spectral_efficiency(const ue_cell& ue_cc)
{
  const csi_meas_config* csi = ue_cc.cfg().csi_meas_cfg();
  if (csi == nullptr or csi->csi_report_cfg_list.empty()) {
    return std::nullopt;
  }
  const cqi_table_t table = csi->csi_report_cfg_list.back().cqi_table.value_or(cqi_table_t::table1);
  if (table == cqi_table_t::table4) {
    return std::nullopt;
  }
  return fyp_offered::peer_se_from_cqi(ue_cc.channel_state_manager().get_wideband_cqi().value(),
                                       static_cast<unsigned>(table) + 1);
}

/// FYP: spectral efficiency of the PUSCH MCS link adaptation would choose now for a newTx.
static std::optional<double> offered_ul_spectral_efficiency(const ue_cell& ue_cc)
{
  const ue_cell_configuration& cfg   = ue_cc.cfg();
  const pusch_mcs_table        table = cfg.init_bwp().ul.pusch_ded() != nullptr ? cfg.init_bwp().ul.pusch_ded()->mcs_table
                                                                               : pusch_mcs_table::qam64;
  const bool          tp  = cfg.use_pusch_transform_precoding_dci_0_1();
  const sch_mcs_index mcs = ue_cc.link_adaptation_controller().calculate_ul_mcs(table, tp);
  return fyp_offered::peer_se(pusch_mcs_get_config(table, mcs, tp, false).get_spectral_efficiency());
}

void inter_slice_scheduler::collect_offered_arrivals(slot_point slot_tx)
{
  if (not metrics.enabled()) {
    return;
  }
  if (bsr_seen.empty()) {
    bsr_seen.resize(MAX_NOF_DU_UES);
  }
  for (size_t slice_index = 2; slice_index < slices.size(); ++slice_index) {
    const ran_slice_id_t slice_id = slices[slice_index].inst.id;
    for (const slice_ue& user : slices[slice_index].inst.get_ues()) {
      const du_ue_index_t ue_index = user.ue_index();
      if (not ues.contains(ue_index)) {
        continue;
      }
      auto&      lc = ues[ue_index].logical_channels();
      const auto dl = lc.take_dl_arrivals(slice_id);
      const auto ul = lc.take_ul_arrivals(slice_id);
      if (dl.arrived_bytes != 0 or dl.decreased_bytes != 0 or ul.net_bytes != 0 or ul.unknown) {
        const ue_cell& ue_cc = user.get_cc();
        metrics.handle_slice_ue_arrivals(slice_index - 2,
                                         ue_index,
                                         dl,
                                         dl.arrived_bytes > 0 ? offered_dl_spectral_efficiency(ue_cc) : std::nullopt,
                                         ul,
                                         ul.net_bytes != 0 ? offered_ul_spectral_efficiency(ue_cc) : std::nullopt);
      }

      // BSR age: slots since the applied-BSR count last changed, for UEs that still have UL data pending.
      bsr_seen_context& seen = bsr_seen[ue_index];
      const uint32_t    n    = lc.nof_applied_bsrs();
      if (not seen.changed.valid() or seen.count != n) {
        seen = {n, slot_tx};
      }
      if (user.pending_ul_newtx_bytes() > 0) {
        const unsigned age_slots = static_cast<unsigned>(slot_tx - seen.changed);
        metrics.handle_slice_bsr_age(slice_index - 2, age_slots / slot_tx.nof_slots_per_subframe());
      }
    }
  }
}

void inter_slice_scheduler::sample_queue_service_need(slot_point slot_tx, const cell_resource_allocator& res_grid)
{
  if (not metrics.enabled() or not cell_cfg.is_dl_enabled(slot_tx)) {
    return;
  }
  cell_harq_manager& cell_harqs = ues.cell(cell_cfg.cell_index).get_cell_harqs();

  // UL grants are chosen in a PDCCH slot for a future PUSCH slot (k2). Use
  // one representative supported PUSCH opportunity per PDCCH slot. This
  // denominator is deliberately distinct from RRU.PrbUsedUl's UL-slot count.
  slot_point  ul_target;
  const auto& ul_td_list = cell_cfg.params.ul_cfg_common.init_ul_bwp.pusch_cfg_common.value().pusch_td_alloc_list;
  for (uint8_t td_index : cell_cfg.init_bwp.ul.td_mapper().pusch_td_res_indices(slot_tx.count())) {
    const auto&    td          = ul_td_list[td_index];
    const unsigned pusch_delay = td.k2 + cell_cfg.ntn_cs_koffset;
    slot_point     target      = slot_tx + pusch_delay;
    if (cell_cfg.is_ul_enabled(target) and cell_cfg.get_nof_ul_symbol_per_slot(target) >= td.symbols.stop()) {
      ul_target = target;
      break;
    }
  }
  // Same symbol rule as intra_slice_scheduler: SRS is scheduled before slice
  // grants, and PUSCH must end before the first SRS symbol of the target slot.
  unsigned ul_symbol_stop = NOF_OFDM_SYM_PER_SLOT_NORMAL_CP;
  if (ul_target.valid()) {
    for (const auto& srs : res_grid[ul_target - slot_tx].result.ul.srss) {
      ul_symbol_stop = std::min(static_cast<unsigned>(srs.symbols.start()), ul_symbol_stop);
    }
  }

  for (size_t slice_index = 2; slice_index < slices.size(); ++slice_index) {
    const auto& slice = slices[slice_index].inst;
    const auto& slice_ues = slice.get_ues();
    for (bool dl : {true, false}) {
      if (not dl and not ul_target.valid()) {
        continue;
      }
      uint64_t newtx_prbs = 0;
      uint64_t retx_prbs = 0;
      uint64_t unserved_bytes = 0;
      bool complete = true;

      for (const slice_ue& user : slice_ues) {
        const unsigned pending = dl ? user.pending_dl_newtx_bytes() : user.pending_ul_newtx_bytes();
        if (pending == 0) {
          continue;
        }
        const ue_cell& ue_cc = user.get_cc();
        if (dl) {
          if (not ue_cc.is_pdsch_enabled(slot_tx, slot_tx)) {
            complete = false;
            continue;
          }
          // The scheduler's DL grant helper assumes a valid CQI/MCS. Check
          // it first so a starved UE with CQI=0 yields unknown, not a crash.
          const auto& ss = ue_cc.cfg().search_space(to_search_space_id(2));
          const auto td_indices = ue_cc.active_bwp().dl.td_mapper().pdsch_td_res_indices(
              ss.get_dl_dci_format(), slot_tx.count());
          if (td_indices.empty()) {
            complete = false;
            continue;
          }
          const auto& first_pdsch = ss.get_pdsch_config(
              td_indices[0], ue_cc.channel_state_manager().get_nof_dl_layers());
          if (not ue_cc.link_adaptation_controller().calculate_dl_mcs(first_pdsch.mcs_table).has_value()) {
            complete = false;
            continue;
          }
          auto ctxt = sched_helper::get_newtx_dl_sched_context(
              user, slot_tx, slot_tx, false, units::bytes{pending});
          if (not ctxt.has_value()) {
            complete = false;
            continue;
          }
          const auto& pdsch = ss.get_pdsch_config(ctxt->pdsch_td_res_index, ctxt->recommended_ri);
          auto tbs = compute_dl_mcs_tbs(pdsch, ctxt->recommended_mcs, ctxt->expected_nof_rbs, true);
          if (not tbs.has_value()) {
            complete = false;
            continue;
          }
          newtx_prbs += ctxt->expected_nof_rbs;
          unserved_bytes += pending > tbs->tbs.value() ? pending - tbs->tbs.value() : 0;
        } else {
          if (not ue_cc.is_pusch_enabled(slot_tx, ul_target)) {
            complete = false;
            continue;
          }
          // HARQ-ACK bits on PUSCH are assumed absent (0): a grant carrying UCI
          // may need more PRBs than this data-only estimate.
          auto ctxt = sched_helper::get_newtx_ul_sched_context(
              user, slot_tx, ul_target, 0, units::bytes{pending}, ofdm_symbol_range{0, ul_symbol_stop});
          if (not ctxt.has_value()) {
            complete = false;
            continue;
          }
          auto tbs = compute_ul_tbs(
              ctxt->pusch_cfg, ue_cc.active_bwp(), ctxt->recommended_mcs, ctxt->expected_nof_rbs, true);
          if (not tbs.has_value()) {
            complete = false;
            continue;
          }
          newtx_prbs += ctxt->expected_nof_rbs;
          unserved_bytes += pending > tbs->value() ? pending - tbs->value() : 0;
        }
      }

      // Only HARQs explicitly pending retransmission are included. A process
      // awaiting ACK/CRC is not counted again as queue demand.
      if (dl) {
        for (auto h : cell_harqs.pending_dl_retxs()) {
          if (h.get_grant_params().slice_id == slice.id) {
            retx_prbs += h.get_grant_params().rbs.type1().length();
          }
        }
      } else {
        for (auto h : cell_harqs.pending_ul_retxs()) {
          if (h.get_grant_params().slice_id == slice.id) {
            retx_prbs += h.get_grant_params().rbs.type1().length();
          }
        }
      }
      metrics.handle_slice_queue_need(slice_index - 2, dl, newtx_prbs, retx_prbs, unserved_bytes, complete);
    }
  }
}

void inter_slice_scheduler::add_ue(du_ue_index_t ue_idx)
{
  ue* u = fetch_ue_to_update(ue_idx);
  if (u == nullptr) {
    return;
  }

  // Add UE and new bearers.
  add_impl(*u);
}

void inter_slice_scheduler::reconf_ue(du_ue_index_t ue_idx)
{
  // When the UE is reconfigured, it enters fallback mode and only leaves it when config_applied is called.
  // For this reason, we remove the UE from its slices.
  rem_ue(ue_idx);
}

void inter_slice_scheduler::rem_ue(du_ue_index_t ue_idx)
{
  // Remove all logical channels of UE.
  // Note: We take the conservative approach of traversing all slices, because the current UE config might not match
  // the UE repositories inside each slice instance (e.g. in case of fallback or during reconfig).
  for (auto& slice : slices) {
    if (slice.inst.get_ues().contains(ue_idx)) {
      get_policy(slice.inst.id).rem_ue(ue_idx);
    }
    slice.inst.get_ues().rem_ue(ue_idx);
  }
}

void inter_slice_scheduler::config_applied(du_ue_index_t ue_idx)
{
  ue* u = fetch_ue_to_update(ue_idx);
  if (u == nullptr) {
    logger.error("Config applied to a UE that is inactive or in fallback");
    return;
  }

  // Add UE to slices.
  add_impl(*u);
}

void inter_slice_scheduler::add_impl(ue& u)
{
  // Iterate the logical channels tracked by the logical channel system rather than the UE dedicated config, as the
  // latter also tracks deactivated logical channels.
  for (logical_channel_config_ptr lc_cfg : u.logical_channels().cfg()) {
    ran_slice_instance& sl_inst = get_slice(*lc_cfg);
    if (lc_cfg->lcid != LCID_SRB0 and not sl_inst.get_ues().contains(u.ue_index)) {
      get_policy(sl_inst.id).add_ue(u.ue_index);
    }
    sl_inst.get_ues().add_logical_channel(u, lc_cfg->lcid, lc_cfg->lc_group);
  }
}

ue* inter_slice_scheduler::fetch_ue_to_update(du_ue_index_t ue_idx) const
{
  if (not ues.contains(ue_idx)) {
    // UE should be added to the repository at this stage.
    logger.warning("ue={}: Not configuring UE to slice scheduler. Cause: No UE context found", fmt::underlying(ue_idx));
    return nullptr;
  }

  auto&                   u      = ues[ue_idx];
  const ue_configuration& ue_cfg = *u.ue_cfg_dedicated();

  // If UE does not have complete configuration, we won't be added to any slice.
  if (not ue_cfg.is_ue_cfg_complete()) {
    return nullptr;
  }

  // If UE is in fallback mode, we do not add it to the slice scheduler.
  const ue_cell* ue_cc = u.find_cell(cell_cfg.cell_index);
  if (ue_cc == nullptr) {
    logger.warning("ue={}: Not adding UE to slice scheduler. Cause: No UE context found in cell {}",
                   fmt::underlying(ue_cfg.ue_index),
                   fmt::underlying(cell_cfg.cell_index));
    return nullptr;
  }
  if (ue_cc->is_in_fallback_mode()) {
    // Do not include yet the UE in the slice scheduler.
    return nullptr;
  }

  return &u;
}

ran_slice_instance& inter_slice_scheduler::get_slice(const logical_channel_config& lc_cfg)
{
  // Return default SRB slice if LCID belongs to a SRB.
  if (lc_cfg.lcid < LCID_MIN_DRB) {
    return slices[SRB_RAN_SLICE_ID.value()].inst;
  }
  auto it = std::find_if(slices.begin(), slices.end(), [&lc_cfg](const ran_slice_sched_context& slice) {
    return slice.inst.cfg.rrc_member == lc_cfg.rrm_policy;
  });
  if (it == slices.end() or lc_cfg.rrm_policy == rrm_policy_member{}) {
    // Slice with the provided RRM policy member was not found. Return default DRB slice.
    return slices[DEFAULT_DRB_RAN_SLICE_ID.value()].inst;
  }
  return it->inst;
}

template <bool IsDownlink>
std::optional<std::conditional_t<IsDownlink, dl_ran_slice_candidate, ul_ran_slice_candidate>>
inter_slice_scheduler::get_next_candidate()
{
  using candidate_type = std::conditional_t<IsDownlink, dl_ran_slice_candidate, ul_ran_slice_candidate>;

  slice_prio_queue& prio_queue = IsDownlink ? dl_prio_queue : ul_prio_queue;
  while (not prio_queue.empty()) {
    ran_slice_sched_context& chosen_slice = slices[prio_queue.top().id.value()];
    interval<unsigned>       rb_lims      = prio_queue.top().rb_lims;
    slot_point               pxsch_slot   = prio_queue.top().slot_tx;
    prio_queue.pop();

    // Check if two consecutive candidates belong to the same slice. If so, we can combine them into a single candidate.
    if (not prio_queue.empty() and prio_queue.top().id == chosen_slice.inst.id and
        prio_queue.top().slot_tx == pxsch_slot) {
      rb_lims = {std::min(rb_lims.start(), prio_queue.top().rb_lims.start()),
                 std::max(rb_lims.stop(), prio_queue.top().rb_lims.stop())};
      prio_queue.pop();
    }

    // Compute how many RBs can be allocated for this slice scheduling opportunity.
    const unsigned cell_max_rbs = IsDownlink ? cell_cfg.params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length()
                                             : cell_cfg.params.ul_cfg_common.init_ul_bwp.generic_params.crbs.length();
    // Consider the limit imposed by dedicated resources or RBs allocated for already scheduled slices.
    const unsigned nof_used_rbs = get_nof_used_reserved_rbs<IsDownlink>(pxsch_slot);
    const unsigned rb_count =
        IsDownlink ? chosen_slice.inst.pdsch_rb_count : chosen_slice.inst.nof_pusch_rbs_allocated(pxsch_slot);
    // Subtract the dedicated RBs not yet assigned to the slice from \ref nof_used_rbs; without this, we would prevent
    // the slice candidate from using its own dedicated RBs.
    const unsigned rbs_to_discount =
        chosen_slice.inst.cfg.rbs.dedicated() > rb_count ? chosen_slice.inst.cfg.rbs.dedicated() - rb_count : 0U;
    ocudu_assert(cell_max_rbs + rbs_to_discount >= nof_used_rbs,
                 "Remaining RB count cannot result in a negative value");
    const unsigned rem_rbs = cell_max_rbs + rbs_to_discount - nof_used_rbs;
    ocudu_sanity_check(rb_lims.stop() >= rb_count,
                       "This slice has been allocated more RBs than its previous slice candidate rb_max");
    // If the slice this candidate belongs to has been already allocated some RBs, then cap max_rbs_candidate to the
    // difference rb_lims.stop() - rb_count.
    const unsigned max_rbs_candidate = std::min(rb_lims.stop() - rb_count, rem_rbs);
    if (max_rbs_candidate == 0) {
      // We can skip this candidate, as there is no more RB space for it.
      continue;
    }
    if (rb_lims.start() > rb_count) {
      // The number of RBs already allocated to this slice is outside the range of the candidate valid RB limits.
      // Note: This can happen when the scheduler could not fill up to minRB limit of the first candidate when
      // minRB > 0. At this point, we should skip the candidate {minRB, maxRB} as well.
      continue;
    }

    // Return the candidate.
    return candidate_type{chosen_slice.inst, pxsch_slot, max_rbs_candidate};
  }
  return std::nullopt;
}

std::optional<dl_ran_slice_candidate> inter_slice_scheduler::get_next_dl_candidate()
{
  return get_next_candidate<true>();
}

std::optional<ul_ran_slice_candidate> inter_slice_scheduler::get_next_ul_candidate()
{
  return get_next_candidate<false>();
}

void inter_slice_scheduler::handle_slice_reconfiguration_request(const du_cell_slice_reconfig_request& req)
{
  if (req.completion) {
    auto expected = slice_reconfiguration_completion::pending;
    if (not req.completion->status.compare_exchange_strong(expected, slice_reconfiguration_completion::applying)) {
      return; // Cancelled while queued; no late mutation after a timeout.
    }
  }
  // Prevalidate the whole batch before mutating scheduler state.
  for (const auto& rrm : req.rrm_policies) {
    if (std::none_of(slices.begin(), slices.end(), [&rrm](const auto& slice) {
          return slice.inst.cfg.rrc_member == rrm.rrc_member;
        })) {
      logger.warning("Unknown slice {}; rejecting scheduler batch", rrm.rrc_member);
      if (req.completion) req.completion->status.store(slice_reconfiguration_completion::rejected);
      if (req.stage2_trace_id) {
        stage2::emit("\"event\":\"slice_rejected\",\"trace_id\":" + std::to_string(req.stage2_trace_id));
      }
      return;
    }
  }
  std::string policies = "[";
  for (const auto& rrm : req.rrm_policies) {
    for (auto& slice : slices) {
      if (slice.inst.cfg.rrc_member == rrm.rrc_member) {
        const auto old = slice.inst.cfg.rbs;
        slice.inst.cfg.rbs = rrm.rbs;
        if (req.stage2_trace_id) {
          if (policies.size() > 1) policies += ",";
          policies += fmt::format("{{\"plmn\":\"{}\",\"sst\":{},\"sd\":{},\"min_prbs\":{},\"max_prbs\":{},"
                                  "\"ded_prbs\":{},\"old_min_prbs\":{},\"old_max_prbs\":{},\"old_ded_prbs\":{}}}",
                                  rrm.rrc_member.plmn_id.to_string(), rrm.rrc_member.s_nssai.sst.value(),
                                  rrm.rrc_member.s_nssai.sd.value(), rrm.rbs.min(), rrm.rbs.max(), rrm.rbs.dedicated(),
                                  old.min(), old.max(), old.dedicated());
        }
      }
    }
  }
  if (req.stage2_trace_id) {
    stage2::emit("\"event\":\"slice_applied\",\"trace_id\":" + std::to_string(req.stage2_trace_id) +
                 ",\"cell_index\":" + std::to_string(static_cast<unsigned>(req.cell_index)) +
                 ",\"policies\":" + policies + "]");
  }
  if (req.completion) req.completion->status.store(slice_reconfiguration_completion::applied);
}

template <bool IsDownlink>
unsigned inter_slice_scheduler::get_nof_used_reserved_rbs(slot_point pxsch_slot) const
{
  unsigned nof_used_rbs = 0;
  for (const auto& slice : slices) {
    const unsigned rb_count = IsDownlink ? slice.inst.pdsch_rb_count : slice.inst.nof_pusch_rbs_allocated(pxsch_slot);
    nof_used_rbs += std::max(slice.inst.cfg.rbs.dedicated(), rb_count);
  }
  return nof_used_rbs;
}

inter_slice_scheduler::priority_type inter_slice_scheduler::ran_slice_sched_context::get_prio(bool       is_dl,
                                                                                              slot_point pdcch_slot,
                                                                                              slot_point pxsch_slot,
                                                                                              unsigned   rb_lims) const
{
  // Note: The positive integer representing the priority of a slice consists of a concatenation of several priority
  // values:
  // 1. slice slot distance (7 most significant bits). It prioritizes slices whose PXSCH slot is closer to the PDCCH
  // slot. This is to avoid scheduling PDSCHs/PUSCHs out-of-order for the same UE.
  // 2. slice min/max RB priority (1 bit). It prioritizes slices that have not yet achieved their minRB ratio.
  // 3. slice traffic priority (8 bits). It differentiates slices based on their configured priority value.
  // 4. delay priority (8 bits), which attributes the highest priority to slices that have not been scheduled for a
  // long time.
  // 5. round-robin based on slot indication count (7 least significant bits).
  // 6. one bit set to 1 to differentiate from "skip" priority.

  static constexpr priority_type slice_dist_bitsize       = 7U;
  static constexpr priority_type slice_dist_bitmask       = (1U << slice_dist_bitsize) - 1U;
  static constexpr priority_type slice_minrb_prio_bitsize = 1U;
  static constexpr priority_type slice_prio_bitsize       = 8U;
  static constexpr priority_type slice_prio_bitmask       = (1U << slice_prio_bitsize) - 1U;
  static constexpr priority_type delay_prio_bitsize       = 8U;
  static constexpr priority_type delay_prio_bitmask       = (1U << delay_prio_bitsize) - 1U;
  static constexpr priority_type rr_bitsize               = 7U;
  static constexpr priority_type rr_bitmask               = (1U << rr_bitsize) - 1U;

  if (not inst.active()) {
    // If the slice is not in a state to be scheduled in this slot, return skip priority level.
    return skip_prio;
  }

  // 1. (most significant bits) Prioritize closer slots over slots further away into the future.
  // Note: This is relevant for UL-heavy cases, to avoid scheduling PUSCHs out-of-order for the same UE.
  const priority_type slot_dist =
      slice_dist_bitmask - std::min(static_cast<unsigned>(pxsch_slot - pdcch_slot), slice_dist_bitmask);
  priority_type prio = slot_dist;

  // 2. In case minRB > 0 and minimum RB ratio agreement is not yet reached, we give it a higher priority.
  const priority_type slice_minrb_prio = inst.cfg.rbs.min() > 0 and rb_lims <= inst.cfg.rbs.min() ? 1U : 0U;
  prio                                 = (prio << slice_minrb_prio_bitsize) + slice_minrb_prio;

  // 3. Prioritize slices with higher priority.
  prio = (prio << slice_prio_bitsize) + std::min(inst.cfg.priority, slice_prio_bitmask);

  // 4. Increase priorities of slices that have not been scheduled for a long time.
  const priority_type delay_prio =
      std::min(is_dl ? inst.nof_slots_since_last_pdsch(pxsch_slot) : inst.nof_slots_since_last_pusch(pxsch_slot),
               delay_prio_bitmask);
  prio = (prio << delay_prio_bitsize) + delay_prio;

  // 5. Round-robin across slices with the same slice and delay priorities.
  const float         rbs_per_slot = is_dl ? inst.average_pdsch_rbs_per_slot() : inst.average_pusch_rbs_per_slot();
  const priority_type rr_prio      = rr_bitmask - std::min(static_cast<unsigned>(std::round(rbs_per_slot)), rr_bitmask);
  prio                             = (prio << rr_bitsize) + rr_prio;

  // Add one bit to differentiate from skip priority.
  return (prio << 1U) + 1U;
}
