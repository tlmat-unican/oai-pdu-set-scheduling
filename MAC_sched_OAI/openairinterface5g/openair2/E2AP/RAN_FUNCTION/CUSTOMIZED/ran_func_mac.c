/*
 * Licensed to the OpenAirInterface (OAI) Software Alliance under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.
 * The OpenAirInterface Software Alliance licenses this file to You under
 * the OAI Public License, Version 1.1  (the "License"); you may not use this file
 * except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.openairinterface.org/?page_id=698
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *-------------------------------------------------------------------------------
 * For more information about the OpenAirInterface (OAI) Software Alliance:
 *      contact@openairinterface.org
 */

#include "ran_func_mac.h"
#include <assert.h>

static
const int mod_id = 0;

bool read_mac_sm(void* data)
{

  assert(data != NULL);

  mac_ind_data_t* mac = (mac_ind_data_t*)data;

  mac->msg.tstamp = time_now_us();

  NR_UEs_t *UE_info = &RC.nrmac[mod_id]->UE_info; // RC.nrmac includes the MAC layer state
  // Count the number of connected UEs by iterating over the connected_ue_list until we find a NULL entry
  size_t num_ues = 0;
  UE_iterator(UE_info->connected_ue_list, ue) {
    if (ue)
      num_ues += 1;
  }

  // Calloc to allocate an array for statistics, where the size is equal to the number of connected users
  mac->msg.len_ue_stats = num_ues;
  if(mac->msg.len_ue_stats > 0){
    mac->msg.ue_stats = calloc(mac->msg.len_ue_stats, sizeof(mac_ue_stats_impl_t));
    assert(mac->msg.ue_stats != NULL && "Memory exhausted" );
  }

  size_t i = 0;
  UE_iterator(UE_info->connected_ue_list, UE) {
    const NR_UE_sched_ctrl_t* sched_ctrl = &UE->UE_sched_ctrl;
    mac_ue_stats_impl_t* rd = &mac->msg.ue_stats[i];

    rd->frame = RC.nrmac[mod_id]->frame;
    rd->slot = 0; // previously had slot info, but the gNB runs multiple slots
                  // in parallel, so this has no real meaning

    rd->dl_aggr_tbs = UE->mac_stats.dl.total_bytes;
    rd->ul_aggr_tbs = UE->mac_stats.ul.total_bytes;

    if (is_dl_slot(rd->slot, &RC.nrmac[mod_id]->frame_structure)) {
      rd->dl_curr_tbs = UE->mac_stats.dl.current_bytes;
      rd->dl_sched_rb = UE->mac_stats.dl.current_rbs;
    }
    if (is_ul_slot(rd->slot, &RC.nrmac[mod_id]->frame_structure)) {
      rd->ul_curr_tbs = UE->mac_stats.ul.current_bytes;
      rd->ul_sched_rb = sched_ctrl->sched_pusch.rbSize;
    }

    rd->rnti = UE->rnti;
    rd->dl_aggr_prb = UE->mac_stats.dl.total_rbs;
    rd->ul_aggr_prb = UE->mac_stats.ul.total_rbs;
    rd->dl_aggr_retx_prb = UE->mac_stats.dl.total_rbs_retx;
    rd->ul_aggr_retx_prb = UE->mac_stats.ul.total_rbs_retx;

    rd->dl_aggr_bytes_sdus = UE->mac_stats.dl.lc_bytes[3];
    rd->ul_aggr_bytes_sdus = UE->mac_stats.ul.lc_bytes[3];

    rd->dl_aggr_sdus = UE->mac_stats.dl.num_mac_sdu;
    rd->ul_aggr_sdus = UE->mac_stats.ul.num_mac_sdu;

    rd->pusch_snr = (float) sched_ctrl->pusch_snrx10 / 10; //: float = -64;
    rd->pucch_snr = (float) sched_ctrl->pucch_snrx10 / 10; //: float = -64;

    rd->wb_cqi = sched_ctrl->CSI_report.cri_ri_li_pmi_cqi_report.wb_cqi_1tb;
    rd->dl_mcs1 = sched_ctrl->dl_bler_stats.mcs;
    rd->dl_bler = sched_ctrl->dl_bler_stats.bler;
    rd->ul_mcs1 = sched_ctrl->ul_bler_stats.mcs;
    rd->ul_bler = sched_ctrl->ul_bler_stats.bler;
    rd->dl_mcs2 = 0;
    rd->ul_mcs2 = 0;
    rd->phr = sched_ctrl->ph;

    const uint32_t bufferSize = sched_ctrl->estimated_ul_buffer - sched_ctrl->sched_ul_bytes;
    rd->bsr = bufferSize;

    const size_t numDLHarq = 4;
    rd->dl_num_harq = numDLHarq;
    for (uint8_t j = 0; j < numDLHarq; ++j)
      rd->dl_harq[j] = UE->mac_stats.dl.rounds[j];
    rd->dl_harq[numDLHarq] = UE->mac_stats.dl.errors;

    const size_t numUlHarq = 4;
    rd->ul_num_harq = numUlHarq;
    for (uint8_t j = 0; j < numUlHarq; ++j)
      rd->ul_harq[j] = UE->mac_stats.ul.rounds[j];
    rd->ul_harq[numUlHarq] = UE->mac_stats.ul.errors;

    ////////////////
    // 1. Variable: queues
    rd->n_flows = 1; // TODO extend to multiple flows
    rd->queues = calloc(rd->n_flows, sizeof(uint8_t)); 
    assert(rd->queues != NULL && "Memory exhausted for queues");
    rd->queues[0] = sched_ctrl->num_total_bytes; // Index 0 because is hardcoded n_flows=1 for now

    // 2. Variable: virtual_queues
    rd->virtual_queues = calloc(rd->n_flows, sizeof(uint8_t));
    assert(rd->virtual_queues != NULL && "Memory exhausted for virtual_queues");
    rd->virtual_queues[0] = sched_ctrl->virtual_thput_queue[4]; // TODO

    // 3. Variable: mcs
    rd->mcs = calloc(rd->n_flows, sizeof(uint8_t));
    assert(rd->mcs != NULL && "Memory exhausted for mcs");
    // rd->mcs[i] = sched_ctrl->dl_bler_stats.mcs;
    if (sched_ctrl->dl_bler_stats.mcs <= 12)
      rd->mcs[0] = (uint8_t) 0; // test with 10
    else if (sched_ctrl->dl_bler_stats.mcs <= 18)
      rd->mcs[0] = (uint8_t) 1; // test with 16
    else
      rd->mcs[0] = (uint8_t) 2; // test with 22

    // 4. Variable: sensing
    rd->sensing = calloc(rd->n_flows, sizeof(uint8_t));
    assert(rd->sensing != NULL && "Memory exhausted for sensing");
    rd->sensing[0] = 0; // TODO

    // 5. Variables: Bytes
    rd->bytes = calloc(rd->n_flows, sizeof(uint64_t));
    assert(rd->bytes != NULL && "Memory exhausted for bytes");
    rd->bytes[0] = UE->mac_stats.dl.total_bytes;
    printf("[E2 DEBUG] total_bytes: %lu\n", rd->bytes[0]);

    // 6. Variable: rbs
    rd->rbs = calloc(rd->n_flows, sizeof(uint32_t));
    assert(rd->rbs != NULL && "Memory exhausted for rbs");
    rd->rbs[0] = UE->mac_stats.dl.total_rbs;
    printf("[E2 DEBUG] total_rbs: %u\n", rd->rbs[0]);
    ////////////////

    // printf("[E2 DEBUG] RNTI=%u, dl_mcs3=%d, q1=%d, q2=%d, q3=%d\n", rd->rnti, rd->dl_mcs3, rd->q1, rd->q2, rd->q3);
    // printf("[E2 DEBUG] RNTI=%u, q1=%d, q2=%d, q3=%d\n", rd->rnti, rd->q1, rd->q2, rd->q3);
    printf("[E2 DEBUG] sizeof(mac_ue_stats_impl_t) = %zu\n", sizeof(mac_ue_stats_impl_t));
    printf("[E2 DEBUG] sizeof(mac_ue_stats_impl_t) = %zu bytes\n", sizeof(mac_ue_stats_impl_t));
    printf("[E2 DEBUG] offset rnti = %zu\n", offsetof(mac_ue_stats_impl_t, rnti));
    printf("[E2 DEBUG] offset dl_mcs1 = %zu\n", offsetof(mac_ue_stats_impl_t, dl_mcs1));
    printf("[E2 DEBUG] offset dl_mcs2 = %zu\n", offsetof(mac_ue_stats_impl_t, dl_mcs2));

    ++i;
  }

  return num_ues > 0;
}

