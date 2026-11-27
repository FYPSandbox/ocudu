// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Whole-scheduler tests of the project-specific queued-service-need slice measurement.
///
/// The cell mirrors the lab setup (TDD, band n78, 60 MHz, 30 kHz SCS). The estimate is compared against the grants
/// that the unmodified scheduler issues for the same queue and channel state.

#include "test_utils/indication_generators.h"
#include "test_utils/result_test_helpers.h"
#include "test_utils/scheduler_test_simulator.h"
#include "tests/test_doubles/scheduler/cell_config_builder_profiles.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "ocudu/ran/du_types.h"
#include "ocudu/scheduler/config/logical_channel_config_factory.h"
#include "ocudu/scheduler/scheduler_feedback_handler.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

rrm_policy_member slice_member(unsigned sst)
{
  return rrm_policy_member{plmn_identity::test_value(),
                           s_nssai_t{slice_service_type{static_cast<uint8_t>(sst)}, slice_differentiator::create(0).value()}};
}

cell_config_builder_params lab_like_params()
{
  cell_config_builder_params params = cell_config_builder_profiles::create(duplex_mode::TDD);
  params.scs_common                 = subcarrier_spacing::kHz30;
  params.dl_carrier.band            = nr_band::n78;
  params.dl_carrier.carrier_bw      = bs_channel_bandwidth::MHz60;
  params.dl_carrier.arfcn_f_ref     = 627340;
  return params;
}

scheduler_test_sim_config sim_config(bool auto_uci, bool auto_crc)
{
  scheduler_test_sim_config cfg;
  cfg.max_scs  = subcarrier_spacing::kHz30;
  cfg.auto_uci = auto_uci;
  cfg.auto_crc = auto_crc;
  return cfg;
}

class queue_need_bench : public scheduler_test_simulator
{
public:
  queue_need_bench(const std::vector<slice_rrm_policy_config>& slices,
                   bool                                        uci_feedback = true,
                   bool                                        crc_feedback = true,
                   const cell_config_builder_params&           params       = lab_like_params()) :
    scheduler_test_simulator(sim_config(uci_feedback, crc_feedback))
  {
    auto req               = sched_config_helper::make_default_sched_cell_configuration_request(params);
    req.rrm_policy_members = slices;
    add_cell(req);
  }

  rnti_t add_slice_ue(unsigned sst, unsigned cqi = 15)
  {
    auto req = sched_config_helper::create_default_sched_ue_creation_request(cell_cfg().params, {LCID_MIN_DRB});
    for (auto& lc : *req.cfg.lc_config_list) {
      if (lc.lcid == LCID_MIN_DRB) {
        lc.rrm_policy = slice_member(sst);
      }
    }
    req.ue_index = to_du_ue_index(ue_count);
    req.crnti    = to_rnti(0x4601 + ue_count);
    scheduler_test_simulator::add_ue(req);

    uci_indication uci;
    uci.slot_rx    = next_slot_rx();
    uci.cell_index = to_du_cell_index(0);
    uci.ucis.resize(1);
    uci.ucis[0].crnti    = req.crnti;
    uci.ucis[0].ue_index = req.ue_index;
    uci.ucis[0].pdu      = uci_indication::uci_pdu::uci_pucch_f2_or_f3_or_f4_pdu{
             .csi = csi_report_data{.first_tb_wideband_cqi = cqi_value{static_cast<uint8_t>(cqi)}, .valid = true}};
    sched->handle_uci_indication(uci);

    ++ue_count;
    return req.crnti;
  }

  void dl_queue(unsigned ue, unsigned bytes)
  {
    push_dl_buffer_state(dl_buffer_state_indication_message{to_du_ue_index(ue), LCID_MIN_DRB, bytes});
  }

  void ul_queue(unsigned ue, unsigned bytes)
  {
    const lcg_id_t        lcg = config_helpers::create_default_logical_channel_config(LCID_MIN_DRB).lc_group;
    ul_bsr_indication_message bsr{
        to_du_cell_index(0), to_du_ue_index(ue), to_rnti(0x4601 + ue), bsr_format::SHORT_BSR, {}};
    bsr.reported_lcgs.push_back(ul_bsr_lcg_report{lcg, bytes});
    push_bsr(bsr);
  }

