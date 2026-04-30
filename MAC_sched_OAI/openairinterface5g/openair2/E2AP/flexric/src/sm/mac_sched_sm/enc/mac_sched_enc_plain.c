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



#include "mac_sched_enc_plain.h"

#include <assert.h>
#include <stdlib.h>


byte_array_t mac_enc_event_trigger_plain(mac_event_trigger_t const* event_trigger)
{
  assert(event_trigger != NULL);
  byte_array_t  ba = {0};
 
  ba.len = sizeof(event_trigger->ms);
  ba.buf = malloc(ba.len);
  assert(ba.buf != NULL && "Memory exhausted");

  memcpy(ba.buf, &event_trigger->ms, ba.len);

  return ba;
}

byte_array_t mac_enc_action_def_plain(mac_action_def_t const* action_def)
{
  assert(0!=0 && "Not implemented");

  assert(action_def != NULL);
  byte_array_t  ba = {0};
  return ba;
}

byte_array_t mac_enc_ind_hdr_plain(mac_ind_hdr_t const* ind_hdr)
{
  assert(ind_hdr != NULL);

  byte_array_t ba = {0};

  ba.len = sizeof(mac_ind_hdr_t);
  ba.buf = calloc(ba.len,  sizeof(uint8_t));
  assert(ba.buf != NULL && "memory exhausted");
  memcpy(ba.buf, ind_hdr, ba.len);

  return ba;
}

//////////////////////////////////////
// RIC Indication Message 
/////////////////////////////////////

byte_array_t mac_enc_ind_msg_plain(mac_ind_msg_t const* ind_msg)
{
  assert(ind_msg != NULL);

  // Calcular tamaño total
  uint32_t total_size = sizeof(ind_msg->len_ue_stats) + sizeof(ind_msg->tstamp);
  
  for (uint32_t i = 0; i < ind_msg->len_ue_stats; i++) {
    // Tamaño del struct básico
    total_size += sizeof(mac_ue_stats_impl_t);
    
    // Tamaño del array de queues
    total_size += ind_msg->ue_stats[i].n_users * sizeof(uint8_t);
    // Tamaño del array de virtual_queues
    total_size += ind_msg->ue_stats[i].n_users * sizeof(uint8_t);
    // Tamaño del array de mcs
    total_size += ind_msg->ue_stats[i].n_users * sizeof(uint8_t);
    // Tamaño del array de sensing
    total_size += ind_msg->ue_stats[i].n_users * sizeof(uint8_t);
  }

  byte_array_t ba = {0};
  ba.buf = calloc(1, total_size); 
  assert(ba.buf != NULL);
  ba.len = total_size;

  uint8_t* ptr = ba.buf;

  // Copiar len_ue_stats
  memcpy(ptr, &ind_msg->len_ue_stats, sizeof(ind_msg->len_ue_stats));
  ptr += sizeof(ind_msg->len_ue_stats);

  // Copiar cada UE
  for(uint32_t i = 0; i < ind_msg->len_ue_stats; ++i){
    const mac_ue_stats_impl_t* ue = &ind_msg->ue_stats[i];
    
    // Copiar struct completo (incluye n_users pero no el puntero queues)
    memcpy(ptr, ue, sizeof(mac_ue_stats_impl_t));
    ptr += sizeof(mac_ue_stats_impl_t);
    
    // Copiar array de queues
    if (ue->n_users > 0 && ue->queues != NULL) {
      size_t queues_size = ue->n_users * sizeof(uint8_t);
      memcpy(ptr, ue->queues, queues_size);
      ptr += queues_size;
    }
    // Copiar array de virtual_queues
    if (ue->n_users > 0 && ue->virtual_queues != NULL) {
      size_t vqueues_size = ue->n_users * sizeof(uint8_t);
      memcpy(ptr, ue->virtual_queues, vqueues_size);
      ptr += vqueues_size;
    }
    // Copiar array de mcs
    if (ue->n_users > 0 && ue->mcs != NULL) {
      size_t mcs_size = ue->n_users * sizeof(uint8_t);
      memcpy(ptr, ue->mcs, mcs_size);
      ptr += mcs_size;
    }
    // Copiar array de sensing
    if (ue->n_users > 0 && ue->sensing != NULL) {
      size_t sensing_size = ue->n_users * sizeof(uint8_t);
      memcpy(ptr, ue->sensing, sensing_size);
      ptr += sensing_size;
    }
  }

  // Copiar timestamp
  memcpy(ptr, &ind_msg->tstamp, sizeof(ind_msg->tstamp));

  return ba;
}

