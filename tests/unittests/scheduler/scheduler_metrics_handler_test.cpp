// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/scheduler/config/cell_configuration.h"
#include "lib/scheduler/logging/cell_metrics_handler.h"
#include "tests/test_doubles/scheduler/scheduler_config_helper.h"
#include "tests/test_doubles/utils/test_rng.h"
#include "tests/unittests/scheduler/test_utils/config_generators.h"
#include "ocudu/scheduler/config/scheduler_expert_config_factory.h"
#include "ocudu/scheduler/result/sched_result.h"
#include <gtest/gtest.h>

using namespace ocudu;

class test_scheduler_cell_metrics_notifier : public scheduler_cell_metrics_notifier
{
public:
  unsigned                    period_slots = 1000;
  mutable slot_point_extended next_sl_report;
  scheduler_cell_metrics      last_report;

  scheduler_cell_metrics& get_next() override { return last_report; }

  void commit(scheduler_cell_metrics& ptr) override {}

  bool is_sched_report_required(slot_point_extended sl_tx) const override
  {
    if (not next_sl_report.valid()) {
      next_sl_report = sl_tx + period_slots - (sl_tx.count() % period_slots) - 1;
    }
    if (sl_tx >= next_sl_report) {
      next_sl_report += period_slots;
      return true;
    }
    return false;
  }
};

class test_scheduler_ue_metrics_notifier : public scheduler_metrics_notifier
{
public:
  scheduler_cell_metrics last_report;

  void report_metrics(const scheduler_cell_metrics& metrics) override { last_report = metrics; }
};

class scheduler_metrics_handler_tester : public ::testing::Test
{
protected:
  scheduler_metrics_handler_tester(
      std::chrono::milliseconds period = std::chrono::milliseconds{test_rng::uniform_int<unsigned>(2, 100)}) :
    report_period(period),
    cfg_mng{config_helpers::make_default_scheduler_expert_config()},
    cell_cfg(*cfg_mng.add_cell(sched_config_helper::make_default_sched_cell_configuration_request())),
    metrics(cell_cfg, sched_cell_configuration_request_message::metrics_config{&metrics_notif})
  {
    metrics_notif.period_slots = report_period.count() * get_nof_slots_per_subframe(cell_cfg.scs_common());
    metrics.handle_ue_creation(test_ue_index, to_rnti(0x4601), pci_t{0});
  }

  void run_slot(const sched_result& sched_res, std::chrono::microseconds latency = std::chrono::microseconds{0})
  {
    metrics.push_result(next_sl_tx, sched_res, latency);
    ++next_sl_tx;
    slot_count++;
  }

  void get_next_metric()
  {
    metrics_notif.last_report = {};
    sched_result sched_res;
    sched_res.dl.nof_dl_symbols = 14;
    sched_res.ul.nof_ul_symbols = 14;
    while (metrics_notif.last_report.ue_metrics.empty()) {
      run_slot(sched_res);
    }
  }

  std::chrono::milliseconds               report_period;
  test_scheduler_cell_metrics_notifier    metrics_notif;
  test_helpers::test_sched_config_manager cfg_mng;
  const cell_configuration&               cell_cfg;
  cell_metrics_handler                    metrics;
  du_ue_index_t test_ue_index = to_du_ue_index(test_rng::uniform_int<unsigned>(0, MAX_NOF_DU_UES - 1));

  slot_point_extended next_sl_tx{
      subcarrier_spacing::kHz15,
      test_rng::uniform_int<unsigned>(0, NOF_HYPER_SFNS * NOF_SFNS * NOF_SUBFRAMES_PER_FRAME - 1)};
  unsigned slot_count = 0;
};

TEST_F(scheduler_metrics_handler_tester, metrics_sent_with_defined_periodicity)
{
  unsigned nof_reports = test_rng::uniform_int<unsigned>(1, 10);
  ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  ASSERT_LE(metrics_notif.last_report.nof_slots, report_period.count());
  unsigned last_slot_count = slot_count;

  for (unsigned i = 0; i != nof_reports; ++i) {
    get_next_metric();
    ASSERT_EQ(metrics_notif.last_report.ue_metrics.size(), 1);
    ASSERT_EQ(metrics_notif.last_report.nof_slots, report_period.count());
    ASSERT_EQ(slot_count - last_slot_count, report_period.count() * (i + 1));

    ASSERT_EQ(metrics_notif.last_report.ue_metrics[0].rnti, to_rnti(0x4601));
  }
}

