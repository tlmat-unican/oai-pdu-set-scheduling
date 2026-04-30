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


#include "mac_data_ie.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "../../../util/alg_ds/alg/eq_float.h"


//////////////////////////////////////
// RIC Event Trigger Definition
/////////////////////////////////////

void free_mac_event_trigger(mac_event_trigger_t* src)
{
  assert(src != NULL);
  assert(0!=0 && "Not implemented" ); 
}

mac_event_trigger_t cp_mac_event_trigger( mac_event_trigger_t const* src)
{
  assert(src != NULL);
  assert(0!=0 && "Not implemented" ); 

  mac_event_trigger_t et = {0};
  return et;
}

bool eq_mac_event_trigger(mac_event_trigger_t const* m0, mac_event_trigger_t const* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);

  assert(0!=0 && "Not implemented" ); 

  return true;
}


//////////////////////////////////////
// RIC Action Definition 
/////////////////////////////////////

void free_mac_action_def(mac_action_def_t* src)
{
  assert(src != NULL);
}

mac_action_def_t cp_mac_action_def(mac_action_def_t* src)
{
  assert(src != NULL);
  
  mac_action_def_t dst = {
    .dummy = src->dummy
  };
  
  return dst;
}

bool eq_mac_action_def(mac_event_trigger_t* m0,  mac_event_trigger_t* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);
  
  return true;
}


//////////////////////////////////////
// RIC Indication Header 
/////////////////////////////////////


void free_mac_ind_hdr(mac_ind_hdr_t* src)
{
  assert(src != NULL);
  (void)src;
}

mac_ind_hdr_t cp_mac_ind_hdr(mac_ind_hdr_t const* src)
{
  assert(src != NULL);
  mac_ind_hdr_t dst = {0}; 
  dst.dummy = src->dummy;
  return dst;
}

bool eq_mac_ind_hdr(mac_ind_hdr_t* m0, mac_ind_hdr_t* m1)
{
  assert(m0 != 0);
  assert(m1 != 0);

  if(m0->dummy != m1->dummy)
    return false;
  return true;
}


//////////////////////////////////////
// RIC Indication Message 
/////////////////////////////////////

void free_mac_ind_msg(mac_ind_msg_t* src)
{
  assert(src != NULL);

  if(src->len_ue_stats > 0){
    assert(src->ue_stats != NULL);

    // 1. FIRST: Free inner content (the queue array of each UE)
    for (size_t i = 0; i < src->len_ue_stats; i++) {
      // Check if the queue exists before freeing it
      if (src->ue_stats[i].queues != NULL) {
        free(src->ue_stats[i].queues);
        src->ue_stats[i].queues = NULL;
      }
      if (src->ue_stats[i].virtual_queues != NULL) {
        free(src->ue_stats[i].virtual_queues);
        src->ue_stats[i].virtual_queues = NULL;
      }
      if (src->ue_stats[i].mcs != NULL) {
        free(src->ue_stats[i].mcs);
        src->ue_stats[i].mcs = NULL;
      }
      if (src->ue_stats[i].sensing != NULL) {
        free(src->ue_stats[i].sensing);
        src->ue_stats[i].sensing = NULL;
      }
    }

    // 2. SECOND: Free the main container
    free(src->ue_stats);
    src->ue_stats = NULL; // Good practice to avoid double free
  }
}