  void run(unsigned nof_slots)
  {
    for (unsigned i = 0; i != nof_slots; ++i) {
      run_slot();
    }
  }

  /// Close the current measurement window on the next slot and return the report for slice \c index.
  scheduler_slice_metrics step_and_report(unsigned index = 0)
  {
    request_metrics_on_next_slot();
    run_slot();
    return last_metrics()->slice_metrics.at(index);
  }

  /// Run \c nof_slots with one accumulated report. \c per_slot is called before every slot.
  template <typename Func>
  std::vector<scheduler_slice_metrics> window(unsigned nof_slots, Func per_slot)
  {
    step_and_report();
    for (unsigned i = 0; i + 1 < nof_slots; ++i) {
      per_slot();
      run_slot();
    }
    per_slot();
    step_and_report();
    return last_metrics()->slice_metrics;
  }

  unsigned ue_count = 0;
};

const std::vector<slice_rrm_policy_config> one_open_slice = {{slice_member(1), {0, MAX_NOF_PRBS}}};

/// True if a channel other than the UE's own PDSCH already occupies DL PRBs in this slot.
bool dl_grid_shared(const sched_result& res)
{
  return not res.dl.bc.ssb_info.empty() or not res.dl.bc.sibs.empty() or not res.dl.rar_grants.empty() or
         not res.dl.paging_grants.empty() or res.dl.ue_grants.size() > 1;
}

/// True if a channel other than the UE's own PUSCH already occupies UL PRBs in this slot.
bool ul_grid_shared(const sched_result& res)
{
  return not res.ul.pucchs.empty() or not res.ul.prachs.empty() or not res.ul.srss.empty() or res.ul.puschs.size() > 1;
}

/// Sums of every non-empty subset of \c sizes (one per in-flight HARQ).
std::vector<unsigned> subset_sums(const std::vector<unsigned>& sizes)
{
  std::vector<unsigned> sums;
  for (unsigned mask = 1; mask < (1U << sizes.size()); ++mask) {
    unsigned sum = 0;
    for (unsigned i = 0; i != sizes.size(); ++i) {
      if ((mask >> i) & 1U) {
        sum += sizes[i];
      }
    }
    sums.push_back(sum);
  }
  return sums;
}

} // namespace

TEST(queue_need_scheduler_test, empty_queues_are_complete_zero_observations)
{
  queue_need_bench bench(one_open_slice);
  bench.add_slice_ue(1);
  bench.run(40);

  const auto m = bench.window(200, []() {}).at(0);

  EXPECT_GT(m.dl_queue_samples, 0U);
  EXPECT_GT(m.ul_queue_samples, 0U);
  EXPECT_EQ(m.dl_queue_invalid_samples, 0U);
  EXPECT_EQ(m.ul_queue_invalid_samples, 0U);
  EXPECT_EQ(m.dl_queue_newtx_prbs, 0U);
  EXPECT_EQ(m.ul_queue_newtx_prbs, 0U);
  EXPECT_EQ(m.dl_queue_retx_prbs, 0U);
  EXPECT_EQ(m.ul_queue_retx_prbs, 0U);
}

TEST(queue_need_scheduler_test, loaded_tdd_window_is_complete_in_both_directions)
{
  queue_need_bench bench(one_open_slice);
  bench.add_slice_ue(1);
  bench.run(40);

  const auto m = bench
                     .window(400,
                             [&bench]() {
                               bench.dl_queue(0, 5000000);
                               bench.ul_queue(0, 1000000);
                             })
                     .at(0);

  fmt::print("loaded: dl samples={} invalid={} newtx={} unserved={} | ul samples={} invalid={} newtx={} unserved={}\n",
             m.dl_queue_samples,
             m.dl_queue_invalid_samples,
             m.dl_queue_newtx_prbs,
             m.dl_queue_unserved_bytes,
             m.ul_queue_samples,
             m.ul_queue_invalid_samples,
             m.ul_queue_newtx_prbs,
             m.ul_queue_unserved_bytes);
  EXPECT_GT(m.dl_queue_samples, 0U);
  EXPECT_GT(m.ul_queue_samples, 0U);
  EXPECT_EQ(m.dl_queue_invalid_samples, 0U);
  EXPECT_EQ(m.ul_queue_invalid_samples, 0U);
  EXPECT_GT(m.dl_queue_newtx_prbs, 0U);
  EXPECT_GT(m.ul_queue_newtx_prbs, 0U);
  EXPECT_GT(m.dl_queue_unserved_bytes, 0U);
}