TEST_F(scheduler_metrics_handler_tester, when_ue_metrics_disabled_then_only_cell_metrics_are_reported)
{
  test_scheduler_cell_metrics_notifier cell_only_notif;
  cell_only_notif.period_slots = report_period.count() * get_nof_slots_per_subframe(cell_cfg.scs_common());
  cell_metrics_handler cell_only_metrics(
      cell_cfg, sched_cell_configuration_request_message::metrics_config{&cell_only_notif, 64, false});
  cell_only_metrics.handle_ue_creation(test_ue_index, to_rnti(0x4601), pci_t{0});

  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;

  // Drive enough slots to span at least two report periods.
  slot_point_extended sl = next_sl_tx;
  for (unsigned i = 0, e = 2 * cell_only_notif.period_slots; i != e; ++i) {
    cell_only_metrics.push_result(sl, sched_res, std::chrono::microseconds{0});
    ++sl;
  }

  ASSERT_GT(cell_only_notif.last_report.nof_slots, 0) << "Cell metrics must still be reported";
  ASSERT_FALSE(cell_only_notif.last_report.ue_metrics.empty()) << "UE metrics must still be computed";
  ASSERT_FALSE(cell_only_notif.last_report.report_ue_metrics) << "Per-UE reporting flag must be disabled";
}

TEST_F(scheduler_metrics_handler_tester, when_no_events_took_place_then_metrics_are_zero)
{
  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];

  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0);
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0);
  ASSERT_EQ(ue_metrics.dl_mcs, 0);
  ASSERT_EQ(ue_metrics.ul_mcs, 0);
  ASSERT_EQ(ue_metrics.dl_nof_ok, 0);
  ASSERT_EQ(ue_metrics.dl_nof_nok, 0);
  ASSERT_EQ(ue_metrics.ul_nof_ok, 0);
  ASSERT_EQ(ue_metrics.ul_nof_nok, 0);
  ASSERT_EQ(ue_metrics.pusch_snr_db, 0);
  ASSERT_EQ(ue_metrics.pusch_rsrp_db, -std::numeric_limits<float>::infinity());
  ASSERT_EQ(ue_metrics.pucch_snr_db, 0);
  ASSERT_EQ(ue_metrics.bsr, 0);

  // Check that the CQI and RI statistics have no observations.
  ASSERT_EQ(ue_metrics.cqi_stats.get_nof_observations(), 0);
  ASSERT_EQ(ue_metrics.dl_ri_stats.get_nof_observations(), 0);
  ASSERT_EQ(ue_metrics.ul_ri_stats.get_nof_observations(), 0);
}

TEST_F(scheduler_metrics_handler_tester, compute_nof_dl_oks_and_noks)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_dl_harq_ack(test_ue_index, true, units::bytes{1});
  }
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_dl_harq_ack(test_ue_index, false, units::bytes{1});
  }

  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.dl_nof_nok, nof_nacks);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_nof_ok, 0) << "Nof DL OKs not reset";
  ASSERT_EQ(ue_metrics.dl_nof_nok, 0) << "Nof DL NOKs not reset";
}

TEST_F(scheduler_metrics_handler_tester, compute_nof_ul_oks_and_noks)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = true;
  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }
  crc_pdu.tb_crc_success = false;
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }

  this->get_next_metric();
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.pci, pci_t{0});
  ASSERT_EQ(ue_metrics.ul_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.ul_nof_nok, nof_nacks);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.ul_nof_ok, 0) << "Nof DL OKs not reset";
  ASSERT_EQ(ue_metrics.ul_nof_nok, 0) << "Nof DL NOKs not reset";
}