byte_array_t mac_enc_call_proc_id_plain(mac_call_proc_id_t const* call_proc_id)
{
  assert(0!=0 && "Not implemented");

  assert(call_proc_id != NULL);
  byte_array_t  ba = {0};
  return ba;
}

byte_array_t mac_enc_ctrl_hdr_plain(mac_ctrl_hdr_t const* ctrl_hdr)
{
  assert(ctrl_hdr != NULL);
  byte_array_t  ba = {0};
  ba.len = sizeof(mac_ctrl_hdr_t);
  ba.buf = calloc(ba.len ,sizeof(uint8_t)); 
  assert(ba.buf != NULL);

  memcpy(ba.buf, &ctrl_hdr->dummy, sizeof(uint32_t));

  return ba;
}

//////////////////////////////////////
// RIC Control Message 
/////////////////////////////////////

byte_array_t mac_enc_ctrl_msg_plain(mac_ctrl_msg_t const* ctrl_msg)
{
  assert(ctrl_msg != NULL);

  // Calcular tamaño: n_users(4) + V(4) + wq(n_users) + wg(n_users)
  size_t total_size = sizeof(uint8_t) + sizeof(uint8_t);
  if (ctrl_msg->n_users > 0) {
    total_size += ctrl_msg->n_users * sizeof(uint8_t);  // wq
    total_size += ctrl_msg->n_users * sizeof(uint8_t);  // wg
  }

  byte_array_t ba = {0};
  ba.buf = calloc(1, total_size);
  assert(ba.buf != NULL);
  ba.len = total_size;

  uint8_t* ptr = ba.buf;

  // Copiar n_users
  memcpy(ptr, &ctrl_msg->n_users, sizeof(ctrl_msg->n_users));
  ptr += sizeof(ctrl_msg->n_users);

  // Copiar v
  memcpy(ptr, &ctrl_msg->v, sizeof(ctrl_msg->v));
  ptr += sizeof(ctrl_msg->v);

  memcpy(ptr, ctrl_msg->wq, ctrl_msg->n_users * sizeof(uint8_t));
  ptr += ctrl_msg->n_users * sizeof(uint8_t);

  memcpy(ptr, ctrl_msg->wg, ctrl_msg->n_users * sizeof(uint8_t));
  ptr += ctrl_msg->n_users * sizeof(uint8_t);

  // log for debugging
  printf("[ENC_CTRL] Encoded control msg: n_users=%u, V=%d\n", ctrl_msg->n_users, ctrl_msg->v);
  printf("[ENC_CTRL] wq: [");
  for (uint32_t i = 0; i < ctrl_msg->n_users; i++) {
    printf("%u", ctrl_msg->wq[i]);
    if (i < ctrl_msg->n_users - 1) printf(", ");
  }
  printf("]\n");
  printf("[ENC_CTRL] wg: [");
  for (uint32_t i = 0; i < ctrl_msg->n_users; i++) {
    printf("%u", ctrl_msg->wg[i]);
    if (i < ctrl_msg->n_users - 1) printf(", ");
  }
  printf("]\n");

  return ba;
}

byte_array_t mac_enc_ctrl_out_plain(mac_ctrl_out_t const* ctrl) 
{
  assert(0!=0 && "Not implemented");

  assert(ctrl != NULL );
  byte_array_t  ba = {0};
  return ba;
}

byte_array_t mac_enc_func_def_plain(mac_func_def_t const* func)
{
  assert(0!=0 && "Not implemented");

  assert(func != NULL);
  byte_array_t  ba = {0};
  return ba;
}