TEST(queue_need_scheduler_test, dl_estimate_matches_the_grant_of_a_lone_unrestricted_ue)
{
  queue_need_bench bench(one_open_slice);
  const rnti_t     rnti = bench.add_slice_ue(1);
  bench.run(40);

  unsigned compared = 0;
  unsigned matched  = 0;
  for (unsigned size : {200U, 1500U, 6000U, 20000U}) {
    for (unsigned i = 0; i != 60; ++i) {
      bench.dl_queue(0, size);
      const auto          m     = bench.step_and_report();
      const dl_msg_alloc* grant = find_ue_pdsch(rnti, *bench.last_sched_result());
      if (grant == nullptr or not grant->pdsch_cfg.codewords[0].new_data or m.dl_queue_samples != 1) {
        continue;
      }
      const unsigned granted = grant->pdsch_cfg.rbs.type1().length();
      ++compared;
      if (granted == m.dl_queue_newtx_prbs) {
        ++matched;
        continue;
      }
      // The estimate deliberately ignores the used resource grid. A difference is accepted only when the queue needs the
      // whole BWP and SSB, SIB1, RAR or paging already occupied some of its PRBs in the same slot.
      fmt::print("dl occupied-grid difference size={} estimate={} granted={}\n", size, m.dl_queue_newtx_prbs, granted);
      EXPECT_EQ(m.dl_queue_newtx_prbs, bench.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length());
      EXPECT_LT(granted, m.dl_queue_newtx_prbs);
      EXPECT_TRUE(dl_grid_shared(*bench.last_sched_result()));
    }
  }
  fmt::print("dl matched {}/{}\n", matched, compared);
  ASSERT_GT(compared, 20U);
  EXPECT_GT(matched, compared * 9 / 10);
}

TEST(queue_need_scheduler_test, ul_estimate_matches_the_grant_of_a_lone_unrestricted_ue)
{
  queue_need_bench bench(one_open_slice);
  const rnti_t     rnti = bench.add_slice_ue(1);
  bench.run(40);
  bench.step_and_report();

  unsigned compared = 0;
  unsigned matched  = 0;
  for (unsigned size : {200U, 1500U, 6000U, 20000U}) {
    for (unsigned episode = 0; episode != 8; ++episode) {
      bench.ul_queue(0, size);
      std::optional<uint64_t> estimate_at_dci;
      for (unsigned i = 0; i != 40; ++i) {
        const auto m = bench.step_and_report();
        if (not estimate_at_dci.has_value() and bench.find_ue_ul_pdcch(rnti) != nullptr) {
          ASSERT_EQ(m.ul_queue_samples, 1U) << "UL grant issued in a slot without a queue-need sample";
          estimate_at_dci = m.ul_queue_newtx_prbs;
        }
        const ul_sched_info* grant = find_ue_pusch(rnti, *bench.last_sched_result());
        if (grant != nullptr and grant->pusch_cfg.new_data and estimate_at_dci.has_value()) {
          // UCI multiplexed on PUSCH legitimately changes the grant size; compare data-only grants.
          if (not grant->uci.has_value()) {
            const unsigned granted = grant->pusch_cfg.rbs.type1().length();
            ++compared;
            if (granted == *estimate_at_dci) {
              ++matched;
            } else {
              // As in DL, a difference is accepted only when the queue needs the whole BWP and edge PRBs were already
              // reserved for control channels at decision time. A reserved SR PUCCH is dropped once the PUSCH is
              // granted, so it need not appear in the final slot result; the grant then starts after those PRBs.
              fmt::print("ul occupied-grid difference size={} estimate={} granted={} rb_start={} pucchs={}\n",
                         size,
                         *estimate_at_dci,
                         granted,
                         grant->pusch_cfg.rbs.type1().start(),
                         bench.last_sched_result()->ul.pucchs.size());
              EXPECT_EQ(*estimate_at_dci,
                        bench.cell_cfg().params.ul_cfg_common.init_ul_bwp.generic_params.crbs.length());
              EXPECT_LT(granted, *estimate_at_dci);
              EXPECT_TRUE(ul_grid_shared(*bench.last_sched_result()) or grant->pusch_cfg.rbs.type1().start() > 0);
            }
          }
          break;
        }
      }
      // Drain the queue so that only one grant is in flight per episode, then close the drain window so the next
      // episode starts with a fresh one-slot measurement window.
      bench.ul_queue(0, 0);
      bench.run(30);
      bench.step_and_report();
    }
  }
  fmt::print("ul matched {}/{}\n", matched, compared);
  ASSERT_GT(compared, 8U);
  EXPECT_GT(matched, compared * 3 / 4);
}