TEST_F(scheduler_metrics_handler_tester, compute_mcs)
{
  sch_mcs_index dl_mcs{test_rng::uniform_int<uint8_t>(1, 28)};
  sch_mcs_index ul_mcs{test_rng::uniform_int<uint8_t>(1, 28)};

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  sched_result res;
  res.dl.nof_dl_symbols = 14;
  res.ul.nof_ul_symbols = 14;
  dl_msg_alloc& dl_msg  = res.dl.ue_grants.emplace_back();
  dl_msg.pdsch_cfg.rnti = to_rnti(0x4601);
  auto&          cw     = dl_msg.pdsch_cfg.codewords.emplace_back();
  ul_sched_info& ul_msg = res.ul.puschs.emplace_back();
  ul_msg.pusch_cfg.rnti = to_rnti(0x4601);

  for (unsigned i = 0; i != report_period.count() and metrics_notif.last_report.nof_slots == 0; ++i) {
    cw.mcs_index               = dl_mcs;
    ul_msg.pusch_cfg.mcs_index = ul_mcs;
    run_slot(res);
  }

  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_mcs, dl_mcs);
  ASSERT_EQ(ue_metrics.ul_mcs, ul_mcs);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_mcs, 0) << "DL MCS not reset";
  ASSERT_EQ(ue_metrics.ul_mcs, 0) << "UL MCS not reset";
}

TEST_F(scheduler_metrics_handler_tester, compute_bitrate)
{
  units::bytes dl_tbs{test_rng::uniform_int<unsigned>(1, 1000)};
  units::bytes ul_tbs{test_rng::uniform_int<unsigned>(1, 1000)};

  // Slot with bytes ACKed.
  metrics.handle_dl_harq_ack(test_ue_index, true, dl_tbs);
  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = true;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, ul_tbs);

  this->get_next_metric();
  unsigned             msec_elapsed = metrics_notif.last_report.nof_slots * next_sl_tx.nof_slots_per_subframe();
  scheduler_ue_metrics ue_metrics   = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.pci, pci_t{0});
  ASSERT_EQ(ue_metrics.dl_brate_kbps, static_cast<double>(dl_tbs.value() * 8) / msec_elapsed);
  ASSERT_EQ(ue_metrics.ul_brate_kbps, static_cast<double>(ul_tbs.value() * 8) / msec_elapsed);

  // Slot with no ACKs.
  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0) << "DL bitrate not reset";
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0) << "UL bitrate not reset";

  // Slot with bytes NACKed.
  metrics.handle_dl_harq_ack(test_ue_index, false, dl_tbs);
  crc_pdu.rnti           = to_rnti(0x4601);
  crc_pdu.ue_index       = test_ue_index;
  crc_pdu.tb_crc_success = false;
  metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, ul_tbs);

  this->get_next_metric();
  ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.dl_brate_kbps, 0) << "NACKs should not count for bitrate";
  ASSERT_EQ(ue_metrics.ul_brate_kbps, 0) << "NACKs should not count for bitrate";
}

TEST_F(scheduler_metrics_handler_tester, compute_latency_metric)
{
  using usecs                = std::chrono::microseconds;
  std::vector<usecs> samples = {usecs{10}, usecs{50}, usecs{150}, usecs{300}, usecs{500}};
  samples.resize(report_period.count());

  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;

  for (unsigned i = 0; i != samples.size(); ++i) {
    ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());
    this->run_slot(sched_res, samples[i]);
  }
  ASSERT_FALSE(metrics_notif.last_report.ue_metrics.empty());
  scheduler_cell_metrics cell_metrics = metrics_notif.last_report;

  unsigned expected_avg = std::accumulate(samples.begin(), samples.end(), usecs{0}).count() / report_period.count();
  ASSERT_EQ(cell_metrics.average_decision_latency.count(), expected_avg);

  for (unsigned i = 0; i != cell_metrics.latency_histogram.size(); ++i) {
    unsigned expected = std::count_if(samples.begin(), samples.end(), [i](usecs u) {
      unsigned bin = u.count() / scheduler_cell_metrics::nof_usec_per_bin;
      bin          = std::min(bin, scheduler_cell_metrics::latency_hist_bins - 1);
      return bin == i;
    });
    ASSERT_EQ(cell_metrics.latency_histogram[i], expected);
  }
}