mac_ue_stats_impl_t cp_mac_ue_stats_impl(mac_ue_stats_impl_t const* src)
{
  assert(src != NULL);

  // Copy all scalar fields and fixed arrays
  mac_ue_stats_impl_t dst = { 
    .dl_aggr_tbs = src->dl_aggr_tbs,
    .ul_aggr_tbs = src->ul_aggr_tbs,
    .dl_aggr_bytes_sdus = src->dl_aggr_bytes_sdus,
    .ul_aggr_bytes_sdus = src->ul_aggr_bytes_sdus,
    .dl_curr_tbs = src->dl_curr_tbs,
    .ul_curr_tbs = src->ul_curr_tbs,
    .dl_sched_rb = src->dl_sched_rb,
    .ul_sched_rb = src->ul_sched_rb,
    .pusch_snr = src->pusch_snr,
    .pucch_snr = src->pucch_snr,
    .rnti = src->rnti,
    .dl_aggr_prb = src->dl_aggr_prb, 
    .ul_aggr_prb = src->ul_aggr_prb,
    .dl_aggr_sdus = src->dl_aggr_sdus,
    .ul_aggr_sdus = src->ul_aggr_sdus,
    .dl_aggr_retx_prb = src->dl_aggr_retx_prb,
    .ul_aggr_retx_prb = src->ul_aggr_retx_prb,
    .wb_cqi = src->wb_cqi, 
    .dl_mcs1 = src->dl_mcs1,
    .ul_mcs1 = src->ul_mcs1,
    .dl_mcs2 = src->dl_mcs2, 
    .ul_mcs2 = src->ul_mcs2, 
    .phr = src->phr,
    .bsr = src->bsr,
    .dl_bler = src->dl_bler,
    .ul_bler = src->ul_bler,
    .dl_num_harq = src->dl_num_harq,
    .ul_num_harq = src->ul_num_harq,
    .frame = src->frame,
    .slot = src->slot,
    .n_flows = src->n_flows
  };
  
  // Copy fixed HARQ arrays
  for(uint32_t i = 0; i < 5; ++i){
    dst.dl_harq[i] = src->dl_harq[i];
    dst.ul_harq[i] = src->ul_harq[i];
  }
  
  // Initialize queues to NULL EXPLICITLY
  dst.queues = NULL;
  dst.virtual_queues = NULL;
  dst.mcs = NULL;
  dst.sensing = NULL;
  dst.bytes = NULL;
  dst.rbs = NULL;
    
  // Copy dynamic queues array (deep copy)
  if (src->n_flows > 0 && src->queues != NULL) {
    dst.queues = calloc(src->n_flows, sizeof(uint8_t));
    assert(dst.queues != NULL && "Memory exhausted");
    
    // printf("[CP] BEFORE: src->queues @ %p: [", (void*)src->queues);
    // for (uint8_t j = 0; j < src->n_flows; j++) {
    //   printf("%u", src->queues[j]);
    //   if (j < src->n_flows - 1) printf(", ");
    // }
    // printf("]\n");
    
    memcpy(dst.queues, src->queues, src->n_flows * sizeof(uint8_t));
    
    // printf("[CP] AFTER:  dst.queues @ %p: [", (void*)dst.queues);
    // for (uint8_t j = 0; j < dst.n_flows; j++) {
    //   printf("%u", dst.queues[j]);
    //   if (j < dst.n_flows - 1) printf(", ");
    // }
    // printf("]\n");
  }

  // Copy dynamic virtual_queues array (deep copy)
  if (src->n_flows > 0 && src->virtual_queues != NULL) {
    dst.virtual_queues = calloc(src->n_flows, sizeof(uint8_t));
    assert(dst.virtual_queues != NULL && "Memory exhausted");
    memcpy(dst.virtual_queues, src->virtual_queues, src->n_flows * sizeof(uint8_t));
  }
  // Copy dynamic mcs array (deep copy)
  if (src->n_flows > 0 && src->mcs != NULL) {
    dst.mcs = calloc(src->n_flows, sizeof(uint8_t));
    assert(dst.mcs != NULL && "Memory exhausted");
    memcpy(dst.mcs, src->mcs, src->n_flows * sizeof(uint8_t));
  }
  // Copy dynamic sensing array (deep copy)
  if (src->n_flows > 0 && src->sensing != NULL) {
    dst.sensing = calloc(src->n_flows, sizeof(uint8_t));
    assert(dst.sensing != NULL && "Memory exhausted");
    memcpy(dst.sensing, src->sensing, src->n_flows * sizeof(uint8_t));
  }

  // Copy dynamic bytes array (deep copy)
  if (src->n_flows > 0 && src->bytes != NULL) {
    dst.bytes = calloc(src->n_flows, sizeof(uint64_t));
    assert(dst.bytes != NULL && "Memory exhausted");
    memcpy(dst.bytes, src->bytes, src->n_flows * sizeof(uint64_t));
  }

  // Copy dynamic rbs array (deep copy)
  if (src->n_flows > 0 && src->rbs != NULL) {
    dst.rbs = calloc(src->n_flows, sizeof(uint32_t));
    assert(dst.rbs != NULL && "Memory exhausted");
    memcpy(dst.rbs, src->rbs, src->n_flows * sizeof(uint32_t));
  }

  return dst;
}