void read_mac_setup_sm(void* data)
{
  assert(data != NULL);
  assert(0 !=0 && "Not supported");
}

sm_ag_if_ans_t write_ctrl_mac_sm(void const* data)
{
  assert(data != NULL);
  
  mac_ctrl_req_data_t const* ctrl = (mac_ctrl_req_data_t const*)data;
  mac_ctrl_msg_t const* msg = &ctrl->msg;
  
  printf("[E2 DEBUG] ===== CONTROL RECEIVED =====\n");
  printf("[E2 DEBUG] n_users = %u\n", msg->n_users);
  printf("[E2 DEBUG] V = %u\n", msg->v);
  
  printf("[E2 DEBUG] wq = [");
  for(uint8_t i = 0; i < msg->n_users; i++) {
    printf("%u", msg->wq[i]);
    if(i < msg->n_users - 1) printf(", ");
  }
  printf("]\n");
  
  printf("[E2 DEBUG] wg = [");
  for(uint8_t i = 0; i < msg->n_users; i++) {
    printf("%u", msg->wg[i]);
    if(i < msg->n_users - 1) printf(", ");
  }
  printf("]\n");
  printf("[E2 DEBUG] ============================\n");
  
  // Apply new parameters to the scheduler
  NR_UEs_t *UE_info = &RC.nrmac[mod_id]->UE_info; // RC.nrmac includes the MAC layer state
  size_t i = 0;
  UE_iterator(UE_info->connected_ue_list, UE) {
    NR_UE_sched_ctrl_t* sched_ctrl = &UE->UE_sched_ctrl;
    sched_ctrl->e2_mac_ctrl.v = msg->v;
    sched_ctrl->e2_mac_ctrl.wq = msg->wq[i];
    sched_ctrl->e2_mac_ctrl.wg = msg->wg[i];
    printf("[E2 DEBUG] +++++++++++ Applied to RNTI=%u: V=%u, wq=%u, wg=%u\n",
           UE->rnti,
           sched_ctrl->e2_mac_ctrl.v,
           sched_ctrl->e2_mac_ctrl.wq,
           sched_ctrl->e2_mac_ctrl.wg);
    i++;
  }
  
  sm_ag_if_ans_t ans = {0};
  return ans;
}