TEST_F(scheduler_metrics_handler_tester, when_ue_is_removed_mid_period_then_its_last_metrics_are_still_reported)
{
  unsigned nof_acks  = test_rng::uniform_int<unsigned>(1, 100);
  unsigned nof_nacks = test_rng::uniform_int<unsigned>(1, 100);

  ul_crc_pdu_indication crc_pdu;
  crc_pdu.rnti     = to_rnti(0x4601);
  crc_pdu.ue_index = test_ue_index;

  crc_pdu.tb_crc_success = true;
  for (unsigned i = 0; i != nof_acks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }
  crc_pdu.tb_crc_success = false;
  for (unsigned i = 0; i != nof_nacks; ++i) {
    metrics.handle_crc_indication(next_sl_tx.without_hyper_sfn() - 1, crc_pdu, units::bytes{1});
  }

  // Remove the UE before the periodic report is generated.
  metrics.handle_ue_deletion(test_ue_index);

  // The next report must still carry the metrics accumulated by the removed UE.
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.ue_metrics.size(), 1);
  scheduler_ue_metrics ue_metrics = metrics_notif.last_report.ue_metrics[0];
  ASSERT_EQ(ue_metrics.rnti, to_rnti(0x4601));
  ASSERT_EQ(ue_metrics.ul_nof_ok, nof_acks);
  ASSERT_EQ(ue_metrics.ul_nof_nok, nof_nacks);

  // The removed UE must not be reported again on subsequent reports.
  metrics_notif.last_report = {};
  sched_result sched_res;
  sched_res.dl.nof_dl_symbols = 14;
  sched_res.ul.nof_ul_symbols = 14;
  for (unsigned i = 0; i != report_period.count() * next_sl_tx.nof_slots_per_subframe(); ++i) {
    run_slot(sched_res);
  }
  ASSERT_TRUE(metrics_notif.last_report.ue_metrics.empty());
}

TEST_F(scheduler_metrics_handler_tester, compute_error_indications)
{
  metrics.handle_error_indication();
  metrics.handle_error_indication();
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.nof_error_indications, 2);
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.nof_error_indications, 0);
}

TEST_F(scheduler_metrics_handler_tester, failed_pdcch_allocs_are_accumulated_per_dl_ul_and_common)
{
  // Discard first report, as it may have not enough slots.
  get_next_metric();
  metrics_notif.last_report = {};

  // Per-slot failures: two DL (one of which in a common SearchSpace) and one common UL.
  sched_result sched_res;
  sched_res.dl.nof_dl_symbols               = 14;
  sched_res.ul.nof_ul_symbols               = 14;
  sched_res.failed_attempts.dl_pdcch        = 2;
  sched_res.failed_attempts.common_dl_pdcch = 1;
  sched_res.failed_attempts.ul_pdcch        = 1;
  sched_res.failed_attempts.common_ul_pdcch = 1;

  for (unsigned i = 0; i != report_period.count() and metrics_notif.last_report.nof_slots == 0; ++i) {
    run_slot(sched_res);
  }

  const scheduler_cell_metrics& report = metrics_notif.last_report;
  ASSERT_GT(report.nof_slots, 0);
  ASSERT_EQ(report.failed_dl_pdcch, report.nof_slots * 2);
  ASSERT_EQ(report.failed_common_dl_pdcch, report.nof_slots * 1);
  ASSERT_EQ(report.failed_ul_pdcch, report.nof_slots * 1);
  ASSERT_EQ(report.failed_common_ul_pdcch, report.nof_slots * 1);

  // Counters reset on the next report.
  this->get_next_metric();
  ASSERT_EQ(metrics_notif.last_report.failed_dl_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_common_dl_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_ul_pdcch, 0);
  ASSERT_EQ(metrics_notif.last_report.failed_common_ul_pdcch, 0);
}

