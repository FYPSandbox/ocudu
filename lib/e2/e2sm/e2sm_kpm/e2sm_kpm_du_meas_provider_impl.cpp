// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "e2sm_kpm_du_meas_provider_impl.h"

using namespace asn1::e2ap;
using namespace asn1::e2sm;
using namespace ocudu;

e2sm_kpm_du_meas_provider_impl::e2sm_kpm_du_meas_provider_impl(odu::f1ap_ue_id_translator& f1ap_ue_id_translator_) :
  logger(ocudulog::fetch_basic_logger("E2SM-KPM")), f1ap_ue_id_provider(f1ap_ue_id_translator_)
{
  // Array of supported metrics.
  supported_metrics.emplace(
      "CQI", e2sm_kpm_supported_metric_t{NO_LABEL, UNKNOWN_LEVEL, false, &e2sm_kpm_du_meas_provider_impl::get_cqi});
  supported_metrics.emplace(
      "RSRP", e2sm_kpm_supported_metric_t{NO_LABEL, UNKNOWN_LEVEL, false, &e2sm_kpm_du_meas_provider_impl::get_rsrp});
  supported_metrics.emplace(
      "RSRQ", e2sm_kpm_supported_metric_t{NO_LABEL, UNKNOWN_LEVEL, false, &e2sm_kpm_du_meas_provider_impl::get_rsrq});

  supported_metrics.emplace(
      "RRU.PrbAvailDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_avail_dl});

  supported_metrics.emplace(
      "RRU.PrbAvailUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_avail_ul});

  supported_metrics.emplace(
      "RRU.PrbUsedDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL | SLICE_ID_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_used_dl});

  supported_metrics.emplace(
      "RRU.PrbUsedUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL | SLICE_ID_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_used_ul});

  // Project-specific queue snapshots. They are deliberately named separately
  // from standardized PRB-use and future new-demand metrics.
  supported_metrics.emplace(
      "FYP.SlicePendingBytesDl",
      e2sm_kpm_supported_metric_t{
          SLICE_ID_LABEL, E2_NODE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_slice_pending_bytes_dl});
  supported_metrics.emplace(
      "FYP.SlicePendingBytesUl",
      e2sm_kpm_supported_metric_t{
          SLICE_ID_LABEL, E2_NODE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_slice_pending_bytes_ul});

#define FYP_QUEUE_METRIC(name, getter)                                                                                \
  supported_metrics.emplace(name,                                                                                     \
                            e2sm_kpm_supported_metric_t{SLICE_ID_LABEL, E2_NODE_LEVEL, true,                        \
                                                         &e2sm_kpm_du_meas_provider_impl::getter})
  FYP_QUEUE_METRIC("FYP.SliceQueueNewTxPrbDl", get_slice_queue_newtx_prb_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueNewTxPrbUl", get_slice_queue_newtx_prb_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueRetxPrbDl", get_slice_queue_retx_prb_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueRetxPrbUl", get_slice_queue_retx_prb_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueUnservedBytesDl", get_slice_queue_unserved_bytes_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueUnservedBytesUl", get_slice_queue_unserved_bytes_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueSamplesDl", get_slice_queue_samples_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueSamplesUl", get_slice_queue_samples_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueInvalidSamplesDl", get_slice_queue_invalid_samples_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueInvalidSamplesUl", get_slice_queue_invalid_samples_ul);
  // Offered (newly arrived) traffic, the Bi-LSTM input (calibrations peer_dl_v1 / peer_ul_v1).
  FYP_QUEUE_METRIC("FYP.SliceArrivedBytesDl", get_slice_arrived_bytes_dl);
  FYP_QUEUE_METRIC("FYP.SliceArrivedBytesUl", get_slice_arrived_bytes_ul);
  FYP_QUEUE_METRIC("FYP.SliceOfferedPrbPeerDl", get_slice_offered_prb_peer_dl);
  FYP_QUEUE_METRIC("FYP.SliceOfferedPrbPeerUl", get_slice_offered_prb_peer_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueEndBytesDl", get_slice_queue_end_bytes_dl);
  FYP_QUEUE_METRIC("FYP.SliceQueueEndBytesUl", get_slice_queue_end_bytes_ul);
  FYP_QUEUE_METRIC("FYP.SliceQueueDecreaseBytesDl", get_slice_queue_decrease_bytes_dl);
  FYP_QUEUE_METRIC("FYP.SliceOfferedInvalidDl", get_slice_offered_invalid_dl);
  FYP_QUEUE_METRIC("FYP.SliceOfferedInvalidUl", get_slice_offered_invalid_ul);
  FYP_QUEUE_METRIC("FYP.SliceOfferedSlots", get_slice_offered_slots);
  FYP_QUEUE_METRIC("FYP.SliceBsrAgeMaxUl", get_slice_bsr_age_max_ul);
  // Exact mean PRBs used per slot of the direction (RRU.PrbUsed truncates to an integer).
  FYP_QUEUE_METRIC("FYP.SlicePrbUsedDl", get_slice_prb_used_real_dl);
  FYP_QUEUE_METRIC("FYP.SlicePrbUsedUl", get_slice_prb_used_real_ul);
#undef FYP_QUEUE_METRIC

  supported_metrics.emplace(
      "RRU.PrbTotDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_use_perc_dl});

  supported_metrics.emplace(
      "RRU.PrbTotUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prb_use_perc_ul});

  supported_metrics.emplace(
      "DRB.RlcSduDelayDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_drb_dl_rlc_sdu_latency});

  supported_metrics.emplace(
      "DRB.UEThpDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_drb_dl_mean_throughput});
  supported_metrics.emplace(
      "DRB.UEThpUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, E2_NODE_LEVEL | UE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_drb_ul_mean_throughput});
  supported_metrics.emplace(
      "DRB.RlcPacketDropRateDl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_drb_rlc_packet_drop_rate_dl});
  supported_metrics.emplace(
      "DRB.RlcSduTransmittedVolumeDL",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_drb_rlc_sdu_transmitted_volume_dl});
  supported_metrics.emplace(
      "DRB.RlcSduTransmittedVolumeUL",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_drb_rlc_sdu_transmitted_volume_ul});

  supported_metrics.emplace(
      "DRB.AirIfDelayUl",
      e2sm_kpm_supported_metric_t{NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_delay_ul});

  supported_metrics.emplace(
      "DRB.RlcDelayUl",
      e2sm_kpm_supported_metric_t{
          NO_LABEL, ALL_LEVELS, true, &e2sm_kpm_du_meas_provider_impl::get_drb_ul_rlc_sdu_latency});

  supported_metrics.emplace("RACH.PreambleDedCell",
                            e2sm_kpm_supported_metric_t{
                                NO_LABEL, E2_NODE_LEVEL, true, &e2sm_kpm_du_meas_provider_impl::get_prach_cell_count});

  // Check if the supported metrics are matching e2sm_kpm metrics definitions.
  check_e2sm_kpm_metrics_definitions(get_e2sm_kpm_28_552_metrics());
  check_e2sm_kpm_metrics_definitions(get_e2sm_kpm_oran_metrics());

  // Register for UE-context updates so slice-scoped metrics (e.g. RRU.PrbUsedDl/Ul with a
  // SliceID label) can be computed from ue_slice_map.
  e2_du_notifier_registry::get_instance().register_ue_context_notifier(this);
}

e2sm_kpm_du_meas_provider_impl::e2sm_kpm_du_meas_provider_impl(odu::f1ap_ue_id_translator& f1ap_ue_id_translator_,
                                                               int                         max_rlc_metrics_) :
  e2sm_kpm_du_meas_provider_impl(f1ap_ue_id_translator_)
{
  max_rlc_metrics = max_rlc_metrics_;
}

e2sm_kpm_du_meas_provider_impl::~e2sm_kpm_du_meas_provider_impl()
{
  e2_du_notifier_registry::get_instance().unregister_ue_context_notifier(this);
}

void e2sm_kpm_du_meas_provider_impl::on_ue_context_update(const e2_ue_context_info& ue_ctx)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  // Retain ambiguity: a per-UE counter cannot be attributed to one of several slices.
  ue_slice_map[ue_ctx.ue_index] = ue_ctx.slices;
}

void e2sm_kpm_du_meas_provider_impl::on_ue_context_release(du_ue_index_t ue_index)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  ue_slice_map.erase(ue_index);
}

bool e2sm_kpm_du_meas_provider_impl::extract_requested_slice(const asn1::e2sm::label_info_list_l& label_info_list,
                                                              s_nssai_t&                            slice_out)
{
  if (label_info_list.size() != 1 or not label_info_list[0].meas_label.slice_id_present) {
    return false;
  }
  const asn1::e2sm::s_nssai_s& asn1_slice = label_info_list[0].meas_label.slice_id;
  uint8_t                      sst_val    = static_cast<uint8_t>(asn1_slice.sst.to_number());
  slice_differentiator sd;
  if (asn1_slice.sd_present) {
    auto parsed = slice_differentiator::create(asn1_slice.sd.to_number());
    if (not parsed.has_value()) return false;
    sd = parsed.value();
  }
  slice_out = s_nssai_t{slice_service_type(sst_val), sd};
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::ue_in_slice(du_ue_index_t ue_index, const s_nssai_t& slice) const
{
  // Caller holds slice_map_mutex. Absent SD is an identity value, not a wildcard.
  auto it = ue_slice_map.find(ue_index);
  return it != ue_slice_map.end() and it->second.size() == 1 and it->second.front() == slice;
}

bool e2sm_kpm_du_meas_provider_impl::check_e2sm_kpm_metrics_definitions(span<const e2sm_kpm_metric_t> metric_defs)
{
  std::string metric_name;
  auto        name_matches = [&metric_name](const e2sm_kpm_metric_t& x) {
    return (x.name == metric_name.c_str() or x.name == metric_name);
  };

  for (auto& supported_metric : supported_metrics) {
    metric_name = supported_metric.first;
    auto it     = std::find_if(metric_defs.begin(), metric_defs.end(), name_matches);
    if (it == metric_defs.end()) {
      continue;
    }

    if (supported_metric.second.supported_labels & ~(it->optional_labels | e2sm_kpm_label_enum::NO_LABEL)) {
      logger.debug("Wrong definition of the supported metric: \"{}\", labels cannot be supported.", metric_name);
    }

    if (supported_metric.second.supported_levels & ~it->optional_levels) {
      logger.debug("Wrong definition of the supported metric: \"{}\", level cannot be supported.", metric_name);
    }

    if (is_cell_id_required(*it) and not supported_metric.second.cell_scope_supported) {
      logger.debug("Wrong definition of the supported metric: \"{}\", cell scope has to be supported.",
                   metric_name.c_str());
    }
  }
  return true;
}

void e2sm_kpm_du_meas_provider_impl::report_metrics(const scheduler_cell_metrics& cell_metrics)
{
  stage2_sched_window = cell_metrics.stage2_window;
  stage2_sched_seq = cell_metrics.stage2_seq;
  {
    std::lock_guard<std::mutex> lock(slice_map_mutex);
    last_slice_metrics = cell_metrics.slice_metrics;
  }
  last_ue_metrics.clear();
  nof_cell_prbs          = cell_metrics.nof_prbs;
  nof_dl_slots           = cell_metrics.nof_dl_slots;
  nof_ul_slots           = cell_metrics.nof_ul_slots;
  nof_ded_cell_preambles = cell_metrics.nof_prach_preambles;
  for (auto& ue_metric : cell_metrics.ue_metrics) {
    last_ue_metrics.push_back(ue_metric);
  }
  // Scheduler metrics are always delivered, RLC only when UEs are present to clear RLC history.
  clear_rlc_metrics();
}

void e2sm_kpm_du_meas_provider_impl::report_metrics(const rlc_metrics& metrics)
{
  logger.debug("Received RLC metrics: ue={} {}.", fmt::underlying(metrics.ue_index), metrics.rb_id.get_drb_id());
  ue_aggr_rlc_metrics[metrics.ue_index].push_back(metrics);
  if (ue_aggr_rlc_metrics[metrics.ue_index].size() > max_rlc_metrics) {
    ue_aggr_rlc_metrics[metrics.ue_index].pop_front();
  }
}

void e2sm_kpm_du_meas_provider_impl::clear_rlc_metrics()
{
  // Check if enough time has passed since last clear (at least 1 second)
  auto current_time = std::chrono::system_clock::now();
  auto time_since_last_clear =
      std::chrono::duration_cast<std::chrono::milliseconds>(current_time - last_rlc_metrics_clear_time);

  if (time_since_last_clear.count() <= 1000) {
    return;
  }

  last_rlc_metrics_clear_time = current_time;
  auto it                     = ue_aggr_rlc_metrics.begin();
  while (it != ue_aggr_rlc_metrics.end()) {
    du_ue_index_t       ue_index   = to_du_ue_index(it->first);
    gnb_du_ue_f1ap_id_t ue_f1ap_id = f1ap_ue_id_provider.get_gnb_du_ue_f1ap_id(ue_index);
    if (ue_f1ap_id == gnb_du_ue_f1ap_id_t::invalid) {
      it = ue_aggr_rlc_metrics.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<std::string> e2sm_kpm_du_meas_provider_impl::get_supported_metric_names(e2sm_kpm_metric_level_enum level)
{
  std::vector<std::string> metrics;
  for (auto& m : supported_metrics) {
    if ((level & E2_NODE_LEVEL) and (m.second.supported_levels & E2_NODE_LEVEL)) {
      metrics.push_back(m.first);
    } else if ((level & UE_LEVEL) and (m.second.supported_levels & UE_LEVEL)) {
      metrics.push_back(m.first);
    }
  }
  return metrics;
}

bool e2sm_kpm_du_meas_provider_impl::is_cell_supported(const asn1::e2sm::cgi_c& cell_global_id)
{
  // TODO: check if CELL is supported
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::is_ue_supported(const asn1::e2sm::ue_id_c& ueid)
{
  // TODO: check if UE is supported
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::is_test_cond_supported(const asn1::e2sm::test_cond_type_c& test_cond_type)
{
  // TODO: check if test condition is supported
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::is_metric_supported(const asn1::e2sm::meas_type_c&   meas_type,
                                                         const asn1::e2sm::meas_label_s&  label,
                                                         const e2sm_kpm_metric_level_enum level,
                                                         const bool&                      cell_scope)
{
  e2sm_kpm_label_enum requested_label = asn1_label_2_enum(label);

  for (auto& metric : supported_metrics) {
    if (strcmp(meas_type.meas_name().to_string().c_str(), metric.first.c_str()) == 0) {
      if ((metric.second.supported_labels & requested_label) == 0) {
        logger.debug("Metric {} does not support label {}.",
                     meas_type.meas_name().to_string(),
                     e2sm_kpm_label_2_str(requested_label));
        return false;
      }
      return true;
    }
  }

  // TODO: check if metric supported with required level and cell_scope
  return false;
}

bool e2sm_kpm_du_meas_provider_impl::get_ues_matching_test_conditions(
    const asn1::e2sm::matching_cond_list_l& matching_cond_list,
    std::vector<asn1::e2sm::ue_id_c>&       ues)
{
  // TODO: add test condition checking, now return all UEs
  for (const auto& ue : ue_aggr_rlc_metrics) {
    du_ue_index_t                      ue_index          = to_du_ue_index(ue.first);
    std::optional<gnb_cu_ue_f1ap_id_t> gnb_cu_ue_f1ap_id = f1ap_ue_id_provider.get_gnb_cu_ue_f1ap_id(ue_index);
    if (not gnb_cu_ue_f1ap_id.has_value()) {
      continue;
    }
    ue_id_c        ueid;
    ue_id_gnb_du_s ueid_gnb_du{};
    ueid_gnb_du.gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_to_uint(gnb_cu_ue_f1ap_id.value());
    ueid_gnb_du.ran_ue_id_present = false;
    ueid.set_gnb_du_ue_id()       = ueid_gnb_du;
    ues.push_back(ueid);
  }

  return true;
}

bool e2sm_kpm_du_meas_provider_impl::get_ues_matching_test_conditions(
    const asn1::e2sm::matching_ue_cond_per_sub_list_l& matching_ue_cond_list,
    std::vector<asn1::e2sm::ue_id_c>&                  ues)
{
  // TODO: add test condition checking, now return all UEs
  for (const auto& ue : ue_aggr_rlc_metrics) {
    du_ue_index_t                      ue_index          = to_du_ue_index(ue.first);
    std::optional<gnb_cu_ue_f1ap_id_t> gnb_cu_ue_f1ap_id = f1ap_ue_id_provider.get_gnb_cu_ue_f1ap_id(ue_index);
    if (not gnb_cu_ue_f1ap_id.has_value()) {
      continue;
    }
    ue_id_c        ueid;
    ue_id_gnb_du_s ueid_gnb_du{};
    ueid_gnb_du.gnb_cu_ue_f1ap_id = gnb_cu_ue_f1ap_id_to_uint(gnb_cu_ue_f1ap_id.value());
    ueid_gnb_du.ran_ue_id_present = false;
    ueid.set_gnb_du_ue_id()       = ueid_gnb_du;
    ues.push_back(ueid);
  }

  return true;
}

bool e2sm_kpm_du_meas_provider_impl::get_meas_data(const asn1::e2sm::meas_type_c&               meas_type,
                                                   const asn1::e2sm::label_info_list_l          label_info_list,
                                                   const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                   const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                   std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  metric_meas_getter_func_ptr metric_meas_getter_func;
  auto                        it = supported_metrics.find(meas_type.meas_name().to_string().c_str());
  if (it == supported_metrics.end()) {
    logger.debug("Metric {} not supported.", meas_type.meas_name().to_string());
    return false;
  }
  metric_meas_getter_func = it->second.func;
  ocudu_assert(metric_meas_getter_func != nullptr, "Metric getter function cannot be empty.");
  return (this->*metric_meas_getter_func)(label_info_list, ues, cell_global_id, items);
}

bool e2sm_kpm_du_meas_provider_impl::handle_no_meas_data_available(
    const std::vector<asn1::e2sm::ue_id_c>&        ues,
    std::vector<asn1::e2sm::meas_record_item_c>&   items,
    asn1::e2sm::meas_record_item_c::types::options value_type)
{
  if (ues.empty()) {
    // Fill with zero if E2 Node Measurement (Report Style 1)
    meas_record_item_c meas_record_item;
    if (value_type == asn1::e2sm::meas_record_item_c::types::options::integer) {
      meas_record_item.set_integer() = 0;
    } else if (value_type == asn1::e2sm::meas_record_item_c::types::options::real) {
      meas_record_item.set_real();
      meas_record_item.real().value = 0;
    } else if (value_type == asn1::e2sm::meas_record_item_c::types::options::not_satisfied) {
      meas_record_item.set_not_satisfied();
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    return true;
  }
  return false;
}

bool e2sm_kpm_du_meas_provider_impl::get_cqi(const asn1::e2sm::label_info_list_l          label_info_list,
                                             const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                             const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                             std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  scheduler_ue_metrics ue_metrics = last_ue_metrics[0];

  meas_record_item_c meas_record_item;
  meas_record_item.set_integer() = ue_metrics.cqi_stats.get_nof_observations() > 0
                                       ? static_cast<uint64_t>(std::round(ue_metrics.cqi_stats.get_mean()))
                                       : 0;
  items.push_back(meas_record_item);
  meas_collected = true;

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_rsrp(const asn1::e2sm::label_info_list_l          label_info_list,
                                              const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                              const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                              std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  scheduler_ue_metrics ue_metrics = last_ue_metrics[0];

  meas_record_item_c meas_record_item;
  meas_record_item.set_integer() = (int)ue_metrics.pusch_snr_db;
  items.push_back(meas_record_item);
  meas_collected = true;

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_rsrq(const asn1::e2sm::label_info_list_l          label_info_list,
                                              const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                              const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                              std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  scheduler_ue_metrics ue_metrics = last_ue_metrics[0];

  meas_record_item_c meas_record_item;
  meas_record_item.set_integer() = (int)ue_metrics.pusch_snr_db;
  items.push_back(meas_record_item);
  meas_collected = true;

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prb_avail_dl(const asn1::e2sm::label_info_list_l          label_info_list,
                                                      const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                      const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                      std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbAvailDl supports only NO_LABEL label.");
    return meas_collected;
  }
  unsigned sum_dl_prbs_used = std::accumulate(
      last_ue_metrics.begin(), last_ue_metrics.end(), 0u, [](unsigned sum, const scheduler_ue_metrics& metric) {
        return sum + metric.tot_pdsch_prbs_used;
      });
  int mean_dl_prbs_used = nof_dl_slots > 0 ? static_cast<int>(sum_dl_prbs_used / nof_dl_slots) : 0;

  for (size_t i = 0; i < std::max(ues.size(), size_t(1)); ++i) {
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = (nof_cell_prbs - mean_dl_prbs_used);
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prb_avail_ul(const asn1::e2sm::label_info_list_l          label_info_list,
                                                      const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                      const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                      std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbAvailUl supports only NO_LABEL label.");
    return meas_collected;
  }
  unsigned sum_ul_prbs_used = std::accumulate(
      last_ue_metrics.begin(), last_ue_metrics.end(), 0u, [](unsigned sum, const scheduler_ue_metrics& metric) {
        return sum + metric.tot_pusch_prbs_used;
      });
  int mean_ul_prbs_used = nof_ul_slots > 0 ? static_cast<int>(sum_ul_prbs_used / nof_ul_slots) : 0;

  for (size_t i = 0; i < std::max(ues.size(), size_t(1)); ++i) {
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = (nof_cell_prbs - mean_ul_prbs_used);
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prb_used_dl(const asn1::e2sm::label_info_list_l          label_info_list,
                                                     const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                     const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                     std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  stage2::source("RRU.PrbUsedDl", -1, stage2_sched_window, stage2_sched_seq);
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  bool meas_collected = false;
  s_nssai_t requested_slice;
  bool      slice_scoped = extract_requested_slice(label_info_list, requested_slice);
  if (not slice_scoped and
      (label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbUsedDl supports only NO_LABEL and SliceID labels.");
    return meas_collected;
  }
  if (slice_scoped) {
    // Slice-label reports come from actual grant ownership, never whole-UE totals.
    // A slice label alone cannot disambiguate two PLMNs sharing one S-NSSAI.
    const scheduler_slice_metrics* selected = nullptr;
    unsigned matches = 0;
    for (const auto& metric : last_slice_metrics) {
      if (metric.member.s_nssai == requested_slice) { selected = &metric; ++matches; }
    }
    for (size_t i = 0; i < std::max(ues.size(), size_t{1}); ++i) {
      meas_record_item_c item;
      if (matches == 1 and ues.empty() and nof_dl_slots > 0) {
        item.set_integer() = selected->dl_prbs / nof_dl_slots;
      } else {
        item.set_no_value();
      }
      items.push_back(item);
    }
    return true;
  }
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if (ues.empty()) {
    unsigned sum_dl_prbs = 0;
    for (const scheduler_ue_metrics& ue_metric : last_ue_metrics) {
      if (slice_scoped and not ue_in_slice(ue_metric.ue_index, requested_slice)) {
        continue;
      }
      sum_dl_prbs += ue_metric.tot_pdsch_prbs_used;
    }
    double             dl_prbs_used = nof_dl_slots > 0 ? sum_dl_prbs / nof_dl_slots : 0;
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = dl_prbs_used;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    meas_record_item_c  meas_record_item;
    const auto metric = std::find_if(last_ue_metrics.begin(), last_ue_metrics.end(), [ue_idx](const auto& m) {
      return static_cast<unsigned>(m.ue_index) == ue_idx;
    });
    if (metric != last_ue_metrics.end() and (not slice_scoped or ue_in_slice(metric->ue_index, requested_slice))) {
      meas_record_item.set_integer() = nof_dl_slots > 0 ? metric->tot_pdsch_prbs_used / nof_dl_slots : 0;
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prb_used_ul(const asn1::e2sm::label_info_list_l          label_info_list,
                                                     const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                     const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                     std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  stage2::source("RRU.PrbUsedUl", -1, stage2_sched_window, stage2_sched_seq);
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  bool meas_collected = false;
  s_nssai_t requested_slice;
  bool      slice_scoped = extract_requested_slice(label_info_list, requested_slice);
  if (not slice_scoped and
      (label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbUsedUl supports only NO_LABEL and SliceID labels.");
    return meas_collected;
  }

  if (slice_scoped) {
    // Slice-label reports come from actual grant ownership, never whole-UE totals.
    // A slice label alone cannot disambiguate two PLMNs sharing one S-NSSAI.
    const scheduler_slice_metrics* selected = nullptr;
    unsigned matches = 0;
    for (const auto& metric : last_slice_metrics) {
      if (metric.member.s_nssai == requested_slice) { selected = &metric; ++matches; }
    }
    for (size_t i = 0; i < std::max(ues.size(), size_t{1}); ++i) {
      meas_record_item_c item;
      if (matches == 1 and ues.empty() and nof_ul_slots > 0) {
        item.set_integer() = selected->ul_prbs / nof_ul_slots;
      } else {
        item.set_no_value();
      }
      items.push_back(item);
    }
    return true;
  }
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if (ues.empty()) {
    unsigned sum_ul_prbs = 0;
    for (const scheduler_ue_metrics& ue_metric : last_ue_metrics) {
      if (slice_scoped and not ue_in_slice(ue_metric.ue_index, requested_slice)) {
        continue;
      }
      sum_ul_prbs += ue_metric.tot_pusch_prbs_used;
    }
    double             ul_prbs_used = nof_ul_slots > 0 ? sum_ul_prbs / nof_ul_slots : 0;
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = ul_prbs_used;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    meas_record_item_c  meas_record_item;
    const auto metric = std::find_if(last_ue_metrics.begin(), last_ue_metrics.end(), [ue_idx](const auto& m) {
      return static_cast<unsigned>(m.ue_index) == ue_idx;
    });
    if (metric != last_ue_metrics.end() and (not slice_scoped or ue_in_slice(metric->ue_index, requested_slice))) {
      meas_record_item.set_integer() = nof_ul_slots > 0 ? metric->tot_pusch_prbs_used / nof_ul_slots : 0;
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_slice_pending_bytes_dl(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
    std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  s_nssai_t requested_slice;
  if (not extract_requested_slice(label_info_list, requested_slice)) {
    return false;
  }
  const scheduler_slice_metrics* selected = nullptr;
  unsigned matches = 0;
  for (const auto& metric : last_slice_metrics) {
    if (metric.member.s_nssai == requested_slice) {
      selected = &metric;
      ++matches;
    }
  }
  meas_record_item_c item;
  if (ues.empty() and matches == 1 and selected->dl_pending_samples > 0) {
    item.set_integer() = selected->dl_pending_bytes / selected->dl_pending_samples;
  } else {
    item.set_no_value();
  }
  items.push_back(item);
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::get_slice_pending_bytes_ul(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
    std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  s_nssai_t requested_slice;
  if (not extract_requested_slice(label_info_list, requested_slice)) {
    return false;
  }
  const scheduler_slice_metrics* selected = nullptr;
  unsigned matches = 0;
  for (const auto& metric : last_slice_metrics) {
    if (metric.member.s_nssai == requested_slice) {
      selected = &metric;
      ++matches;
    }
  }
  meas_record_item_c item;
  if (ues.empty() and matches == 1 and selected->ul_pending_samples > 0) {
    item.set_integer() = selected->ul_pending_bytes / selected->ul_pending_samples;
  } else {
    item.set_no_value();
  }
  items.push_back(item);
  return true;
}

bool e2sm_kpm_du_meas_provider_impl::get_slice_queue_metric(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    std::vector<asn1::e2sm::meas_record_item_c>& items,
    bool                                        downlink,
    queue_field                                 field)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  s_nssai_t requested_slice;
  if (not extract_requested_slice(label_info_list, requested_slice)) {
    return false;
  }
  const scheduler_slice_metrics* selected = nullptr;
  unsigned matches = 0;
  for (const auto& metric : last_slice_metrics) {
    if (metric.member.s_nssai == requested_slice) {
      selected = &metric;
      ++matches;
    }
  }
  meas_record_item_c item;
  if (not ues.empty() or matches != 1) {
    item.set_no_value();
  } else {
    const uint64_t samples = downlink ? selected->dl_queue_samples : selected->ul_queue_samples;
    const uint64_t invalid = downlink ? selected->dl_queue_invalid_samples : selected->ul_queue_invalid_samples;
    if (field == queue_field::samples) {
      item.set_integer() = samples;
    } else if (field == queue_field::invalid_samples) {
      item.set_integer() = invalid;
    } else if (samples == 0 or invalid != 0) {
      item.set_no_value();
    } else {
      uint64_t total = 0;
      if (field == queue_field::newtx_prbs) {
        total = downlink ? selected->dl_queue_newtx_prbs : selected->ul_queue_newtx_prbs;
      } else if (field == queue_field::retx_prbs) {
        total = downlink ? selected->dl_queue_retx_prbs : selected->ul_queue_retx_prbs;
      } else {
        total = downlink ? selected->dl_queue_unserved_bytes : selected->ul_queue_unserved_bytes;
      }
      // Preserve fractional PRB/opportunity values; integer truncation could
      // hide low-rate IoT demand as a false zero.
      item.set_real().value = static_cast<double>(total) / static_cast<double>(samples);
    }
  }
  items.push_back(item);
  return true;
}

#define FYP_QUEUE_GETTER(method, is_dl, selected_field)                                                               \
  bool e2sm_kpm_du_meas_provider_impl::method(                                                                       \
      const asn1::e2sm::label_info_list_l          label_info_list,                                                   \
      const std::vector<asn1::e2sm::ue_id_c>&      ues,                                                               \
      const std::optional<asn1::e2sm::cgi_c>       cell_global_id,                                                     \
      std::vector<asn1::e2sm::meas_record_item_c>& items)                                                             \
  {                                                                                                                   \
    return get_slice_queue_metric(label_info_list, ues, items, is_dl, queue_field::selected_field);                  \
  }
FYP_QUEUE_GETTER(get_slice_queue_newtx_prb_dl, true, newtx_prbs)
FYP_QUEUE_GETTER(get_slice_queue_newtx_prb_ul, false, newtx_prbs)
FYP_QUEUE_GETTER(get_slice_queue_retx_prb_dl, true, retx_prbs)
FYP_QUEUE_GETTER(get_slice_queue_retx_prb_ul, false, retx_prbs)
FYP_QUEUE_GETTER(get_slice_queue_unserved_bytes_dl, true, unserved_bytes)
FYP_QUEUE_GETTER(get_slice_queue_unserved_bytes_ul, false, unserved_bytes)
FYP_QUEUE_GETTER(get_slice_queue_samples_dl, true, samples)
FYP_QUEUE_GETTER(get_slice_queue_samples_ul, false, samples)
FYP_QUEUE_GETTER(get_slice_queue_invalid_samples_dl, true, invalid_samples)
FYP_QUEUE_GETTER(get_slice_queue_invalid_samples_ul, false, invalid_samples)
#undef FYP_QUEUE_GETTER

bool e2sm_kpm_du_meas_provider_impl::get_slice_offered_metric(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    std::vector<asn1::e2sm::meas_record_item_c>& items,
    bool                                        downlink,
    offered_field                               field)
{
  std::lock_guard<std::mutex> lock(slice_map_mutex);
  s_nssai_t requested_slice;
  if (not extract_requested_slice(label_info_list, requested_slice)) {
    return false;
  }
  const scheduler_slice_metrics* selected = nullptr;
  unsigned matches = 0;
  for (const auto& metric : last_slice_metrics) {
    if (metric.member.s_nssai == requested_slice) {
      selected = &metric;
      ++matches;
    }
  }
  meas_record_item_c item;
  if (not ues.empty() or matches != 1) {
    item.set_no_value();
    items.push_back(item);
    return true;
  }
  const scheduler_slice_metrics& m = *selected;
  switch (field) {
    case offered_field::arrived_bytes:
      item.set_integer() = downlink ? m.dl_arrived_bytes : m.ul_arrived_bytes;
      break;
    case offered_field::offered_prb_peer: {
      const uint64_t invalid = downlink ? m.dl_offered_invalid_subwindows : m.ul_offered_invalid_subwindows;
      if (m.offered_slots == 0 or invalid != 0) {
        // Never report a partly unknown window as a value: the rApp treats the second as missing.
        item.set_no_value();
      } else {
        const double prb_slots = downlink ? m.dl_offered_prb_peer_slots : m.ul_offered_prb_peer_slots;
        item.set_real().value  = prb_slots / static_cast<double>(m.offered_slots);
      }
      break;
    }
    case offered_field::queue_end_bytes:
      item.set_integer() = downlink ? m.dl_queue_end_bytes : m.ul_queue_end_bytes;
      break;
    case offered_field::queue_decrease_bytes:
      item.set_integer() = m.dl_queue_decrease_bytes;
      break;
    case offered_field::invalid_subwindows:
      item.set_integer() = downlink ? m.dl_offered_invalid_subwindows : m.ul_offered_invalid_subwindows;
      break;
    case offered_field::offered_slots:
      item.set_integer() = m.offered_slots;
      break;
    case offered_field::bsr_age_max_ms:
      item.set_integer() = m.ul_bsr_age_max_ms;
      break;
    case offered_field::used_prbs: {
      const unsigned slots = downlink ? nof_dl_slots : nof_ul_slots;
      if (slots == 0) {
        item.set_no_value();
      } else {
        item.set_real().value = static_cast<double>(downlink ? m.dl_prbs : m.ul_prbs) / slots;
      }
      break;
    }
  }
  items.push_back(item);
  return true;
}

#define FYP_OFFERED_GETTER(method, is_dl, selected_field)                                                             \
  bool e2sm_kpm_du_meas_provider_impl::method(                                                                       \
      const asn1::e2sm::label_info_list_l          label_info_list,                                                   \
      const std::vector<asn1::e2sm::ue_id_c>&      ues,                                                               \
      const std::optional<asn1::e2sm::cgi_c>       cell_global_id,                                                     \
      std::vector<asn1::e2sm::meas_record_item_c>& items)                                                             \
  {                                                                                                                   \
    return get_slice_offered_metric(label_info_list, ues, items, is_dl, offered_field::selected_field);              \
  }
FYP_OFFERED_GETTER(get_slice_arrived_bytes_dl, true, arrived_bytes)
FYP_OFFERED_GETTER(get_slice_arrived_bytes_ul, false, arrived_bytes)
FYP_OFFERED_GETTER(get_slice_offered_prb_peer_dl, true, offered_prb_peer)
FYP_OFFERED_GETTER(get_slice_offered_prb_peer_ul, false, offered_prb_peer)
FYP_OFFERED_GETTER(get_slice_queue_end_bytes_dl, true, queue_end_bytes)
FYP_OFFERED_GETTER(get_slice_queue_end_bytes_ul, false, queue_end_bytes)
FYP_OFFERED_GETTER(get_slice_queue_decrease_bytes_dl, true, queue_decrease_bytes)
FYP_OFFERED_GETTER(get_slice_offered_invalid_dl, true, invalid_subwindows)
FYP_OFFERED_GETTER(get_slice_offered_invalid_ul, false, invalid_subwindows)
FYP_OFFERED_GETTER(get_slice_offered_slots, true, offered_slots)
FYP_OFFERED_GETTER(get_slice_bsr_age_max_ul, false, bsr_age_max_ms)
FYP_OFFERED_GETTER(get_slice_prb_used_real_dl, true, used_prbs)
FYP_OFFERED_GETTER(get_slice_prb_used_real_ul, false, used_prbs)
#undef FYP_OFFERED_GETTER

bool e2sm_kpm_du_meas_provider_impl::get_prb_use_perc_dl(const asn1::e2sm::label_info_list_l          label_info_list,
                                                         const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                         const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                         std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbTotDl supports only NO_LABEL label.");
    return meas_collected;
  }

  if (ues.empty()) {
    unsigned sum_dl_prbs_used = std::accumulate(
        last_ue_metrics.begin(), last_ue_metrics.end(), 0u, [](unsigned sum, const scheduler_ue_metrics& metric) {
          return sum + metric.tot_pdsch_prbs_used;
        });
    double             mean_dl_prbs_used = nof_dl_slots > 0 ? sum_dl_prbs_used / nof_dl_slots : 0;
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = nof_cell_prbs > 0 ? mean_dl_prbs_used * 100 / nof_cell_prbs : 0;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    meas_record_item_c  meas_record_item;
    if (ue_idx != du_ue_index_t::INVALID_DU_UE_INDEX && ue_idx < last_ue_metrics.size()) {
      unsigned ue_mean_dl_prbs_used = nof_dl_slots > 0 ? last_ue_metrics[ue_idx].tot_pdsch_prbs_used / nof_dl_slots : 0;
      meas_record_item.set_integer() = nof_cell_prbs > 0 ? ue_mean_dl_prbs_used * 100 / nof_cell_prbs : 0;
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prb_use_perc_ul(const asn1::e2sm::label_info_list_l          label_info_list,
                                                         const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                         const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                         std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RRU.PrbTotUl supports only NO_LABEL label.");
    return meas_collected;
  }
  if (ues.empty()) {
    unsigned sum_ul_prbs_used = std::accumulate(
        last_ue_metrics.begin(), last_ue_metrics.end(), 0u, [](unsigned sum, const scheduler_ue_metrics& metric) {
          return sum + metric.tot_pusch_prbs_used;
        });
    double             mean_ul_prbs_used = nof_ul_slots > 0 ? sum_ul_prbs_used / nof_ul_slots : 0;
    meas_record_item_c meas_record_item;
    meas_record_item.set_integer() = nof_cell_prbs > 0 ? mean_ul_prbs_used * 100 / nof_cell_prbs : 0;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    meas_record_item_c  meas_record_item;
    if (ue_idx != du_ue_index_t::INVALID_DU_UE_INDEX && ue_idx < last_ue_metrics.size()) {
      unsigned ue_mean_ul_prbs_used = nof_ul_slots > 0 ? last_ue_metrics[ue_idx].tot_pusch_prbs_used / nof_ul_slots : 0;
      meas_record_item.set_integer() = nof_cell_prbs > 0 ? ue_mean_ul_prbs_used * 100 / nof_cell_prbs : 0;
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}
bool e2sm_kpm_du_meas_provider_impl::get_delay_ul(const asn1::e2sm::label_info_list_l          label_info_list,
                                                  const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                  const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                  std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::no_value);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.AirIfDelayUl supports only NO_LABEL label.");
    return meas_collected;
  }

  if (ues.empty()) {
    float sum_ul_delay_ms = std::accumulate(
        last_ue_metrics.begin(), last_ue_metrics.end(), 0.0f, [](float sum, const scheduler_ue_metrics& metric) {
          return sum + metric.avg_crc_delay_ms.value_or(0.0f);
        });
    float              mean_ul_delay_ms = sum_ul_delay_ms / static_cast<float>(last_ue_metrics.size());
    meas_record_item_c meas_record_item;
    if (mean_ul_delay_ms) {
      meas_record_item.set_real().value = mean_ul_delay_ms * 10; // unit is 0.1ms
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    meas_record_item_c  meas_record_item;
    if (ue_idx != du_ue_index_t::INVALID_DU_UE_INDEX && ue_idx < last_ue_metrics.size()) {
      if (last_ue_metrics[ue_idx].avg_crc_delay_ms.has_value()) {
        meas_record_item.set_real().value = (last_ue_metrics[ue_idx].avg_crc_delay_ms.value() * 10); // unit is 0.1ms
      } else {
        meas_record_item.set_no_value();
      }
    } else {
      meas_record_item.set_no_value();
    }
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_prach_cell_count(const asn1::e2sm::label_info_list_l          label_info_list,
                                                          const std::vector<asn1::e2sm::ue_id_c>&      ues,
                                                          const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
                                                          std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (last_ue_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: RACH.PreambleDedCell supports only NO_LABEL label.");
    return meas_collected;
  }
  meas_record_item_c meas_record_item;
  meas_record_item.set_integer() = nof_ded_cell_preambles;
  items.push_back(meas_record_item);
  meas_collected = true;

  return meas_collected;
}

float e2sm_kpm_du_meas_provider_impl::bytes_to_kbits(float value)
{
  constexpr unsigned nof_bits_per_byte = 8;
  return (nof_bits_per_byte * value / 1e3);
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_dl_mean_throughput(const asn1::e2sm::label_info_list_l     label_info_list,
                                                                const std::vector<asn1::e2sm::ue_id_c>& ues,
                                                                const std::optional<asn1::e2sm::cgi_c>  cell_global_id,
                                                                std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::real);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.UEThpDl supports only NO_LABEL label.");
    return meas_collected;
  }
  double                       seconds = 1;
  std::map<uint16_t, unsigned> ue_throughput;
  for (auto& ue : ue_aggr_rlc_metrics) {
    size_t num_pdu_bytes_with_segmentation;
    if (std::holds_alternative<rlc_um_tx_metrics_lower>(ue.second.front().tx.tx_low.mode_specific)) {
      // get average from queue
      num_pdu_bytes_with_segmentation =
          std::accumulate(ue.second.begin(), ue.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            auto& um = std::get<rlc_um_tx_metrics_lower>(metric.tx.tx_low.mode_specific);
            return sum + um.num_pdu_bytes_with_segmentation;
          });
      num_pdu_bytes_with_segmentation /= ue.second.size();
    } else if (std::holds_alternative<rlc_am_tx_metrics_lower>(ue.second.front().tx.tx_low.mode_specific)) {
      num_pdu_bytes_with_segmentation =
          std::accumulate(ue.second.begin(), ue.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            auto& am = std::get<rlc_am_tx_metrics_lower>(metric.tx.tx_low.mode_specific);
            return sum + am.num_pdu_bytes_with_segmentation;
          });
      num_pdu_bytes_with_segmentation /= ue.second.size();
    } else {
      num_pdu_bytes_with_segmentation = 0;
    }
    auto num_pdu_bytes_no_segmentation =
        std::accumulate(ue.second.begin(), ue.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
          return sum + metric.tx.tx_low.num_pdu_bytes_no_segmentation;
        });
    num_pdu_bytes_no_segmentation /= ue.second.size();
    seconds = (float)std::chrono::duration_cast<std::chrono::milliseconds>(ue.second.back().metrics_period).count() /
              (float)1000;
    ue_throughput[ue.first] = bytes_to_kbits(num_pdu_bytes_no_segmentation + num_pdu_bytes_with_segmentation) / seconds;
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    int                total_throughput = 0;
    for (auto& ue : ue_throughput) {
      total_throughput += ue.second;
      for (const auto& m : ue_aggr_rlc_metrics[ue.first]) stage2::source("DRB.UEThpDl", -1, m.tx.tx_low.stage2_window, m.tx.tx_low.counter);
    }
    meas_record_item.set_real().value = total_throughput;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    meas_record_item_c  meas_record_item;
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    if (ue_throughput.count(ue_idx) == 0) {
      meas_record_item.set_no_value();
      items.push_back(meas_record_item);
      meas_collected = true;
      continue;
    }
    for (const auto& m : ue_aggr_rlc_metrics[ue_idx])
      stage2::source("DRB.UEThpDl", ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id, m.tx.tx_low.stage2_window, m.tx.tx_low.counter);
    meas_record_item.set_real().value = ue_throughput[ue_idx];
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_ul_mean_throughput(const asn1::e2sm::label_info_list_l     label_info_list,
                                                                const std::vector<asn1::e2sm::ue_id_c>& ues,
                                                                const std::optional<asn1::e2sm::cgi_c>  cell_global_id,
                                                                std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::real);
  }
  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.UEThpUl supports only NO_LABEL label.");
    return meas_collected;
  }

  double                       seconds = 1;
  std::map<uint16_t, unsigned> ue_throughput;
  for (auto& ue : ue_aggr_rlc_metrics) {
    auto num_pdu_bytes =
        std::accumulate(ue.second.begin(), ue.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
          return sum + metric.rx.num_pdu_bytes;
        });
    num_pdu_bytes /= ue.second.size();
    seconds = (float)std::chrono::duration_cast<std::chrono::milliseconds>(ue.second.back().metrics_period).count() /
              (float)1000;
    ue_throughput[ue.first] = bytes_to_kbits(num_pdu_bytes) / seconds; // unit is kbps
  }
  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    int                total_throughput = 0;
    for (auto& ue : ue_throughput) {
      total_throughput += ue.second;
      for (const auto& m : ue_aggr_rlc_metrics[ue.first]) stage2::source("DRB.UEThpUl", -1, m.rx.stage2_window, m.rx.counter);
    }
    meas_record_item.set_real().value = total_throughput;
    items.push_back(meas_record_item);
    meas_collected = true;
  }

  for (auto& ue : ues) {
    meas_record_item_c  meas_record_item;
    gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
    uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
    if (ue_throughput.count(ue_idx) == 0) {
      meas_record_item.set_no_value();
      items.push_back(meas_record_item);
      meas_collected = true;
      continue;
    }
    for (const auto& m : ue_aggr_rlc_metrics[ue_idx])
      stage2::source("DRB.UEThpUl", ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id, m.rx.stage2_window, m.rx.counter);
    meas_record_item.set_real().value = ue_throughput[ue_idx];
    items.push_back(meas_record_item);
    meas_collected = true;
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_rlc_packet_drop_rate_dl(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
    std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::no_value);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.RlcPacketDropRateDl supports only NO_LABEL label.");
    return meas_collected;
  }

  if (cell_global_id.has_value()) {
    logger.debug("Metric: DRB.RlcPacketDropRateDl currently does not support cell_global_id filter.");
  }

  if (ues.empty()) {
    // E2 level measurements.
    meas_record_item_c meas_record_item;
    float              drop_rate          = 0;
    uint32_t           total_dropped_sdus = 0;
    uint32_t           total_tx_num_sdus  = 0;
    for (auto& rlc_metric : ue_aggr_rlc_metrics) {
      total_dropped_sdus += std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.tx.tx_high.num_dropped_sdus + metric.tx.tx_high.num_discarded_sdus;
          });
      total_tx_num_sdus += std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.tx.tx_high.num_sdus;
          });
    }
    if (total_tx_num_sdus) {
      drop_rate = 1.0 * total_dropped_sdus / total_tx_num_sdus;
    }
    uint32_t drop_rate_int         = drop_rate * 100;
    meas_record_item.set_integer() = drop_rate_int;
    items.push_back(meas_record_item);
    meas_collected = true;
  } else {
    // UE level measurements.
    for (auto& ue : ues) {
      meas_record_item_c  meas_record_item;
      gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
      uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
      if (ue_aggr_rlc_metrics.count(ue_idx) == 0) {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
        continue;
      }
      float    drop_rate = 0;
      uint32_t total_dropped_sdus =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) {
                            return sum + metric.tx.tx_high.num_dropped_sdus + metric.tx.tx_high.num_discarded_sdus;
                          });
      uint32_t total_tx_num_sdus =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.tx.tx_high.num_sdus; });
      if (total_tx_num_sdus) {
        drop_rate = 1.0 * total_dropped_sdus / total_tx_num_sdus;
      }
      uint32_t drop_rate_int         = drop_rate * 100;
      meas_record_item.set_integer() = drop_rate_int;
      items.push_back(meas_record_item);
      meas_collected = true;
    }
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_rlc_sdu_transmitted_volume_dl(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
    std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.RlcSduTransmittedVolumeDL supports only NO_LABEL label.");
    return false;
  }

  if (cell_global_id.has_value()) {
    logger.debug("Metric: DRB.RlcSduTransmittedVolumeDL currently does not support cell_global_id filter.");
  }

  if (ues.empty()) {
    // E2 level measurements.
    meas_record_item_c meas_record_item;
    size_t             total_tx_num_sdu_bytes = 0;
    for (auto& rlc_metric : ue_aggr_rlc_metrics) {
      total_tx_num_sdu_bytes += std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.tx.tx_high.num_sdu_bytes;
          });
    }
    meas_record_item.set_integer() = total_tx_num_sdu_bytes * 8 / 1000; // unit is kbit
    items.push_back(meas_record_item);
    meas_collected = true;
  } else {
    // UE level measurements.
    for (auto& ue : ues) {
      meas_record_item_c  meas_record_item;
      gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
      uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
      if (ue_aggr_rlc_metrics.count(ue_idx) == 0) {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
        continue;
      }
      int num_sdu_bytes =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.tx.tx_high.num_sdu_bytes; });
      meas_record_item.set_integer() = num_sdu_bytes * 8 / 1000; // unit is kbit
      items.push_back(meas_record_item);
      meas_collected = true;
    }
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_rlc_sdu_transmitted_volume_ul(
    const asn1::e2sm::label_info_list_l          label_info_list,
    const std::vector<asn1::e2sm::ue_id_c>&      ues,
    const std::optional<asn1::e2sm::cgi_c>       cell_global_id,
    std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::integer);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.RlcSduTransmittedVolumeUL supports only NO_LABEL label.");
    return meas_collected;
  }

  if (cell_global_id.has_value()) {
    logger.debug("Metric: DRB.RlcSduTransmittedVolumeUL currently does not support cell_global_id filter.");
  }

  if (ues.empty()) {
    // E2 level measurements.
    meas_record_item_c meas_record_item;
    size_t             total_rx_num_sdu_bytes = 0;
    for (auto& rlc_metric : ue_aggr_rlc_metrics) {
      total_rx_num_sdu_bytes += std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.rx.num_sdu_bytes;
          });
    }
    meas_record_item.set_integer() = total_rx_num_sdu_bytes * 8 / 1000; // unit is kbit
    items.push_back(meas_record_item);
    meas_collected = true;
  } else {
    // UE level measurements.
    for (auto& ue : ues) {
      meas_record_item_c  meas_record_item;
      gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
      uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
      if (ue_aggr_rlc_metrics.count(ue_idx) == 0) {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
        continue;
      }
      int num_sdu_bytes =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.rx.num_sdu_bytes; });
      meas_record_item.set_integer() = num_sdu_bytes * 8 / 1000; // unit is kbit
      items.push_back(meas_record_item);
      meas_collected = true;
    }
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_dl_rlc_sdu_latency(const asn1::e2sm::label_info_list_l     label_info_list,
                                                                const std::vector<asn1::e2sm::ue_id_c>& ues,
                                                                const std::optional<asn1::e2sm::cgi_c>  cell_global_id,
                                                                std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::no_value);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.RlcSduDelayDl supports only NO_LABEL label.");
    return meas_collected;
  }

  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    float              av_ue_sdu_latency_us = 0;
    for (auto& rlc_metric : ue_aggr_rlc_metrics) {
      int tot_num_of_pulled_sdus = std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.tx.tx_low.num_of_pulled_sdus;
          });
      int tot_sum_sdu_latency_us = std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.tx.tx_low.sum_sdu_latency_us;
          });
      if (tot_num_of_pulled_sdus && tot_sum_sdu_latency_us) {
        av_ue_sdu_latency_us += (float)tot_sum_sdu_latency_us / (float)tot_num_of_pulled_sdus;
      }
    }
    if (av_ue_sdu_latency_us) {
      float av_ue_sdu_latency_ms = (av_ue_sdu_latency_us / ue_aggr_rlc_metrics.size()) / 100; // Unit is 0.1 ms.
      av_ue_sdu_latency_ms       = std::round(av_ue_sdu_latency_ms * 10.0f) / 10.0f;
      meas_record_item.set_real();
      meas_record_item.real().value = av_ue_sdu_latency_ms;
      items.push_back(meas_record_item);
      meas_collected = true;
    } else {
      meas_record_item.set_no_value();
      items.push_back(meas_record_item);
      meas_collected = true;
      return meas_collected;
    }
  } else {
    for (auto& ue : ues) {
      meas_record_item_c  meas_record_item;
      gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
      uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
      if (ue_aggr_rlc_metrics.count(ue_idx) == 0) {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
        continue;
      }
      int tot_sdu_latency_us = std::accumulate(
          ue_aggr_rlc_metrics[ue_idx].begin(),
          ue_aggr_rlc_metrics[ue_idx].end(),
          0,
          [](size_t sum, const rlc_metrics& metric) { return sum + metric.tx.tx_low.sum_sdu_latency_us; });
      int tot_num_sdus =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.tx.tx_high.num_sdus; });
      if (tot_sdu_latency_us && tot_num_sdus) {
        float av_ue_sdu_latency_ms = (static_cast<float>(tot_sdu_latency_us) / tot_num_sdus) / 100; // Unit is 0.1 ms.
        av_ue_sdu_latency_ms       = std::round(av_ue_sdu_latency_ms * 10.0f) / 10.0f;
        meas_record_item.set_real();
        meas_record_item.real().value = av_ue_sdu_latency_ms;
        items.push_back(meas_record_item);
        meas_collected = true;
      } else {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
      }
    }
  }
  return meas_collected;
}

bool e2sm_kpm_du_meas_provider_impl::get_drb_ul_rlc_sdu_latency(const asn1::e2sm::label_info_list_l     label_info_list,
                                                                const std::vector<asn1::e2sm::ue_id_c>& ues,
                                                                const std::optional<asn1::e2sm::cgi_c>  cell_global_id,
                                                                std::vector<asn1::e2sm::meas_record_item_c>& items)
{
  bool meas_collected = false;
  if (ue_aggr_rlc_metrics.empty()) {
    return handle_no_meas_data_available(ues, items, asn1::e2sm::meas_record_item_c::types::options::no_value);
  }

  if ((label_info_list.size() > 1 or
       (label_info_list.size() == 1 and not label_info_list[0].meas_label.no_label_present))) {
    logger.debug("Metric: DRB.RlcDelayUl supports only NO_LABEL label.");
    return meas_collected;
  }

  if (ues.empty()) {
    meas_record_item_c meas_record_item;
    float              av_ue_sdu_latency_us = 0;
    for (auto& rlc_metric : ue_aggr_rlc_metrics) {
      int tot_num_sdus = std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.rx.num_sdus;
          });
      int tot_sdu_latency_us = std::accumulate(
          rlc_metric.second.begin(), rlc_metric.second.end(), 0, [](size_t sum, const rlc_metrics& metric) {
            return sum + metric.rx.sdu_latency_us;
          });
      if (tot_num_sdus && tot_sdu_latency_us) {
        av_ue_sdu_latency_us += (float)tot_sdu_latency_us / (float)tot_num_sdus;
      }
    }
    if (av_ue_sdu_latency_us) {
      meas_record_item.set_real();
      meas_record_item.real().value = (av_ue_sdu_latency_us / ue_aggr_rlc_metrics.size()) / 100; // Unit is 0.1ms.
      items.push_back(meas_record_item);
      meas_collected = true;
    } else {
      meas_record_item.set_no_value();
      items.push_back(meas_record_item);
      meas_collected = true;
      return meas_collected;
    }
  } else {
    for (auto& ue : ues) {
      meas_record_item_c  meas_record_item;
      gnb_cu_ue_f1ap_id_t gnb_cu_ue_f1ap_id = int_to_gnb_cu_ue_f1ap_id(ue.gnb_du_ue_id().gnb_cu_ue_f1ap_id);
      uint32_t            ue_idx            = f1ap_ue_id_provider.get_ue_index(gnb_cu_ue_f1ap_id);
      if (ue_aggr_rlc_metrics.count(ue_idx) == 0) {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
        continue;
      }
      int tot_sdu_latency_us =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.rx.sdu_latency_us; });
      int tot_num_sdus =
          std::accumulate(ue_aggr_rlc_metrics[ue_idx].begin(),
                          ue_aggr_rlc_metrics[ue_idx].end(),
                          0,
                          [](size_t sum, const rlc_metrics& metric) { return sum + metric.rx.num_sdus; });
      if (tot_sdu_latency_us && tot_num_sdus) {
        meas_record_item.set_real();
        meas_record_item.real().value = (static_cast<float>(tot_sdu_latency_us) / tot_num_sdus) / 100; // Unit is 0.1ms.
        items.push_back(meas_record_item);
        meas_collected = true;
      } else {
        meas_record_item.set_no_value();
        items.push_back(meas_record_item);
        meas_collected = true;
      }
    }
  }
  return meas_collected;
}
