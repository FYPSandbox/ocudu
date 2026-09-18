// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#include "du_manager_test_helpers.h"
#include "ocudu/du/du_high/du_manager/du_configurator.h"
#include "ocudu/du/du_cell_config_helpers.h"
#include <gtest/gtest.h>
using namespace ocudu;
using namespace odu;

class du_slice_control_test : public ::testing::Test
{
protected:
  std::vector<du_cell_config> configs = [] {
    auto cell = config_helpers::make_default_du_cell_config();
    cell.rrm_policy_members = {{{plmn_identity::test_value(), s_nssai_t{slice_service_type{1}}}, {0, 50}},
                               {{plmn_identity::test_value(), s_nssai_t{slice_service_type{2}}}, {0, 50}}};
    return std::vector<du_cell_config>{cell};
  }();
  du_manager_test_bench bench{configs};
  du_cell_param_config_request request()
  {
    du_cell_param_config_request result;
    result.nr_cgi = configs[0].nr_cgi;
    rrm_policy_ratio_group policy;
    policy.policy_members_list = {configs[0].rrm_policy_members[0].rrc_member};
    policy.minimum_ratio = 10;
    policy.maximum_ratio = 40;
    policy.dedicated_ratio = 5;
    result.rrm_policy_ratio_list = {policy};
    return result;
  }
};
TEST_F(du_slice_control_test, preserves_dedicated_and_defers_cache_commit)
{
  auto result = bench.cell_mng.handle_cell_reconf_request(request());
  ASSERT_TRUE(result.has_value());
  ASSERT_TRUE(result->slice_reconf_req.has_value());
  const unsigned rbs = configs[0].ran.dl_cfg_common.init_dl_bwp.generic_params.crbs.length();
  ASSERT_EQ(result->slice_reconf_req->cell_index, to_du_cell_index(0));
  ASSERT_EQ(result->slice_reconf_req->rrm_policies[0].rbs.dedicated(), 5*rbs/100);
  ASSERT_EQ(bench.cell_mng.get_cell_cfg(to_du_cell_index(0)).rrm_policy_members[0].rbs.max(), 50);
  bench.cell_mng.commit_slice_config(*result->slice_reconf_req);
  ASSERT_EQ(bench.cell_mng.get_cell_cfg(to_du_cell_index(0)).rrm_policy_members[0].rbs.max(), 40*rbs/100);
}
TEST_F(du_slice_control_test, unknown_member_rejects_entire_batch)
{
  auto req = request();
  auto unknown = req.rrm_policy_ratio_list.front();
  unknown.policy_members_list[0].s_nssai.sst = slice_service_type{99};
  req.rrm_policy_ratio_list.push_back(unknown);
  ASSERT_FALSE(bench.cell_mng.handle_cell_reconf_request(req).has_value());
  ASSERT_EQ(bench.cell_mng.get_cell_cfg(to_du_cell_index(0)).rrm_policy_members[0].rbs.max(), 50);
}
TEST_F(du_slice_control_test, stale_expected_state_is_rejected)
{
  auto req = request();
  req.expected_rrm_policy_ratio_list = req.rrm_policy_ratio_list;
  ASSERT_FALSE(bench.cell_mng.handle_cell_reconf_request(req).has_value());
}
TEST_F(du_slice_control_test, direction_specific_and_invalid_ratios_are_rejected)
{
  auto req = request();
  req.rrm_policy_ratio_list[0].resource_type = rrm_policy_ratio_group::resource_type_t::prb_dl;
  ASSERT_FALSE(bench.cell_mng.handle_cell_reconf_request(req).has_value());
  req.rrm_policy_ratio_list[0].resource_type = rrm_policy_ratio_group::resource_type_t::prb;
  req.rrm_policy_ratio_list[0].dedicated_ratio = 70;
  ASSERT_FALSE(bench.cell_mng.handle_cell_reconf_request(req).has_value());
}

TEST_F(du_slice_control_test, malformed_expected_state_is_rejected)
{
  auto req = request();
  req.expected_rrm_policy_ratio_list.emplace_back();
  ASSERT_FALSE(bench.cell_mng.handle_cell_reconf_request(req).has_value());
}