TEST(scheduler_slice_metrics_test, counts_grants_by_slice_and_resets_each_window)
{
  test_helpers::test_sched_config_manager manager{config_helpers::make_default_scheduler_expert_config()};
  auto request = sched_config_helper::make_default_sched_cell_configuration_request();
  request.rrm_policy_members = {{{plmn_identity::test_value(), s_nssai_t{slice_service_type{1}}}, {0, 50}}};
  const auto& cfg = *manager.add_cell(request);
  test_scheduler_cell_metrics_notifier notifier;
  notifier.period_slots = 1;
  cell_metrics_handler handler(cfg, sched_cell_configuration_request_message::metrics_config{&notifier});
  handler.handle_ue_creation(to_du_ue_index(0), to_rnti(0x4601), pci_t{0});
  handler.handle_ue_creation(to_du_ue_index(1), to_rnti(0x4602), pci_t{0});
  sched_result result;
  result.dl.nof_dl_symbols = result.ul.nof_ul_symbols = 14;
  for (unsigned i = 0; i != 2; ++i) {
    auto& dl = result.dl.ue_grants.emplace_back();
    dl.pdsch_cfg.rnti = to_rnti(0x4601+i);
    dl.pdsch_cfg.rbs = vrb_interval{0, 10};
    dl.context.slice_index = 2;
    auto& ul = result.ul.puschs.emplace_back();
    ul.pusch_cfg.rnti = to_rnti(0x4601+i);
    ul.pusch_cfg.rbs = vrb_interval{0, 7};
    ul.context.slice_index = 2;
  }
  slot_point_extended slot{subcarrier_spacing::kHz15, 0};
  handler.push_result(slot, result, std::chrono::microseconds{0});
  ASSERT_EQ(notifier.last_report.slice_metrics.size(), 1);
  ASSERT_EQ(notifier.last_report.slice_metrics[0].dl_prbs, 20);
  ASSERT_EQ(notifier.last_report.slice_metrics[0].ul_prbs, 14);
  // Next window: no grants; prior counts must not leak into it.
  sched_result empty;
  empty.dl.nof_dl_symbols = empty.ul.nof_ul_symbols = 14;
  handler.push_result(slot+1, empty, std::chrono::microseconds{0});
  ASSERT_EQ(notifier.last_report.slice_metrics[0].dl_prbs, 0);
  ASSERT_EQ(notifier.last_report.slice_metrics[0].ul_prbs, 0);
}

TEST(scheduler_slice_metrics_test, queue_need_uses_eligible_opportunities_and_resets_together)
{
  test_helpers::test_sched_config_manager manager{config_helpers::make_default_scheduler_expert_config()};
  auto request = sched_config_helper::make_default_sched_cell_configuration_request();
  request.rrm_policy_members = {{{plmn_identity::test_value(), s_nssai_t{slice_service_type{1}}}, {0, 50}}};
  const auto& cfg = *manager.add_cell(request);
  test_scheduler_cell_metrics_notifier notifier;
  notifier.period_slots = 1;
  cell_metrics_handler handler(cfg, sched_cell_configuration_request_message::metrics_config{&notifier});
  handler.handle_slice_queue_need(0, true, 40, 5, 120, true);
  handler.handle_slice_queue_need(0, false, 20, 2, 30, false);
  sched_result result;
  result.dl.nof_dl_symbols = result.ul.nof_ul_symbols = 14;
  slot_point_extended slot{subcarrier_spacing::kHz15, 0};
  handler.push_result(slot, result, std::chrono::microseconds{0});
  const auto& first = notifier.last_report.slice_metrics.at(0);
  EXPECT_EQ(first.dl_queue_newtx_prbs, 40);
  EXPECT_EQ(first.dl_queue_retx_prbs, 5);
  EXPECT_EQ(first.dl_queue_unserved_bytes, 120);
  EXPECT_EQ(first.dl_queue_samples, 1);
  EXPECT_EQ(first.dl_queue_invalid_samples, 0);
  EXPECT_EQ(first.ul_queue_samples, 1);
  EXPECT_EQ(first.ul_queue_invalid_samples, 1);
  handler.push_result(slot + 1, result, std::chrono::microseconds{0});
  const auto& second = notifier.last_report.slice_metrics.at(0);
  EXPECT_EQ(second.dl_queue_newtx_prbs, 0);
  EXPECT_EQ(second.dl_queue_samples, 0);
  EXPECT_EQ(second.ul_queue_invalid_samples, 0);
}