mac_ind_msg_t cp_mac_ind_msg( mac_ind_msg_t const* src)
{
  assert(src != NULL);

  mac_ind_msg_t dst = {0};

  dst.len_ue_stats = src->len_ue_stats;
  if(dst.len_ue_stats > 0){
    dst.ue_stats = calloc(dst.len_ue_stats, sizeof( mac_ue_stats_impl_t) );
    assert(dst.ue_stats != NULL && "Memory exhausted");
  }

  for(size_t i = 0; i < dst.len_ue_stats; ++i){
    dst.ue_stats[i] = cp_mac_ue_stats_impl(&src->ue_stats[i]); 
  }

  //memcpy(ret.ue_stats, src->ue_stats, sizeof( mac_ue_stats_impl_t )*ret.len_ue_stats);

  dst.tstamp = src->tstamp; 

  return dst;
}

bool eq_mac_ind_msg(mac_ind_msg_t* m0, mac_ind_msg_t* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);

  if(m0->len_ue_stats != m1->len_ue_stats || m0->tstamp != m1->tstamp)
    return false;

  for(uint32_t i = 0 ; i < m0->len_ue_stats; ++i){
    mac_ue_stats_impl_t* ue0 = &m0->ue_stats[i]; 
    mac_ue_stats_impl_t* ue1 = &m1->ue_stats[i]; 

    if(
        ue0->dl_aggr_tbs != ue1->dl_aggr_tbs ||
        ue0->ul_aggr_tbs != ue1->ul_aggr_tbs ||
        ue0->dl_aggr_bytes_sdus != ue1->dl_aggr_bytes_sdus ||
        ue0->ul_aggr_bytes_sdus != ue1->ul_aggr_bytes_sdus ||
        ue0->dl_curr_tbs != ue1->dl_curr_tbs ||
        ue0->ul_curr_tbs != ue1->ul_curr_tbs ||
        ue0->dl_sched_rb != ue1->dl_sched_rb ||
        ue0->ul_sched_rb != ue1->ul_sched_rb ||
        ue0->rnti != ue1->rnti ||
        ue0->dl_aggr_prb != ue1->dl_aggr_prb ||  
        ue0->ul_aggr_prb != ue1->ul_aggr_prb ||
        ue0->dl_aggr_sdus != ue1-> dl_aggr_sdus ||
        ue0->ul_aggr_sdus != ue1->ul_aggr_sdus ||
        ue0->dl_aggr_retx_prb != ue1->dl_aggr_retx_prb ||
        ue0->ul_aggr_retx_prb != ue1->ul_aggr_retx_prb ||
        ue0->wb_cqi != ue1->wb_cqi || 
        ue0->dl_mcs1 != ue1->dl_mcs1 ||
        ue0->ul_mcs1 != ue1->ul_mcs1 ||
        ue0->dl_mcs2 != ue1->dl_mcs2 || 
        ue0->ul_mcs2 != ue1->ul_mcs2 || 
        ue0->phr != ue1->phr || 
        ue0->bsr != ue1->bsr ||
        ue0->dl_bler != ue1->dl_bler ||
        ue0->ul_bler != ue1->ul_bler ||
        ue0->dl_num_harq != ue1->dl_num_harq ||
        ue0->dl_harq[0] != ue1->dl_harq[0] ||
        ue0->dl_harq[1] != ue1->dl_harq[1] ||
        ue0->dl_harq[2] != ue1->dl_harq[2] ||
        ue0->dl_harq[3] != ue1->dl_harq[3] ||
        ue0->dl_harq[4] != ue1->dl_harq[4] ||
        ue0->ul_num_harq != ue1->ul_num_harq ||
        ue0->ul_harq[0] != ue1->ul_harq[0] ||
        ue0->ul_harq[1] != ue1->ul_harq[1] ||
        ue0->ul_harq[2] != ue1->ul_harq[2] ||
        ue0->ul_harq[3] != ue1->ul_harq[3] ||
        ue0->ul_harq[4] != ue1->ul_harq[4] ||
        ue0->frame != ue1->frame ||
        ue0->slot != ue1->slot ||
        ue0->n_flows != ue1->n_flows || // Comparar número de usuarios
        eq_float(ue0->pusch_snr, ue1->pusch_snr, 0.0000001) == false ||
        eq_float(ue0->pucch_snr, ue0->pucch_snr, 0.0000001) == false 
      )
      return false;
      // Compare queues arrays
      if (ue0->n_flows > 0) {
        if (memcmp(ue0->queues, ue1->queues, ue0->n_flows * sizeof(uint8_t)) != 0)
          return false;
      }
      // Compare virtual_queues arrays
      if (ue0->n_flows > 0) {
        if (memcmp(ue0->virtual_queues, ue1->virtual_queues, ue0->n_flows * sizeof(uint8_t)) != 0)
          return false;
      }
      // Compare mcs arrays
      if (ue0->n_flows > 0) {
        if (memcmp(ue0->mcs, ue1->mcs, ue0->n_flows * sizeof(uint8_t)) != 0)
          return false;
      }
      // Compare sensing arrays
      if (ue0->n_flows > 0) {
        if (memcmp(ue0->sensing, ue1->sensing, ue0->n_flows * sizeof(uint8_t)) != 0)
          return false;
      }
  }
  return true;
}

