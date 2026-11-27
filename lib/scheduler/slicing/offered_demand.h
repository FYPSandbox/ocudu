// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace ocudu {
namespace fyp_offered {

/// \file
/// FYP project: newly ARRIVED (offered) traffic per UE and RAN slice, converted to the requested-PRB scale of the
/// peer simulator used to pre-train the rApp Bi-LSTM (fyp-data-genaration, calibrations "peer_dl_v1"/"peer_ul_v1").
/// Reference implementation: fyp-5g-network-slicing/rApp/horizon_pretraining/peer_demand_reference.py.
///
/// Offered bytes are not grants and not queue levels: DL counts increases of the RLC buffer occupancy reported to the
/// scheduler (data entering the queue); UL counts BSR change plus MAC SDU bytes received since the previous BSR.
/// The URLLC latency headroom is not applied here (the rApp multiplies URLLC by a fixed 1.35).

/// Length of one conversion sub-window, as in the peer simulator's 100 ms tick.
constexpr unsigned sub_window_msec = 100;

/// Peer UL constants fixed at the midpoints of the training ranges (agreed 27 November 2026).
constexpr double ul_grant_overhead = 1.175;
constexpr double ul_min_prb_active = 0.25;

/// 3GPP TS 38.214 Table 5.2.2.1-2 (CQI table 1), bits per RE, CQI 1..15.
constexpr std::array<double, 15> cqi_table1_se = {0.1523, 0.2344, 0.3770, 0.6016, 0.8770, 1.1758, 1.4766, 1.9141,
                                                  2.4063, 2.7305, 3.3223, 3.9023, 4.5234, 5.1152, 5.5547};

/// 3GPP TS 38.214 Table 5.2.2.1-3 (CQI table 2, 256QAM), CQI 1..15.
constexpr std::array<double, 15> cqi_table2_se = {0.1523, 0.3770, 0.8770, 1.4766, 1.9141, 2.4063, 2.7305, 3.3223,
                                                  3.9023, 4.5234, 5.1152, 5.5547, 6.2266, 6.9141, 7.4063};

/// 3GPP TS 38.214 Table 5.2.2.1-4 (CQI table 3, low SE), CQI 1..15.
constexpr std::array<double, 15> cqi_table3_se = {0.0586, 0.0977, 0.1523, 0.2344, 0.3770, 0.6016, 0.8770, 1.1758,
                                                  1.4766, 1.9141, 2.4063, 2.7305, 3.3223, 3.9023, 4.5234};

/// Highest spectral efficiency the peer simulator can produce (CQI 15 of table 1). Higher link SE (256QAM) is clamped
/// to it so the model input stays in its training domain; clamping can only raise the PRB value (conservative).
constexpr double peer_max_se = 5.5547;

/// Spectral efficiency of a CQI table 1 index; empty for CQI 0 (out of range) or invalid values.
inline std::optional<double> se_from_cqi_table1(unsigned cqi)
{
  if (cqi < 1 or cqi > cqi_table1_se.size()) {
    return std::nullopt;
  }
  return cqi_table1_se[cqi - 1];
}

/// Spectral efficiency of a CQI index of CQI table 1, 2 or 3, clamped to \c peer_max_se. Empty for CQI 0 or an
/// unknown table.
inline std::optional<double> peer_se_from_cqi(unsigned cqi, unsigned table_number)
{
  if (cqi < 1 or cqi > 15) {
    return std::nullopt;
  }
  const std::array<double, 15>* table = nullptr;
  switch (table_number) {
    case 1:
      table = &cqi_table1_se;
      break;
    case 2:
      table = &cqi_table2_se;
      break;
    case 3:
      table = &cqi_table3_se;
      break;
    default:
      return std::nullopt;
  }
  return std::min((*table)[cqi - 1], peer_max_se);
}

/// Link spectral efficiency (e.g. of the PUSCH MCS) clamped to \c peer_max_se; empty if not positive.
inline std::optional<double> peer_se(double spectral_efficiency)
{
  if (not(spectral_efficiency > 0)) {
    return std::nullopt;
  }
  return std::min(spectral_efficiency, peer_max_se);
}

/// Bits per second one PRB carries when held in every slot of the direction (peer prb_capacity_bps).
inline double prb_capacity_bps(double spectral_efficiency, bool downlink)
{
  constexpr double re_per_prb_slot = 168;
  constexpr double slots_per_s     = 2000;
  if (downlink) {
    return re_per_prb_slot * (1 - 0.14) * slots_per_s * spectral_efficiency * 0.7 * 1.0;
  }
  return re_per_prb_slot * (1 - 0.18) * slots_per_s * spectral_efficiency * 0.3 * 1.0;
}

/// Per-UE demand cap of the peer simulator, chosen by standard SST (1 eMBB, 2 URLLC, 3 MIoT). Same for DL and UL.
inline std::optional<double> ue_max_prb_for_sst(unsigned sst)
{
  switch (sst) {
    case 1:
      return 60.0;
    case 2:
      return 30.0;
    case 3:
      return 25.0;
    default:
      return std::nullopt;
  }
}

/// PRB-seconds that \c bytes would need at the given spectral efficiency (signed, for the UL carry).
inline double prb_seconds(double bytes, double spectral_efficiency, bool downlink)
{
  return bytes * 8.0 / prb_capacity_bps(spectral_efficiency, downlink);
}

/// Per-UE DL demand PRBs for one sub-window from its accumulated PRB-seconds.
inline double dl_ue_prb(double prb_s, double window_s, double ue_cap)
{
  return std::min(std::max(prb_s, 0.0) / window_s, ue_cap);
}

/// Per-UE UL demand PRBs for one sub-window; zero when no data arrived (net bytes <= 0).
inline double ul_ue_prb(double prb_s, int64_t net_bytes, double window_s, double ue_cap)
{
  if (net_bytes <= 0) {
    return 0.0;
  }
  const double prb = std::max(prb_s, 0.0) / window_s * ul_grant_overhead;
  return std::min(std::max(prb, ul_min_prb_active), ue_cap);
}

/// DL arrivals accumulated by the logical channel system between two scheduler reads.
struct dl_arrivals {
  /// Sum of increases of the slice's RLC buffer occupancy at buffer state reports (bytes entering the queue).
  uint64_t arrived_bytes = 0;
  /// Sum of decreases at buffer state reports (discards and scheduler estimate corrections).
  uint64_t decreased_bytes = 0;
};

/// UL arrivals accumulated by the logical channel system between two scheduler reads.
struct ul_arrivals {
  /// BSR change plus MAC SDU bytes received since the previous BSR of the same LCG (signed: BSR quantisation).
  int64_t net_bytes = 0;
  /// True if a BSR could not be applied (e.g. saturated short BSR), so arrivals are unknown.
  bool unknown = false;
};

} // namespace fyp_offered
} // namespace ocudu