TEST(queue_need_scheduler_test, need_is_not_capped_by_the_slice_quota_or_the_cell)
{
  static constexpr unsigned quota = 20;
  queue_need_bench          bench({{slice_member(1), {0, quota}}});
  std::vector<rnti_t>       rntis;
  for (unsigned i = 0; i != 3; ++i) {
    rntis.push_back(bench.add_slice_ue(1));
  }
  bench.run(40);

  unsigned max_granted = 0;
  const auto m         = bench
                     .window(200,
                             [&]() {
                               for (unsigned i = 0; i != rntis.size(); ++i) {
                                 bench.dl_queue(i, 5000000);
                               }
                               if (bench.last_sched_result() != nullptr) {
                                 unsigned granted = 0;
                                 for (const dl_msg_alloc& g : bench.last_sched_result()->dl.ue_grants) {
                                   granted += g.pdsch_cfg.rbs.type1().length();
                                 }
                                 max_granted = std::max(max_granted, granted);
                               }
                             })
                     .at(0);

  const unsigned cell_prbs = bench.cell_cfg().params.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  ASSERT_GT(m.dl_queue_samples, 0U);
  const double need_per_opportunity = static_cast<double>(m.dl_queue_newtx_prbs) / m.dl_queue_samples;
  fmt::print("quota={} cell={} need/opportunity={} max_granted={}\n",
             quota,
             cell_prbs,
             need_per_opportunity,
             max_granted);
  EXPECT_EQ(cell_prbs, 162U);
  EXPECT_LE(max_granted, quota);
  EXPECT_GT(need_per_opportunity, cell_prbs);
  EXPECT_GT(m.dl_queue_unserved_bytes, 0U);
  EXPECT_EQ(m.dl_queue_invalid_samples, 0U);
}

TEST(queue_need_scheduler_test, unusable_cqi_marks_the_window_incomplete_instead_of_zero)
{
  queue_need_bench bench(one_open_slice, false, false);
  bench.add_slice_ue(1, 0);
  bench.run(40);

  const auto m = bench.window(100, [&bench]() { bench.dl_queue(0, 10000); }).at(0);

  ASSERT_GT(m.dl_queue_samples, 0U);
  EXPECT_GT(m.dl_queue_invalid_samples, 0U);
  EXPECT_EQ(m.dl_queue_newtx_prbs, 0U);
}