//////////////////////////////////////
// RIC Call Process ID 
/////////////////////////////////////

void free_mac_call_proc_id(mac_call_proc_id_t* src)
{
  // Note that the src could be NULL
  free(src);
}

mac_call_proc_id_t cp_mac_call_proc_id( mac_call_proc_id_t* src)
{
  assert(src != NULL); 
  mac_call_proc_id_t dst = {0};

  dst.dummy = src->dummy;

  return dst;
}

bool eq_mac_call_proc_id(mac_call_proc_id_t* m0, mac_call_proc_id_t* m1)
{
  if(m0 == NULL && m1 == NULL)
    return true;
  if(m0 == NULL)
    return false;
  if(m1 == NULL)
    return false;

  if(m0->dummy != m1->dummy)
    return false;

  return true;
}


//////////////////////////////////////
// RIC Control Header 
/////////////////////////////////////

void free_mac_ctrl_hdr(mac_ctrl_hdr_t* src)
{
  assert(src != NULL);
  // Nothing to free (only contains dummy)
}

mac_ctrl_hdr_t cp_mac_ctrl_hdr(mac_ctrl_hdr_t* src)
{
  assert(src != NULL);
  
  mac_ctrl_hdr_t dst = {0};
  dst.dummy = src->dummy;
  
  return dst;
}

bool eq_mac_ctrl_hdr(mac_ctrl_hdr_t* m0, mac_ctrl_hdr_t* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);
  
  return (m0->dummy == m1->dummy);
}


//////////////////////////////////////
// RIC Control Message 
/////////////////////////////////////

void free_mac_ctrl_msg(mac_ctrl_msg_t* src)
{
  assert(src != NULL);
  
  // Free wq array
  if (src->wq != NULL) {
    free(src->wq);
    src->wq = NULL;
  }
  
  // Free wg array
  if (src->wg != NULL) {
    free(src->wg);
    src->wg = NULL;
  }
}