// FYP: offered (newly arrived) traffic in peer-scale PRBs. Expected values come from the Python reference
// fyp-5g-network-slicing/rApp/horizon_pretraining/peer_demand_reference.py (ue_prb with peer_v1 constants).
namespace {

struct offered_bench {
  explicit offered_bench(unsigned period_slots, std::vector<unsigned> ssts = {1})
  {
    auto request = sched_config_helper::make_default_sched_cell_configuration_request();
    for (unsigned sst : ssts) {
      request.rrm_policy_members.push_back(
          {{plmn_identity::test_value(), s_nssai_t{slice_service_type{static_cast<uint8_t>(sst)}}}, {0, 50}});
    }
    cfg                   = manager.add_cell(request);
    notifier.period_slots = period_slots;
    handler = std::make_unique<cell_metrics_handler>(*cfg, sched_cell_configuration_request_message::metrics_config{&notifier});
    result.dl.nof_dl_symbols = result.ul.nof_ul_symbols = 14;
  }

  // 15 kHz SCS: one slot per ms, so a sub-window is 100 slots.
  void run(unsigned nof_slots)
  {
    for (unsigned i = 0; i != nof_slots; ++i) {
      handler->push_result(slot, result, std::chrono::microseconds{0});
      ++slot;
    }
  }

  void dl(du_ue_index_t ue, uint64_t bytes, std::optional<double> se, unsigned slice = 0)
  {
    handler->handle_slice_ue_arrivals(slice, ue, {bytes, 0}, se, {}, std::nullopt);
  }

  void ul(du_ue_index_t ue, int64_t net, std::optional<double> se, bool unknown = false, unsigned slice = 0)
  {
    handler->handle_slice_ue_arrivals(slice, ue, {}, std::nullopt, {net, unknown}, se);
  }

  const scheduler_slice_metrics& report(unsigned slice = 0) const { return notifier.last_report.slice_metrics.at(slice); }

  test_helpers::test_sched_config_manager manager{config_helpers::make_default_scheduler_expert_config()};
  const cell_configuration*               cfg = nullptr;
  test_scheduler_cell_metrics_notifier    notifier;
  std::unique_ptr<cell_metrics_handler>   handler;
  sched_result                            result;
  slot_point_extended                     slot{subcarrier_spacing::kHz15, 0};
};

const double se_cqi10 = fyp_offered::se_from_cqi_table1(10).value();

double dl_mean(const scheduler_slice_metrics& m)
{
  return m.dl_offered_prb_peer_slots / m.offered_slots;
}

double ul_mean(const scheduler_slice_metrics& m)
{
  return m.ul_offered_prb_peer_slots / m.offered_slots;
}

} // namespace

TEST(scheduler_offered_metrics_test, dl_arrival_matches_python_reference_and_averages_over_sub_windows)
{
  offered_bench b(1000);
  b.dl(to_du_ue_index(0), 5000, se_cqi10);
  b.run(1000);
  const auto& m = b.report();
  EXPECT_EQ(m.offered_slots, 1000U);
  EXPECT_EQ(m.dl_arrived_bytes, 5000U);
  // 5000 bytes in one 100 ms sub-window = 0.7242392 PRB there, 0 in the other nine.
  EXPECT_NEAR(dl_mean(m), 0.7242392236317752 / 10, 1e-9);
  EXPECT_EQ(m.dl_offered_invalid_subwindows, 0U);
  EXPECT_EQ(ul_mean(m), 0.0);
}

TEST(scheduler_offered_metrics_test, per_ue_cap_applies_to_the_ue_total_of_a_sub_window)
{
  offered_bench b(1000);
  // UE 0: two arrivals summing to 120 PRB in the same sub-window are capped together at 60 (eMBB).
  b.dl(to_du_ue_index(0), 2500000, se_cqi10);
  b.dl(to_du_ue_index(0), 2500000, se_cqi10);
  // UE 1: its own cap of 60.
  b.dl(to_du_ue_index(1), 5000000, se_cqi10);
  b.run(1000);
  EXPECT_NEAR(dl_mean(b.report()), (60.0 + 60.0) / 10, 1e-9);
}