TEST(queue_need_scheduler_test, pending_dl_retx_is_counted_once_per_opportunity_in_its_own_slice)
{
  queue_need_bench bench({{slice_member(1), {0, MAX_NOF_PRBS}}, {slice_member(2), {0, MAX_NOF_PRBS}}});
  const rnti_t     rnti = bench.add_slice_ue(1);
  bench.add_slice_ue(2);
  bench.run(40);

  bool nack_next = true;
  bench.register_uci_handler([&](uci_indication& uci) {
    for (auto& pdu : uci.ucis) {
      if (pdu.crnti != rnti or not nack_next) {
        continue;
      }
      std::visit(
          [&](auto& content) {
            if (not content.harqs.empty()) {
              for (auto& h : content.harqs) {
                h = mac_harq_ack_report_status::nack;
              }
              nack_next = false;
            }
          },
          pdu.pdu);
    }
  });

  bench.dl_queue(0, 3000);
  // The queue may be served by more than one newTx PDSCH before the first HARQ-ACK arrives, and one UCI can NACK
  // several of them. Pending reTx need must then equal the sum of whole original grants, never a partial or double count.
  std::vector<unsigned> newtx_prbs;
  bool                  retx_granted = false;
  uint64_t              retx_samples = 0;
  for (unsigned i = 0; i != 100; ++i) {
    const auto first  = bench.step_and_report(0);
    const auto second = bench.last_metrics()->slice_metrics.at(1);
    EXPECT_EQ(second.dl_queue_retx_prbs, 0U);
    if (first.dl_queue_retx_prbs != 0) {
      ASSERT_FALSE(newtx_prbs.empty());
      const auto sums = subset_sums(newtx_prbs);
      EXPECT_NE(std::find(sums.begin(), sums.end(), first.dl_queue_retx_prbs), sums.end())
          << "reTx need " << first.dl_queue_retx_prbs << " is not a sum of whole original grants";
      ++retx_samples;
    }
    const dl_msg_alloc* grant = find_ue_pdsch(rnti, *bench.last_sched_result());
    if (grant == nullptr) {
      continue;
    }
    if (grant->pdsch_cfg.codewords[0].new_data) {
      if (not retx_granted and newtx_prbs.size() < 8) {
        newtx_prbs.push_back(grant->pdsch_cfg.rbs.type1().length());
      }
    } else {
      retx_granted = true;
    }
  }
  ASSERT_FALSE(newtx_prbs.empty());
  ASSERT_TRUE(retx_granted);
  fmt::print("dl retx: {} original grant(s), sampled in {} opportunities\n", newtx_prbs.size(), retx_samples);
  EXPECT_GE(retx_samples, 1U);
  EXPECT_EQ(bench.step_and_report(0).dl_queue_retx_prbs, 0U);
}

TEST(queue_need_scheduler_test, pending_ul_retx_is_counted_with_the_original_grant_size)
{
  queue_need_bench bench(one_open_slice, true, false);
  const rnti_t     rnti = bench.add_slice_ue(1);
  bench.run(40);

  bench.ul_queue(0, 3000);
  std::optional<unsigned> original_prbs;
  bool                    retx_granted = false;
  uint64_t                retx_samples = 0;
  for (unsigned i = 0; i != 150; ++i) {
    const auto m = bench.step_and_report(0);
    if (m.ul_queue_retx_prbs != 0) {
      ASSERT_TRUE(original_prbs.has_value());
      EXPECT_EQ(m.ul_queue_retx_prbs, *original_prbs);
      ++retx_samples;
    }
    for (const ul_sched_info& pusch : bench.last_sched_result()->ul.puschs) {
      const bool is_first = pusch.pusch_cfg.rnti == rnti and pusch.pusch_cfg.new_data and not original_prbs.has_value();
      if (is_first) {
        original_prbs = pusch.pusch_cfg.rbs.type1().length();
        bench.ul_queue(0, 0);
      } else if (pusch.pusch_cfg.rnti == rnti and not pusch.pusch_cfg.new_data) {
        retx_granted = true;
      }
      auto crc        = test_helper::create_crc_pdu_indication(pusch);
      crc.tb_crc_success = not is_first;
      ul_crc_indication ind;
      ind.cell_index = to_du_cell_index(0);
      ind.sl_rx      = bench.last_result_slot();
      ind.crcs.push_back(crc);
      bench.sched->handle_crc_indication(ind);
    }
  }
  ASSERT_TRUE(original_prbs.has_value());
  ASSERT_TRUE(retx_granted);
  fmt::print("ul retx: original={} PRB, sampled in {} opportunities\n", *original_prbs, retx_samples);
  EXPECT_GE(retx_samples, 1U);
}

class queue_need_tdd_pattern_test
  : public ::testing::TestWithParam<cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz>
{};