mac_ctrl_msg_t cp_mac_ctrl_msg(mac_ctrl_msg_t* src)
{
  assert(src != NULL);
  
  mac_ctrl_msg_t dst = {
    .n_users = src->n_users,
    .v = src->v,
    .wq = NULL,
    .wg = NULL
  };
  
  // Copy wq array
  if (src->n_users > 0 && src->wq != NULL) {
    dst.wq = calloc(src->n_users, sizeof(uint8_t));
    assert(dst.wq != NULL && "Memory exhausted");
    memcpy(dst.wq, src->wq, src->n_users * sizeof(uint8_t));
    
    printf("[CP_CTRL] Copied control msg: n_users=%u, V=%d\n", src->n_users, src->v);
    printf("[CP_CTRL] wq: [");
    for (uint32_t i = 0; i < src->n_users; i++) {
      printf("%u", dst.wq[i]);
      if (i < src->n_users - 1) printf(", ");
    }
    printf("]\n");
  }
  
  // Copy wg array
  if (src->n_users > 0 && src->wg != NULL) {
    dst.wg = calloc(src->n_users, sizeof(uint8_t));
    assert(dst.wg != NULL && "Memory exhausted");
    memcpy(dst.wg, src->wg, src->n_users * sizeof(uint8_t));
    
    printf("[CP_CTRL] wg: [");
    for (uint32_t i = 0; i < src->n_users; i++) {
      printf("%u", dst.wg[i]);
      if (i < src->n_users - 1) printf(", ");
    }
    printf("]\n");
  }
  
  return dst;
}

bool eq_mac_ctrl_msg(mac_ctrl_msg_t* m0, mac_ctrl_msg_t* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);
  
  // Compare scalar fields
  if (m0->n_users != m1->n_users || m0->v != m1->v)
    return false;
  
  // Compare wq arrays
  if (m0->n_users > 0) {
    if (m0->wq == NULL || m1->wq == NULL)
      return false;
    if (memcmp(m0->wq, m1->wq, m0->n_users * sizeof(uint8_t)) != 0)
      return false;
  }
  
  // Compare wg arrays
  if (m0->n_users > 0) {
    if (m0->wg == NULL || m1->wg == NULL)
      return false;
    if (memcmp(m0->wg, m1->wg, m0->n_users * sizeof(uint8_t)) != 0)
      return false;
  }
  
  return true;
}


//////////////////////////////////////
// RIC Control Outcome 
/////////////////////////////////////

void free_mac_ctrl_out(mac_ctrl_out_t* src)
{
  assert(src != NULL);
  // Nothing to free (it's a pointer to enum)
  free(src);
}

mac_ctrl_out_t cp_mac_ctrl_out(mac_ctrl_out_t* src)
{
  assert(src != NULL);

  return *src;
}

bool eq_mac_ctrl_out(mac_ctrl_out_t* m0, mac_ctrl_out_t* m1)
{
  assert(m0 != NULL);
  assert(m1 != NULL);
  
  return (memcmp(m0, m1, sizeof(mac_ctrl_out_t)) == 0);
}


//////////////////////////////////////
// RAN Function Definition 
/////////////////////////////////////

void free_mac_func_def(mac_func_def_t* src)
{
  assert(src != NULL);
  free(src->buf);
}

mac_func_def_t cp_mac_func_def(mac_func_def_t const* src)
{
  assert(src != NULL);

  mac_func_def_t dst = {.len = src->len};
  if(src->len > 0){
    dst.buf = calloc(dst.len, sizeof(uint8_t)); 
    assert(dst.buf != NULL && "memory exhausted");
    memcpy(dst.buf, src->buf, dst.len);
  }

  return dst;
}

bool eq_mac_func_def(mac_func_def_t const* m0, mac_func_def_t const* m1)
{
  if(m0 == m1)
    return true;

  if(m0 == NULL || m1 == NULL)
    return false;

  if(m0->len != m1->len)
    return false;

  int rc = memcmp(m0, m1, m0->len);
  return rc == 0;
}

///////////////
// RIC Indication
///////////////

mac_ind_data_t cp_mac_ind_data( mac_ind_data_t const* src)
{
  assert(src != NULL);
  mac_ind_data_t dst = {0};
  dst.hdr = cp_mac_ind_hdr(&src->hdr);
  dst.msg = cp_mac_ind_msg(&src->msg);
  
  if(src->proc_id != NULL){
    dst.proc_id = malloc(sizeof(mac_call_proc_id_t)); 
    assert(dst.proc_id != NULL && "Memory exhausted");
    *dst.proc_id = cp_mac_call_proc_id(src->proc_id);
  }

  return dst;
}

void free_mac_ind_data(mac_ind_data_t* ind)
{
  assert(ind != NULL);
  free_mac_ind_hdr(&ind->hdr);
  free_mac_ind_msg(&ind->msg);
  free_mac_call_proc_id(ind->proc_id);
}