TEST(scheduler_offered_metrics_test, ul_uses_grant_overhead_min_grant_and_signed_carry)
{
  offered_bench b(1000);
  const auto ue = to_du_ue_index(0);
  // Sub-window 0: BSR quantisation makes the net negative; nothing is offered, the deficit is carried.
  b.ul(ue, -100, se_cqi10);
  b.run(100);
  // Sub-window 1: 20100 - 100 = 20000 bytes = 8.3299287 PRB.
  b.ul(ue, 20100, se_cqi10);
  b.run(100);
  // Sub-window 2: 200 bytes is below the minimum grant, so 0.25 PRB.
  b.ul(ue, 200, se_cqi10);
  b.run(800);
  const auto& m = b.report();
  EXPECT_EQ(m.ul_arrived_bytes, 20200U);
  EXPECT_NEAR(ul_mean(m), (8.329928696519286 + 0.25) / 10, 1e-9);
  EXPECT_EQ(m.ul_offered_invalid_subwindows, 0U);
}

TEST(scheduler_offered_metrics_test, missing_conversion_inputs_mark_sub_windows_invalid_and_reset)
{
  offered_bench b(1000);
  b.dl(to_du_ue_index(0), 5000, std::nullopt);           // CQI unavailable.
  b.ul(to_du_ue_index(0), 0, se_cqi10, /*unknown=*/true); // Saturated BSR not applied.
  b.run(1000);
  EXPECT_EQ(b.report().dl_offered_invalid_subwindows, 1U);
  EXPECT_EQ(b.report().ul_offered_invalid_subwindows, 1U);
  b.run(1000);
  EXPECT_EQ(b.report().dl_offered_invalid_subwindows, 0U);
  EXPECT_EQ(b.report().ul_offered_invalid_subwindows, 0U);
  EXPECT_EQ(b.report().dl_arrived_bytes, 0U);
  EXPECT_EQ(b.report().offered_slots, 1000U);
}

TEST(scheduler_offered_metrics_test, report_boundary_closes_a_partial_sub_window)
{
  offered_bench b(150);
  b.run(120);
  // Slots 100..149 form a 50 ms sub-window cut by the report: the same bytes need twice the PRBs.
  b.dl(to_du_ue_index(0), 5000, se_cqi10);
  b.run(30);
  const auto& m = b.report();
  EXPECT_EQ(m.offered_slots, 150U);
  EXPECT_NEAR(dl_mean(m), 2 * 0.7242392236317752 * 50 / 150, 1e-9);
}

TEST(scheduler_offered_metrics_test, non_standard_sst_with_arrivals_is_invalid)
{
  offered_bench b(1000, {4});
  b.dl(to_du_ue_index(0), 5000, se_cqi10);
  b.run(1000);
  EXPECT_EQ(b.report().dl_offered_invalid_subwindows, 1U);
  EXPECT_EQ(b.report().ul_offered_invalid_subwindows, 1U);
  EXPECT_EQ(dl_mean(b.report()), 0.0);
}

TEST(scheduler_offered_metrics_test, spectral_efficiency_follows_the_reported_cqi_table_and_is_clamped_to_peer_domain)
{
  EXPECT_DOUBLE_EQ(fyp_offered::peer_se_from_cqi(10, 1).value(), 2.7305);
  EXPECT_DOUBLE_EQ(fyp_offered::peer_se_from_cqi(7, 2).value(), 2.7305);
  EXPECT_DOUBLE_EQ(fyp_offered::peer_se_from_cqi(12, 3).value(), 2.7305);
  EXPECT_DOUBLE_EQ(fyp_offered::peer_se_from_cqi(15, 2).value(), fyp_offered::peer_max_se);
  EXPECT_FALSE(fyp_offered::peer_se_from_cqi(0, 1).has_value());
  EXPECT_FALSE(fyp_offered::peer_se_from_cqi(5, 4).has_value());
  EXPECT_DOUBLE_EQ(fyp_offered::peer_se(7.0).value(), fyp_offered::peer_max_se);
  EXPECT_FALSE(fyp_offered::peer_se(0.0).has_value());
}