TEST_P(queue_need_tdd_pattern_test, a_permanently_loaded_queue_never_makes_the_window_incomplete)
{
  cell_config_builder_params params = lab_like_params();
  params.tdd_ul_dl_cfg_common       = cell_config_builder_profiles::create_tdd_pattern(GetParam());
  queue_need_bench bench(one_open_slice, true, true, params);
  bench.add_slice_ue(1);
  bench.run(40);

  const auto m = bench
                     .window(400,
                             [&bench]() {
                               bench.dl_queue(0, 5000000);
                               bench.ul_queue(0, 1000000);
                             })
                     .at(0);

  fmt::print("pattern={} dl samples={} invalid={} | ul samples={} invalid={}\n",
             static_cast<unsigned>(GetParam()),
             m.dl_queue_samples,
             m.dl_queue_invalid_samples,
             m.ul_queue_samples,
             m.ul_queue_invalid_samples);
  EXPECT_GT(m.dl_queue_samples, 0U);
  EXPECT_GT(m.ul_queue_samples, 0U);
  EXPECT_EQ(m.dl_queue_invalid_samples, 0U);
  EXPECT_EQ(m.ul_queue_invalid_samples, 0U);
}

INSTANTIATE_TEST_SUITE_P(
    fr1_30khz,
    queue_need_tdd_pattern_test,
    ::testing::Values(cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DDDDDDDSUU,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DDDSU,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DDDSUDDSUU,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DDDSUUDDDD,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DSUU,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DSSU,
                      cell_config_builder_profiles::tdd_pattern_profile_fr1_30khz::DDDDDDSUUU));

// FYP: offered (newly arrived) traffic through the whole scheduler.
namespace {

struct offered_run {
  scheduler_slice_metrics m;
};

/// Queue 20000 DL bytes and 5000 UL bytes once and return one report of 400 slots (200 ms at 30 kHz).
scheduler_slice_metrics offered_once(const std::vector<slice_rrm_policy_config>& slices, bool nack_all = false)
{
  queue_need_bench bench(slices);
  const rnti_t     rnti = bench.add_slice_ue(1);
  bench.run(40);
  if (nack_all) {
    bench.register_uci_handler([rnti](uci_indication& uci) {
      for (auto& pdu : uci.ucis) {
        if (pdu.crnti != rnti) {
          continue;
        }
        std::visit(
            [](auto& content) {
              for (auto& h : content.harqs) {
                h = mac_harq_ack_report_status::nack;
              }
            },
            pdu.pdu);
      }
    });
  }
  bench.step_and_report();
  bench.dl_queue(0, 20000);
  bench.ul_queue(0, 5000);
  bench.run(399);
  return bench.step_and_report();
}

} // namespace

TEST(offered_demand_scheduler_test, arrivals_and_peer_prbs_do_not_depend_on_slice_quota_or_retx)
{
  const auto open    = offered_once(one_open_slice);
  const auto capped  = offered_once({{slice_member(1), {0, 10}}});
  const auto retx    = offered_once(one_open_slice, /*nack_all=*/true);

  for (const auto* m : {&open, &capped, &retx}) {
    EXPECT_EQ(m->dl_arrived_bytes, 20000U);
    EXPECT_EQ(m->ul_arrived_bytes, 5000U);
    EXPECT_EQ(m->dl_offered_invalid_subwindows, 0U);
    EXPECT_EQ(m->ul_offered_invalid_subwindows, 0U);
    EXPECT_EQ(m->offered_slots, open.offered_slots);
    EXPECT_DOUBLE_EQ(m->dl_offered_prb_peer_slots, open.dl_offered_prb_peer_slots);
    EXPECT_DOUBLE_EQ(m->ul_offered_prb_peer_slots, open.ul_offered_prb_peer_slots);
  }
  // CQI 15, 20000 bytes in one full 100 ms sub-window (200 slots at 30 kHz): 1.4240446 PRB (Python reference).
  EXPECT_NEAR(open.dl_offered_prb_peer_slots, 1.4240446469667578 * 200, 1e-6);
  EXPECT_GT(open.ul_offered_prb_peer_slots, 0.0);
  EXPECT_EQ(open.offered_slots, 400U);
}
