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



#include "mac_sched_dec_plain.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

mac_event_trigger_t mac_dec_event_trigger_plain(size_t len, uint8_t const ev_tr[len])
{
  mac_event_trigger_t ev = {0};
  memcpy(&ev.ms, ev_tr, sizeof(ev.ms));
  return ev;
}

mac_action_def_t mac_dec_action_def_plain(size_t len, uint8_t const action_def[len])
{
  assert(0!=0 && "Not implemented");
  assert(action_def != NULL);
  mac_action_def_t act_def;// = {0};
  return act_def;
}

mac_ind_hdr_t mac_dec_ind_hdr_plain(size_t len, uint8_t const ind_hdr[len])
{
  assert(len == sizeof(mac_ind_hdr_t)); 
  mac_ind_hdr_t ret;
  memcpy(&ret, ind_hdr, len);
  return ret;
}

mac_ind_msg_t mac_dec_ind_msg_plain(size_t len, uint8_t const ba[len])
{
  assert(ba != NULL);
  mac_ind_msg_t ind_msg = {0};

  uint8_t const* ptr = ba;

  // Leer len_ue_stats
  memcpy(&ind_msg.len_ue_stats, ptr, sizeof(ind_msg.len_ue_stats));
  ptr += sizeof(ind_msg.len_ue_stats);

  // Alocar memoria para UEs
  if(ind_msg.len_ue_stats > 0){
    ind_msg.ue_stats = calloc(ind_msg.len_ue_stats, sizeof(mac_ue_stats_impl_t));
    assert(ind_msg.ue_stats != NULL);
  }

  // Leer cada UE
  for(uint32_t i = 0; i < ind_msg.len_ue_stats; ++i){
    mac_ue_stats_impl_t* ue = &ind_msg.ue_stats[i];
    
    // Leer struct completo
    memcpy(ue, ptr, sizeof(mac_ue_stats_impl_t));
    ptr += sizeof(mac_ue_stats_impl_t);
    
    // Leer array de queues, virtual_queues, mcs y sensing
    if (ue->n_users > 0) {
      if (ue->n_users > 4) {
        // Limitar el tamaño máximo a 4 para evitar problemas de memoria
        printf("------------------ Warning: n_users (%u) exceeds maximum allowed (4). Limiting to 4.\n", ue->n_users);
        ue->n_users = 4;
      }
      ue->queues = calloc(ue->n_users, sizeof(uint8_t));
      assert(ue->queues != NULL);
      ue->virtual_queues = calloc(ue->n_users, sizeof(uint8_t));
      assert(ue->virtual_queues != NULL);
      ue->mcs = calloc(ue->n_users, sizeof(uint8_t));
      assert(ue->mcs != NULL);
      ue->sensing = calloc(ue->n_users, sizeof(uint8_t));
      assert(ue->sensing != NULL);

      size_t queues_size = ue->n_users * sizeof(uint8_t); // Todos los arrays tienen el mismo tamaño
      memcpy(ue->queues, ptr, queues_size);
      ptr += queues_size;
      memcpy(ue->virtual_queues, ptr, queues_size);
      ptr += queues_size;
      memcpy(ue->mcs, ptr, queues_size);
      ptr += queues_size;
      memcpy(ue->sensing, ptr, queues_size);
      ptr += queues_size;
    } else {
      ue->queues = NULL;
      ue->virtual_queues = NULL;
      ue->mcs = NULL;
      ue->sensing = NULL;
    }
    printf("++++++++++ Decoded UE %u: n_users=%u, queues=[", i, ue->n_users);
    for (uint8_t j = 0; j < ue->n_users; j++) {
      printf("%u", ue->queues[j]);
      if (j < ue->n_users - 1) {
        printf(", ");
      }
    }
    printf("]\n");
    printf("++++++++++ Decoded UE %u: n_users=%u, virtual_queues=[", i, ue->n_users);
    for (uint8_t j = 0; j < ue->n_users; j++) {
      printf("%u", ue->virtual_queues[j]);
      if (j < ue->n_users - 1) {
        printf(", ");
      }
    }
    printf("]\n");
    printf("++++++++++ Decoded UE %u: n_users=%u, mcs=[", i, ue->n_users);
    for (uint8_t j = 0; j < ue->n_users; j++) {
      printf("%u", ue->mcs[j]);
      if (j < ue->n_users - 1) {
        printf(", ");
      }
    }
    printf("]\n");
    printf("++++++++++ Decoded UE %u: n_users=%u, sensing=[", i, ue->n_users);
    for (uint8_t j = 0; j < ue->n_users; j++) {
      printf("%u", ue->sensing[j]);
      if (j < ue->n_users - 1) {
        printf(", ");
      }
    }
    printf("]\n");
  }

  // Leer timestamp
  memcpy(&ind_msg.tstamp, ptr, sizeof(ind_msg.tstamp));

  return ind_msg;
}

mac_call_proc_id_t mac_dec_call_proc_id_plain(size_t len, uint8_t const call_proc_id[len])
{
  assert(0!=0 && "Not implemented");
  assert(call_proc_id != NULL);
}

mac_ctrl_hdr_t mac_dec_ctrl_hdr_plain(size_t len, uint8_t const ctrl_hdr[len])
{
  assert(len == sizeof(mac_ctrl_hdr_t)); 
  mac_ctrl_hdr_t ret;
  memcpy(&ret, ctrl_hdr, len);
  return ret;
}

mac_ctrl_msg_t mac_dec_ctrl_msg_plain(size_t len, uint8_t const ba[len])
{
  assert(ba != NULL);
  mac_ctrl_msg_t ctrl_msg = {0};

  uint8_t const* ptr = ba;

  // Leer n_users
  memcpy(&ctrl_msg.n_users, ptr, sizeof(ctrl_msg.n_users));
  ptr += sizeof(ctrl_msg.n_users);

  // Leer v
  memcpy(&ctrl_msg.v, ptr, sizeof(ctrl_msg.v));
  ptr += sizeof(ctrl_msg.v);

  // Leer array wq
  if (ctrl_msg.n_users > 0) {
    ctrl_msg.wq = calloc(ctrl_msg.n_users, sizeof(uint8_t));
    assert(ctrl_msg.wq != NULL);
    
    size_t actions_size = ctrl_msg.n_users * sizeof(uint8_t);
    memcpy(ctrl_msg.wq, ptr, actions_size);
    ptr += actions_size;
  } else {
    ctrl_msg.wq = NULL;
  }

  // Leer array wg
  if (ctrl_msg.n_users > 0) {
    ctrl_msg.wg = calloc(ctrl_msg.n_users, sizeof(uint8_t));
    assert(ctrl_msg.wg != NULL);
    
    size_t actions_size = ctrl_msg.n_users * sizeof(uint8_t);
    memcpy(ctrl_msg.wg, ptr, actions_size);
    ptr += actions_size;
  } else {
    ctrl_msg.wg = NULL;
  }

  // log for debugging
  printf("[DEC_CTRL] Decoded control msg: n_users=%u, V=%d\n", ctrl_msg.n_users, ctrl_msg.v);
  printf("[DEC_CTRL] wq: [");
  for (uint8_t i = 0; i < ctrl_msg.n_users; i++) {
    printf("%u", ctrl_msg.wq[i]);
    if (i < ctrl_msg.n_users - 1) printf(", ");
  }
  printf("]\n");
  printf("[DEC_CTRL] wg: [");
  for (uint8_t i = 0; i < ctrl_msg.n_users; i++) {
    printf("%u", ctrl_msg.wg[i]);
    if (i < ctrl_msg.n_users - 1) printf(", ");
  }
  printf("]\n");

  return ctrl_msg;
}

mac_ctrl_out_t mac_dec_ctrl_out_plain(size_t len, uint8_t const ctrl_out[len]) 
{
  assert(0!=0 && "Not implemented");
  assert(ctrl_out != NULL);
}

mac_func_def_t mac_dec_func_def_plain(size_t len, uint8_t const func_def[len])
{
  assert(0!=0 && "Not implemented");
  assert(func_def != NULL);
}