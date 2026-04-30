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

/*! \file       gNB_scheduler_dlsch.c
 * \brief       procedures related to gNB for the DLSCH transport channel
 * \author      Guido Casati
 * \date        2019
 * \email:      guido.casati@iis.fraunhofe.de
 * \version     1.0
 * @ingroup     _mac

 */

#include "common/utils/nr/nr_common.h"
/*MAC*/
#include "NR_MAC_COMMON/nr_mac.h"
#include "NR_MAC_gNB/nr_mac_gNB.h"
#include "LAYER2/NR_MAC_gNB/mac_proto.h"
#include "openair2/LAYER2/nr_rlc/nr_rlc_oai_api.h"

/*TAG*/
#include "NR_TAG-Id.h"

/*GTP-U DSCP */
#include "openair3/ocp-gtpu/gtp_itf.h"

/*Softmodem params*/
#include "executables/softmodem-common.h"
#include "../../../nfapi/oai_integration/vendor_ext.h"

#include <pthread.h>
#include <time.h>

////////////////////////////////////////////////////////
/////* DLSCH MAC PDU generation (6.1.2 TS 38.321) */////
////////////////////////////////////////////////////////
#define OCTET 8
#define HALFWORD 16
#define WORD 32
//#define SIZE_OF_POINTER sizeof (void *)

uint64_t GFBR[5] = {3e6, 3e6, 6e6, 6e6, 6e6};  // in bps. TODO: Obtain from RRC config. For now, we use fixed values for testing
float SLOT_TIME = 0.0005;  // in seconds. TODO: Obtain from numerology. For now, we use 0.5ms (numerology 1) for testing
bool DYNAMIC_CONFIGURATION = false;
uint64_t V_DICT[4] = {0, 50, 200, 600}; // V values for scheduling
uint32_t WQ_DICT[2] = {0.1, 1}; // WQ values for scheduling
uint32_t WG_DICT[2] = {0.1, 1}; // WG values for scheduling
float psdb = 0.1;

/* DSCP bit mapping used by RTP proxy:
 * bit 5: frame type (1=I, 0=non-I)
 * bit 4: end-of-frame marker (1=last packet of frame)
 * bits 3..0: frame-size level (0..15)
 */
// static const char *dscp_size_level_desc[16] = {
//   "<10 KB",       /* 0 */
//   "10-20 KB",     /* 1 */
//   "20-30 KB",     /* 2 */
//   "30-45 KB",     /* 3 */
//   "45-60 KB",     /* 4 */
//   "60-80 KB",     /* 5 */
//   "80-130 KB",    /* 6 */
//   "130-180 KB",   /* 7 */
//   "180-230 KB",   /* 8 */
//   "230-280 KB",   /* 9 */
//   "280-330 KB",   /* 10 */
//   "330-380 KB",   /* 11 */
//   "380-430 KB",   /* 12 */
//   "430-500 KB",   /* 13 */
//   "500-750 KB",   /* 14 */
//   ">750 KB"       /* 15 */
// };
static const float dscp_size_level_desc[16] = {
  10000.0f,    /* 0 */
  15000.0f,    /* 1 */
  25000.0f,    /* 2 */
  40000.0f,    /* 3 */
  50000.0f,    /* 4 */
  70000.0f,    /* 5 */
  105000.0f,   /* 6 */
  155000.0f,   /* 7 */
  205000.0f,   /* 8 */
  255000.0f,   /* 9 */
  305000.0f,   /* 10 */
  355000.0f,   /* 11 */
  405000.0f,   /* 12 */
  465000.0f,   /* 13 */
  575000.0f,   /* 14 */
  750000.0f    /* 15 */
};

static inline void decode_dscp(uint8_t dscp,
                               bool *is_i_frame,
                               bool *is_last_packet,
                               uint8_t *size_level,
                               float *size_desc)
{
  const uint8_t dscp6 = dscp & 0x3F;
  *is_i_frame = ((dscp6 >> 5) & 0x01) != 0;
  *is_last_packet = ((dscp6 >> 4) & 0x01) != 0;
  *size_level = dscp6 & 0x0F;
  *size_desc = dscp_size_level_desc[*size_level];
}

static inline uint64_t get_monotonic_time_us(void)
{
  return time_average_now();
}

#define DSCP_TRACK_MAX_DRB 32
typedef struct {
  bool valid;
  uint8_t last_dscp;
  bool last_is_last_packet;
  uint32_t new_frame_starts;
  uint64_t frame_delta_bytes;
  uint64_t frame_size_target_bytes;
  bool end_packet_seen;
  uint64_t pdu_set_start_us;
  uint64_t pdu_set_hol_us;
  bool pdu_set_active;
} dscp_transition_tracker_t;

static dscp_transition_tracker_t dscp_tracker[MAX_MOBILES_PER_GNB][DSCP_TRACK_MAX_DRB + 1] = {0};

static uint64_t rlc_total_sdu_bytes_prev[MAX_MOBILES_PER_GNB] = {0};
static bool rlc_total_sdu_bytes_prev_valid[MAX_MOBILES_PER_GNB] = {0};

static inline uint64_t get_rlc_dequeued_delta_bytes(const NR_UE_info_t *UE)
{
  if ((unsigned int)UE->uid >= MAX_MOBILES_PER_GNB)
    return 0;

  const uint64_t curr_total = UE->mac_stats.dl.total_sdu_bytes;
  uint64_t delta = 0;

  if (rlc_total_sdu_bytes_prev_valid[UE->uid] && curr_total >= rlc_total_sdu_bytes_prev[UE->uid])
    delta = curr_total - rlc_total_sdu_bytes_prev[UE->uid];

  rlc_total_sdu_bytes_prev[UE->uid] = curr_total;
  rlc_total_sdu_bytes_prev_valid[UE->uid] = true;
  return delta;
}

// Asynchronous QoS logger: scheduler path only enqueues compact UE samples.
typedef struct {
  uint32_t timestamp_s;
  frame_t frame;
  slot_t slot;
  uint16_t rnti;
  uint16_t uid;
  float throughput_mbps;
  bool coeff_valid;
  float coeff_ue;
  uint64_t rlc_queue_bytes;
  uint64_t virtual_queue_bytes;
} qos_log_entry_t;

#define QOS_LOG_QUEUE_CAPACITY 4096

static qos_log_entry_t qos_log_queue[QOS_LOG_QUEUE_CAPACITY] = {0};
static size_t qos_log_q_head = 0;
static size_t qos_log_q_tail = 0;
static size_t qos_log_q_count = 0;
static uint64_t qos_log_dropped_entries = 0;
static pthread_mutex_t qos_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t qos_log_cond = PTHREAD_COND_INITIALIZER;
static pthread_t qos_log_thread;
static bool qos_log_thread_started = false;
static float qos_coeff_snapshot[MAX_MOBILES_PER_GNB] = {0};
static bool qos_coeff_snapshot_valid[MAX_MOBILES_PER_GNB] = {0};
static uint8_t qos_last_valid_mcs[MAX_MOBILES_PER_GNB] = {0};
static bool qos_last_valid_mcs_valid[MAX_MOBILES_PER_GNB] = {0};
static pthread_mutex_t pset_hol_file_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool pset_hol_file_initialized = false;

static void init_pset_hol_file_once(void)
{
  pthread_mutex_lock(&pset_hol_file_mutex);
  if (!pset_hol_file_initialized) {
    FILE *init_file = fopen("pset_hol.csv", "w");
    if (init_file == NULL) {
      LOG_W(NR_MAC, "Failed to initialize pset_hol.csv for pset_hol logging\n");
      pthread_mutex_unlock(&pset_hol_file_mutex);
      return;
    }
    fprintf(init_file, "Frame,Slot,Scheduler,RNTI,DRB,PduSetType,PduSetSize_bytes,PsetHol_ms\n");
    fclose(init_file);
    pset_hol_file_initialized = true;
  }
  pthread_mutex_unlock(&pset_hol_file_mutex);
}

static void log_pset_hol_before_reset(frame_t frame,
                                      slot_t slot,
                                      const char *scheduler_name,
                                      uint16_t rnti,
                                      int drb_id,
                                      bool is_i_frame,
                                      uint64_t pdu_set_size_bytes,
                                      uint64_t pset_hol_us)
{
  pthread_mutex_lock(&pset_hol_file_mutex);
  if (!pset_hol_file_initialized) {
    FILE *init_file = fopen("pset_hol.csv", "w");
    if (init_file == NULL) {
      LOG_W(NR_MAC, "Failed to initialize pset_hol.csv for pset_hol logging\n");
      pthread_mutex_unlock(&pset_hol_file_mutex);
      return;
    }
    setvbuf(init_file, NULL, _IOLBF, 0);
    fprintf(init_file, "Frame,Slot,Scheduler,RNTI,DRB,PduSetType,PduSetSize_bytes,PsetHol_ms\n");
    fclose(init_file);
    pset_hol_file_initialized = true;
  }

  FILE *pset_file = fopen("pset_hol.csv", "a");
  if (pset_file == NULL) {
    LOG_W(NR_MAC, "Failed to open pset_hol.csv for pset_hol logging\n");
    pthread_mutex_unlock(&pset_hol_file_mutex);
    return;
  }

    fprintf(pset_file,
      "%d,%d,%s,%04x,%d,%s,%llu,%.3f\n",
          frame,
          slot,
          scheduler_name,
          rnti,
          drb_id,
      is_i_frame ? "I" : "non-I",
          (unsigned long long)pdu_set_size_bytes,
          pset_hol_us / 1000.0);
  fclose(pset_file);
  pthread_mutex_unlock(&pset_hol_file_mutex);
}

/* For coefficient only: if UE is inactive and current MCS collapses to 0,
 * reuse last known valid (>0) MCS to avoid artificial priority drops. */
static inline uint8_t get_mcs_for_coeff_tbs(const NR_UE_info_t *UE, uint8_t current_mcs)
{
  return 28;
  if ((unsigned int)UE->uid >= MAX_MOBILES_PER_GNB)
    return current_mcs;

  const bool ue_inactive = UE->mac_stats.dl.current_bytes == 0;

  if (current_mcs > 0) {
    qos_last_valid_mcs[UE->uid] = current_mcs;
    qos_last_valid_mcs_valid[UE->uid] = true;
  }

  // if (ue_inactive && current_mcs == 0 && qos_last_valid_mcs_valid[UE->uid])
  if (current_mcs == 0 && qos_last_valid_mcs_valid[UE->uid])
    return qos_last_valid_mcs[UE->uid];

  return current_mcs;
}

static bool qos_queue_push(const qos_log_entry_t *entry)
{
  bool ok = true;
  pthread_mutex_lock(&qos_log_mutex);
  if (qos_log_q_count >= QOS_LOG_QUEUE_CAPACITY) {
    qos_log_dropped_entries++;
    ok = false;
  } else {
    qos_log_queue[qos_log_q_tail] = *entry;
    qos_log_q_tail = (qos_log_q_tail + 1) % QOS_LOG_QUEUE_CAPACITY;
    qos_log_q_count++;
    pthread_cond_signal(&qos_log_cond);
  }
  pthread_mutex_unlock(&qos_log_mutex);
  return ok;
}

static inline void qos_reset_coeff_snapshot(void)
{
  memset(qos_coeff_snapshot_valid, 0, sizeof(qos_coeff_snapshot_valid));
}

static inline void qos_set_coeff_snapshot(const NR_UE_info_t *UE, float coeff_ue)
{
  if ((unsigned int)UE->uid >= MAX_MOBILES_PER_GNB)
    return;
  qos_coeff_snapshot[UE->uid] = coeff_ue;
  qos_coeff_snapshot_valid[UE->uid] = true;
}

static void *qos_writer_thread(void *arg)
{
  (void)arg;
  FILE *qos_file = fopen("ue_qos_stats.csv", "w");
  if (qos_file == NULL) {
    LOG_E(MAC, "Failed to open ue_qos_stats.csv for async QoS logging\n");
    return NULL;
  }

  setvbuf(qos_file, NULL, _IOLBF, 0);
  fprintf(qos_file, "Time,Frame,Slot,RNTI,UID,Throughput_Mbps,Coeff_UE,RLC_Q_Bytes,Virtual_Q_Bytes\n");

  while (true) {
    pthread_mutex_lock(&qos_log_mutex);
    while (qos_log_q_count == 0)
      pthread_cond_wait(&qos_log_cond, &qos_log_mutex);

    qos_log_entry_t entry = qos_log_queue[qos_log_q_head];
    qos_log_q_head = (qos_log_q_head + 1) % QOS_LOG_QUEUE_CAPACITY;
    qos_log_q_count--;
    const uint64_t dropped = qos_log_dropped_entries;
    qos_log_dropped_entries = 0;
    pthread_mutex_unlock(&qos_log_mutex);

    fprintf(qos_file,
          "%u,%d,%d,%u,%u,%.3f,%.6f,%lu,%lu\n",
            entry.timestamp_s,
            entry.frame,
            entry.slot,
            entry.rnti,
            entry.uid,
            entry.throughput_mbps,
          entry.coeff_valid ? entry.coeff_ue : 0.0f,
            entry.rlc_queue_bytes,
            entry.virtual_queue_bytes);

    if (dropped > 0) {
      LOG_W(MAC, "Async QoS logger dropped %lu entries (queue full)\n", dropped);
    }
  }

  fclose(qos_file);
  return NULL;
}

static void qos_async_logger_init(void)
{
  if (qos_log_thread_started)
    return;

  if (pthread_create(&qos_log_thread, NULL, qos_writer_thread, NULL) == 0) {
    pthread_detach(qos_log_thread);
    qos_log_thread_started = true;
  } else {
    LOG_E(MAC, "Failed to create async QoS logger thread\n");
  }
}

static void write_qos_stats_to_file(module_id_t module_id, frame_t frame, slot_t slot)
{
  qos_async_logger_init();
  const uint32_t current_time = time(NULL);

  UE_iterator(RC.nrmac[module_id]->UE_info.connected_ue_list, UE) {
    const int ue_index = UE->uid;
    if ((unsigned int)ue_index >= MAX_MOBILES_PER_GNB)
      continue;

    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    uint64_t rlc_queue_bytes = 0;
    uint64_t virtual_queue_bytes = 0;
    for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
      const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
      const int lcid = c->lcid;
      rlc_queue_bytes += sched_ctrl->rlc_status[lcid].bytes_in_buffer;
      virtual_queue_bytes += max(sched_ctrl->virtual_thput_queue[lcid], 0L);
    }

    qos_log_entry_t entry = {
      .timestamp_s = current_time,
      .frame = frame,
      .slot = slot,
      .rnti = UE->rnti,
      .uid = UE->uid,
      .throughput_mbps = UE->dl_thr_ue / 1e6f,
      .coeff_valid = qos_coeff_snapshot_valid[ue_index],
      .coeff_ue = qos_coeff_snapshot[ue_index],
      .rlc_queue_bytes = rlc_queue_bytes,
      .virtual_queue_bytes = virtual_queue_bytes,
    };

    (void)qos_queue_push(&entry);
  }
}

void update_virtual_queue(NR_UE_info_t *UE){
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
   // FIXME tengo que enocntrar la forma de que a cola virtual sea 0 si no estoy transmitiendo un video
  // If there is no transmitted data and no pending DL backlog, clear virtual queue.
  if (UE->mac_stats.dl.current_bytes == 0 && sched_ctrl->num_total_bytes == 0) {
    for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
      const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
      const int lcid = c->lcid;
      sched_ctrl->virtual_thput_queue[lcid] = 0;
    }
    return;
  }

  for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
    const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
    const int lcid = c->lcid;
    // LOG_I(MAC, "UE%d LCID %d with G = %lu \n",
    //       UE->uid,
    //       lcid,
    //       sched_ctrl->virtual_thput_queue[lcid]);

    // sched_ctrl->virtual_thput_queue[lcid] = max(sched_ctrl->virtual_thput_queue[lcid] + GFBR[UE->uid] - UE->dl_thr_ue, 0L); // FIXME it should be the thput for the LCID not the UE
    sched_ctrl->virtual_thput_queue[lcid] = max(sched_ctrl->virtual_thput_queue[lcid] + UE->target_thput - UE->dl_thr_ue, 0L);
        LOG_I(MAC, "Updated virtual queue for UE%d LCID %d: G_n+1 = max(G_n + target_thput - p, 0) = max(G_n + %.2f Mbps - %.2f Mbps, 0) = %lu bps\n",
          UE->uid,
          lcid,
          UE->target_thput / 1e6f,
          UE->dl_thr_ue / 1e6f,
          sched_ctrl->virtual_thput_queue[lcid]);

    // if (sched_ctrl->virtual_thput_queue[lcid] > 0) {
      // // // LOG_I(MAC, "UE%d LCID %d with G += GFBR - p = %f - %f = %f Mbps\n",
      // // //       UE->uid,
      // // //       lcid,
      // // //       GFBR[UE->uid]/1e6,
      // // //       UE->dl_thr_ue/1e6,
      // // //       sched_ctrl->virtual_thput_queue[lcid]/1e6);
    // }
  }
}

static inline void reset_virtual_queue(NR_UE_info_t *UE)
{
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
    const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
    const int lcid = c->lcid;
    sched_ctrl->virtual_thput_queue[lcid] = 0;
  }
}

int get_dl_tda(const gNB_MAC_INST *nrmac, int slot)
{
  /* we assume that this function is mutex-protected from outside */
  const frame_structure_t *fs = &nrmac->frame_structure;

  // Use special TDA in case of CSI-RS
  if (nrmac->UE_info.sched_csirs > 0)
    return 1;

  if (fs->frame_type == TDD) {
    int s = get_slot_idx_in_period(slot, fs);
    // if there is a mixed slot where we can transmit DL
    const tdd_bitmap_t *tdd_slot_bitmap = fs->period_cfg.tdd_slot_bitmap;
    if (tdd_slot_bitmap[s].num_dl_symbols > 1 && is_mixed_slot(s, fs)) {
      return 2;
    }
  }
  return 0; // if FDD or not mixed slot in TDD, for now use default TDA
}

// Compute and write all MAC CEs and subheaders, and return number of written
// bytes
int nr_write_ce_dlsch_pdu(module_id_t module_idP,
                          const NR_UE_sched_ctrl_t *ue_sched_ctl,
                          unsigned char *mac_pdu,
                          unsigned char drx_cmd,
                          unsigned char *ue_cont_res_id)
{
  gNB_MAC_INST *gNB = RC.nrmac[module_idP];
  /* already mutex protected: called below and in _RA.c */
  NR_SCHED_ENSURE_LOCKED(&gNB->sched_lock);

  NR_MAC_SUBHEADER_FIXED *mac_pdu_ptr = (NR_MAC_SUBHEADER_FIXED *) mac_pdu;
  uint8_t last_size = 0;
  int offset = 0, mac_ce_size, i, timing_advance_cmd, tag_id = 0;
  // MAC CEs
  uint8_t mac_header_control_elements[16], *ce_ptr;
  ce_ptr = &mac_header_control_elements[0];

  // DRX command subheader (MAC CE size 0)
  if (drx_cmd != 255) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_DRX;
    //last_size = 1;
    mac_pdu_ptr++;
  }

  // Timing Advance subheader
  /* This was done only when timing_advance_cmd != 31
  // now TA is always send when ta_timer resets regardless of its value
  // this is done to avoid issues with the timeAlignmentTimer which is
  // supposed to monitor if the UE received TA or not */
  if (ue_sched_ctl->ta_apply) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_TA_COMMAND;
    //last_size = 1;
    mac_pdu_ptr++;
    // TA MAC CE (1 octet)
    timing_advance_cmd = ue_sched_ctl->ta_update;
    AssertFatal(timing_advance_cmd < 64, "timing_advance_cmd %d > 63\n", timing_advance_cmd);
    ((NR_MAC_CE_TA *) ce_ptr)->TA_COMMAND = timing_advance_cmd;    //(timing_advance_cmd+31)&0x3f;

    tag_id = gNB->tag->tag_Id;
    ((NR_MAC_CE_TA *) ce_ptr)->TAGID = tag_id;

    LOG_D(NR_MAC, "NR MAC CE timing advance command = %d (%d) TAG ID = %d\n", timing_advance_cmd, ((NR_MAC_CE_TA *) ce_ptr)->TA_COMMAND, tag_id);
    mac_ce_size = sizeof(NR_MAC_CE_TA);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *) ce_ptr, mac_ce_size);
    ce_ptr += mac_ce_size;
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  // Contention resolution fixed subheader and MAC CE
  if (ue_cont_res_id) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_CON_RES_ID;
    mac_pdu_ptr++;
    //last_size = 1;
    // contention resolution identity MAC ce has a fixed 48 bit size
    // this contains the UL CCCH SDU. If UL CCCH SDU is longer than 48 bits,
    // it contains the first 48 bits of the UL CCCH SDU
    LOG_D(NR_MAC,
          "[gNB ][RAPROC] Generate contention resolution msg: %x.%x.%x.%x.%x.%x\n",
          ue_cont_res_id[0],
          ue_cont_res_id[1],
          ue_cont_res_id[2],
          ue_cont_res_id[3],
          ue_cont_res_id[4],
          ue_cont_res_id[5]);
    // Copying bytes (6 octects) to CEs pointer
    mac_ce_size = 6;
    memcpy(ce_ptr, ue_cont_res_id, mac_ce_size);
    // Copying bytes for MAC CEs to mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *) ce_ptr, mac_ce_size);
    ce_ptr += mac_ce_size;
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.15 TCI State indication for UE Specific PDCCH MAC CE SubPDU generation
  if (ue_sched_ctl->UE_mac_ce_ctrl.pdcch_state_ind.is_scheduled) {
    //filling subheader
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_TCI_STATE_IND_UE_SPEC_PDCCH;
    mac_pdu_ptr++;
    //Creating the instance of CE structure
    NR_TCI_PDCCH  nr_UESpec_TCI_StateInd_PDCCH;
    //filling the CE structre
    nr_UESpec_TCI_StateInd_PDCCH.CoresetId1 = ((ue_sched_ctl->UE_mac_ce_ctrl.pdcch_state_ind.coresetId) & 0xF) >> 1; //extracting MSB 3 bits from LS nibble
    nr_UESpec_TCI_StateInd_PDCCH.ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.pdcch_state_ind.servingCellId) & 0x1F; //extracting LSB 5 Bits
    nr_UESpec_TCI_StateInd_PDCCH.TciStateId = (ue_sched_ctl->UE_mac_ce_ctrl.pdcch_state_ind.tciStateId) & 0x7F; //extracting LSB 7 bits
    nr_UESpec_TCI_StateInd_PDCCH.CoresetId2 = (ue_sched_ctl->UE_mac_ce_ctrl.pdcch_state_ind.coresetId) & 0x1; //extracting LSB 1 bit
    LOG_D(NR_MAC, "NR MAC CE TCI state indication for UE Specific PDCCH = %d \n", nr_UESpec_TCI_StateInd_PDCCH.TciStateId);
    mac_ce_size = sizeof(NR_TCI_PDCCH);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)&nr_UESpec_TCI_StateInd_PDCCH, mac_ce_size);
    //incrementing the PDU pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.16, SP CSI reporting on PUCCH Activation/Deactivation MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.is_scheduled) {
    //filling the subheader
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_SP_CSI_REP_PUCCH_ACT;
    mac_pdu_ptr++;
    //creating the instance of CE structure
    NR_PUCCH_CSI_REPORTING nr_PUCCH_CSI_reportingActDeact;
    //filling the CE structure
    nr_PUCCH_CSI_reportingActDeact.BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.bwpId) & 0x3; //extracting LSB 2 bibs
    nr_PUCCH_CSI_reportingActDeact.ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.servingCellId) & 0x1F; //extracting LSB 5 bits
    nr_PUCCH_CSI_reportingActDeact.S0 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[0];
    nr_PUCCH_CSI_reportingActDeact.S1 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[1];
    nr_PUCCH_CSI_reportingActDeact.S2 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[2];
    nr_PUCCH_CSI_reportingActDeact.S3 = ue_sched_ctl->UE_mac_ce_ctrl.SP_CSI_reporting_pucch.s0tos3_actDeact[3];
    nr_PUCCH_CSI_reportingActDeact.R2 = 0;
    mac_ce_size = sizeof(NR_PUCCH_CSI_REPORTING);
    // Copying MAC CE data to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)&nr_PUCCH_CSI_reportingActDeact, mac_ce_size);
    //incrementing the PDU pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  //TS 38.321 Sec 6.1.3.14, TCI State activation/deactivation for UE Specific PDSCH MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.is_scheduled) {
    //Computing the number of octects to be allocated for Flexible array member
    //of MAC CE structure
    uint8_t num_octects = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.highestTciStateActivated) / 8 + 1; //Calculating the number of octects for allocating the memory
    //filling the subheader
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->F = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->LCID = DL_SCH_LCID_TCI_STATE_ACT_UE_SPEC_PDSCH;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->L = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    last_size = 2;
    //Incrementing the PDU pointer
    mac_pdu_ptr += last_size;
    //allocating memory for CE Structure
    NR_TCI_PDSCH_APERIODIC_CSI *nr_UESpec_TCI_StateInd_PDSCH = (NR_TCI_PDSCH_APERIODIC_CSI *)malloc(sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //initializing to zero
    memset((void *)nr_UESpec_TCI_StateInd_PDSCH, 0, sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //filling the CE Structure
    nr_UESpec_TCI_StateInd_PDSCH->BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.bwpId) & 0x3; //extracting LSB 2 Bits
    nr_UESpec_TCI_StateInd_PDSCH->ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.servingCellId) & 0x1F; //extracting LSB 5 bits

    for(i = 0; i < (num_octects * 8); i++) {
      if(ue_sched_ctl->UE_mac_ce_ctrl.pdsch_TCI_States_ActDeact.tciStateActDeact[i])
        nr_UESpec_TCI_StateInd_PDSCH->T[i / 8] = nr_UESpec_TCI_StateInd_PDSCH->T[i / 8] | (1 << (i % 8));
    }

    mac_ce_size = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    //Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)nr_UESpec_TCI_StateInd_PDSCH, mac_ce_size);
    //incrementing the mac pdu pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
    //freeing the allocated memory
    free(nr_UESpec_TCI_StateInd_PDSCH);
  }

  //TS38.321 Sec 6.1.3.13 Aperiodic CSI Trigger State Subselection MAC CE
  if (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.is_scheduled) {
    //Computing the number of octects to be allocated for Flexible array member
    //of MAC CE structure
    uint8_t num_octects = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.highestTriggerStateSelected) / 8 + 1; //Calculating the number of octects for allocating the memory
    //filling the subheader
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->F = 0;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->LCID = DL_SCH_LCID_APERIODIC_CSI_TRI_STATE_SUBSEL;
    ((NR_MAC_SUBHEADER_SHORT *) mac_pdu_ptr)->L = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    last_size = 2;
    //Incrementing the PDU pointer
    mac_pdu_ptr += last_size;
    //allocating memory for CE structure
    NR_TCI_PDSCH_APERIODIC_CSI *nr_Aperiodic_CSI_Trigger = (NR_TCI_PDSCH_APERIODIC_CSI *)malloc(sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //initializing to zero
    memset((void *)nr_Aperiodic_CSI_Trigger, 0, sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t));
    //filling the CE Structure
    nr_Aperiodic_CSI_Trigger->BWP_Id = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.bwpId) & 0x3; //extracting LSB 2 bits
    nr_Aperiodic_CSI_Trigger->ServingCellId = (ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.servingCellId) & 0x1F; //extracting LSB 5 bits
    nr_Aperiodic_CSI_Trigger->R = 0;

    for(i = 0; i < (num_octects * 8); i++) {
      if(ue_sched_ctl->UE_mac_ce_ctrl.aperi_CSI_trigger.triggerStateSelection[i])
        nr_Aperiodic_CSI_Trigger->T[i / 8] = nr_Aperiodic_CSI_Trigger->T[i / 8] | (1 << (i % 8));
    }

    mac_ce_size = sizeof(NR_TCI_PDSCH_APERIODIC_CSI) + num_octects * sizeof(uint8_t);
    // Copying  bytes for MAC CEs to the mac pdu pointer
    memcpy((void *) mac_pdu_ptr, (void *)nr_Aperiodic_CSI_Trigger, mac_ce_size);
    //incrementing the mac pdu pointer
    mac_pdu_ptr += (unsigned char) mac_ce_size;
    //freeing the allocated memory
    free(nr_Aperiodic_CSI_Trigger);
  }

  if (ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.is_scheduled) {
    ((NR_MAC_SUBHEADER_FIXED *) mac_pdu_ptr)->R = 0;
    ((NR_MAC_SUBHEADER_FIXED *) mac_pdu_ptr)->LCID = DL_SCH_LCID_SP_ZP_CSI_RS_RES_SET_ACT;
    mac_pdu_ptr++;
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->A_D = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.act_deact;
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->CELLID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.serv_cell_id & 0x1F; //5 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->BWPID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.bwpid & 0x3; //2 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->CSIRS_RSC_ID = ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.rsc_id & 0xF; //4 bits
    ((NR_MAC_CE_SP_ZP_CSI_RS_RES_SET *) mac_pdu_ptr)->R = 0;
    LOG_D(NR_MAC, "NR MAC CE of ZP CSIRS Serv cell ID = %d BWPID= %d Rsc set ID = %d\n", ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.serv_cell_id, ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.bwpid,
          ue_sched_ctl->UE_mac_ce_ctrl.sp_zp_csi_rs.rsc_id);
    mac_ce_size = sizeof(NR_MAC_CE_SP_ZP_CSI_RS_RES_SET);
    mac_pdu_ptr += (unsigned char) mac_ce_size;
  }

  if (ue_sched_ctl->UE_mac_ce_ctrl.csi_im.is_scheduled) {
    mac_pdu_ptr->R = 0;
    mac_pdu_ptr->LCID = DL_SCH_LCID_SP_CSI_RS_CSI_IM_RES_SET_ACT;
    mac_pdu_ptr++;
    CSI_RS_CSI_IM_ACT_DEACT_MAC_CE csi_rs_im_act_deact_ce;
    csi_rs_im_act_deact_ce.A_D = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.act_deact;
    csi_rs_im_act_deact_ce.SCID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.serv_cellid & 0x3F;//gNB_PHY -> ssb_pdu.ssb_pdu_rel15.PhysCellId;
    csi_rs_im_act_deact_ce.BWP_ID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.bwp_id;
    csi_rs_im_act_deact_ce.R1 = 0;
    csi_rs_im_act_deact_ce.IM = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.im;// IF set CSI IM Rsc id will presesent else CSI IM RSC ID is abscent
    csi_rs_im_act_deact_ce.SP_CSI_RSID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.nzp_csi_rsc_id;

    if ( csi_rs_im_act_deact_ce.IM ) { //is_scheduled if IM is 1 else this field will not present
      csi_rs_im_act_deact_ce.R2 = 0;
      csi_rs_im_act_deact_ce.SP_CSI_IMID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.csi_im_rsc_id;
      mac_ce_size = sizeof ( csi_rs_im_act_deact_ce ) - sizeof ( csi_rs_im_act_deact_ce.TCI_STATE );
    } else {
      mac_ce_size = sizeof ( csi_rs_im_act_deact_ce ) - sizeof ( csi_rs_im_act_deact_ce.TCI_STATE ) - 1;
    }

    memcpy ((void *) mac_pdu_ptr, (void *) & ( csi_rs_im_act_deact_ce), mac_ce_size);
    mac_pdu_ptr += (unsigned char) mac_ce_size;

    if (csi_rs_im_act_deact_ce.A_D ) { //Following IE is_scheduled only if A/D is 1
      mac_ce_size = sizeof ( struct TCI_S);

      for ( i = 0; i < ue_sched_ctl->UE_mac_ce_ctrl.csi_im.nb_tci_resource_set_id; i++) {
        csi_rs_im_act_deact_ce.TCI_STATE.R = 0;
        csi_rs_im_act_deact_ce.TCI_STATE.TCI_STATE_ID = ue_sched_ctl->UE_mac_ce_ctrl.csi_im.tci_state_id [i] & 0x7F;
        memcpy ((void *) mac_pdu_ptr, (void *) & (csi_rs_im_act_deact_ce.TCI_STATE), mac_ce_size);
        mac_pdu_ptr += (unsigned char) mac_ce_size;
      }
    }
  }

  // compute final offset
  offset = ((unsigned char *) mac_pdu_ptr - mac_pdu);
  //printf("Offset %d \n", ((unsigned char *) mac_pdu_ptr - mac_pdu));
  return offset;
}

static void nr_store_dlsch_buffer(module_id_t module_id, frame_t frame, slot_t slot)
{
  UE_iterator(RC.nrmac[module_id]->UE_info.connected_ue_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    sched_ctrl->num_total_bytes = 0;
    sched_ctrl->dl_pdus_total = 0;

    /* loop over all activated logical channels */
    // Note: DL_SCH_LCID_DCCH, DL_SCH_LCID_DCCH1, DL_SCH_LCID_DTCH
    for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
      const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
      const int lcid = c->lcid;
      const uint16_t rnti = UE->rnti;
      LOG_D(NR_MAC, "In %s: UE %x: LCID %d\n", __FUNCTION__, rnti, lcid);
      memset(&sched_ctrl->rlc_status[lcid], 0, sizeof(sched_ctrl->rlc_status[lcid]));
      if (c->suspended)
        continue;
      if (lcid == DL_SCH_LCID_DTCH && nr_timer_is_active(&sched_ctrl->transm_interrupt))
        continue;
      sched_ctrl->rlc_status[lcid] = nr_mac_rlc_status_ind(rnti, frame, lcid);
      const uint32_t hol_wait_us = nr_mac_rlc_hol_wait_us_ind(rnti, lcid);
      // if (lcid == 4) {
      //   LOG_E(NR_MAC,
      //     "In %s: UE %x LCID %d HOL: remaining=%d segmented=%d wait_us=%u (%.3f ms)\n",
      //     __FUNCTION__,
      //     rnti,
      //     lcid,
      //     sched_ctrl->rlc_status[lcid].head_sdu_remaining_size_to_send,
      //     sched_ctrl->rlc_status[lcid].head_sdu_is_segmented,
      //     hol_wait_us,
      //     hol_wait_us / 1000.0f);
      // }

      if (sched_ctrl->rlc_status[lcid].bytes_in_buffer == 0)
        continue;

      sched_ctrl->dl_pdus_total += sched_ctrl->rlc_status[lcid].pdus_in_buffer;
      sched_ctrl->num_total_bytes += sched_ctrl->rlc_status[lcid].bytes_in_buffer;
      LOG_D(MAC,
            "[gNB %d][%4d.%2d] %s%d->DLSCH, RLC status for UE %d: %d bytes in buffer, total DL buffer size = %d bytes, %d total PDU bytes, %s TA command\n",
            module_id,
            frame,
            slot,
            lcid < 4 ? "DCCH":"DTCH",
            lcid,
            UE->rnti,
            sched_ctrl->rlc_status[lcid].bytes_in_buffer,
            sched_ctrl->num_total_bytes,
            sched_ctrl->dl_pdus_total,
            sched_ctrl->ta_apply ? "send":"do not send");
      // LOG_I(MAC, "[gNB %d] [%d | %2d] UE %d Q = %d current_rbs = %d current_bytes = %d thput = %.2f \n", module_id, frame, slot, UE->rnti, sched_ctrl->num_total_bytes, UE->mac_stats.dl.current_rbs, UE->mac_stats.dl.current_bytes, UE->dl_thr_ue);
      // FILE *stats_file = fopen("ue_stats.csv", "a");
      // if (stats_file != NULL) {
      //   fprintf(stats_file, "%d,%d,%d,%.2f,%d,%d\n", 
      //           frame, slot, UE->rnti, 
      //           UE->dl_thr_ue, 
      //           UE->mac_stats.dl.current_rbs,
      //           sched_ctrl->num_total_bytes);
      //   fclose(stats_file);
      // }
    }
  }
}

void finish_nr_dl_harq(NR_UE_sched_ctrl_t *sched_ctrl, int harq_pid)
{
  NR_UE_harq_t *harq = &sched_ctrl->harq_processes[harq_pid];

  harq->ndi ^= 1;
  harq->round = 0;

  add_tail_nr_list(&sched_ctrl->available_dl_harq, harq_pid);
}

void abort_nr_dl_harq(NR_UE_info_t* UE, int8_t harq_pid)
{
  /* already mutex protected through handle_dl_harq() */
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;

  finish_nr_dl_harq(sched_ctrl, harq_pid);
  UE->mac_stats.dl.errors++;
}

bwp_info_t get_pdsch_bwp_start_size(gNB_MAC_INST *nr_mac, NR_UE_info_t *UE)
{
  bwp_info_t bwp_info;
  if (!UE) {
    bwp_info.bwpStart = nr_mac->cset0_bwp_start;
    bwp_info.bwpSize = nr_mac->cset0_bwp_size;
    return bwp_info;
  }
  NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  // 3GPP TS 38.214 Section 5.1.2.2 Resource allocation in frequency domain
  // For a PDSCH scheduled with a DCI format 1_0 in any type of PDCCH common search space, regardless of which bandwidth part is the
  // active bandwidth part, RB numbering starts from the lowest RB of the CORESET in which the DCI was received; otherwise RB
  // numbering starts from the lowest RB in the determined downlink bandwidth part.
  //
  // 3GPP TS 38.214 Section 5.1.2.2.2 Downlink resource allocation type 1
  // In downlink resource allocation of type 1, the resource block assignment information indicates to a scheduled UE a set of
  // contiguously allocated non-interleaved or interleaved virtual resource blocks within the active bandwidth part of size   PRBs
  // except for the case when DCI format 1_0 is decoded in any common search space in which case the size of CORESET 0 shall be
  // used if CORESET 0 is configured for the cell and the size of initial DL bandwidth part shall be used if CORESET 0 is not
  // configured for the cell.
  if (dl_bwp->dci_format == NR_DL_DCI_FORMAT_1_0 && sched_ctrl->search_space->searchSpaceType
      && sched_ctrl->search_space->searchSpaceType->present == NR_SearchSpace__searchSpaceType_PR_common) {
    if (sched_ctrl->coreset->controlResourceSetId == 0) {
      bwp_info.bwpStart = nr_mac->cset0_bwp_start;
    } else {
      bwp_info.bwpStart = dl_bwp->BWPStart + sched_ctrl->sched_pdcch.rb_start;
    }
    if (nr_mac->cset0_bwp_size > 0) {
      bwp_info.bwpSize = min(dl_bwp->BWPSize - bwp_info.bwpStart, nr_mac->cset0_bwp_size);
    } else {
      bwp_info.bwpSize = min(dl_bwp->BWPSize - bwp_info.bwpStart, UE->sc_info.initial_dl_BWPSize);
    }
  } else {
    bwp_info.bwpSize = dl_bwp->BWPSize;
    bwp_info.bwpStart = dl_bwp->BWPStart;
  }
  return bwp_info;
}

static bool allocate_dl_retransmission(module_id_t module_id,
                                       frame_t frame,
                                       slot_t slot,
                                       int *n_rb_sched,
                                       NR_UE_info_t *UE,
                                       int beam_idx,
                                       int current_harq_pid)
{

  int CC_id = 0;
  gNB_MAC_INST *nr_mac = RC.nrmac[module_id];
  const NR_ServingCellConfigCommon_t *scc = nr_mac->common_channels->ServingCellConfigCommon;
  NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
  NR_UE_DL_BWP_t *dl_bwp = &UE->current_DL_BWP;
  NR_UE_UL_BWP_t *ul_bwp = &UE->current_UL_BWP;
  NR_sched_pdsch_t *retInfo = &sched_ctrl->harq_processes[current_harq_pid].sched_pdsch;
  NR_sched_pdsch_t *curInfo = &sched_ctrl->sched_pdsch;

  // If the RI changed between current rtx and a previous transmission
  // we need to verify if it is not decreased
  // othwise it wouldn't be possible to transmit the same TBS
  int layers = (curInfo->nrOfLayers < retInfo->nrOfLayers) ? curInfo->nrOfLayers : retInfo->nrOfLayers;
  int pm_index = (curInfo->nrOfLayers < retInfo->nrOfLayers) ? curInfo->pm_index : retInfo->pm_index;

  const int coresetid = sched_ctrl->coreset->controlResourceSetId;
  const int tda = get_dl_tda(nr_mac, slot);
  AssertFatal(tda >= 0,"Unable to find PDSCH time domain allocation in list\n");

  /* Check first whether the old TDA can be reused
  * this helps allocate retransmission when TDA changes (e.g. new nrOfSymbols > old nrOfSymbols) */
  NR_tda_info_t temp_tda = get_dl_tda_info(dl_bwp,
                                           sched_ctrl->search_space->searchSpaceType->present,
                                           tda,
                                           scc->dmrs_TypeA_Position,
                                           1,
                                           TYPE_C_RNTI_,
                                           coresetid,
                                           false);
  if (!temp_tda.valid_tda)
    return false;

  bool reuse_old_tda = (retInfo->tda_info.startSymbolIndex == temp_tda.startSymbolIndex) && (retInfo->tda_info.nrOfSymbols <= temp_tda.nrOfSymbols);
  LOG_D(NR_MAC, "[UE %x] %s old TDA, %s number of layers\n",
        UE->rnti,
        reuse_old_tda ? "reuse" : "do not reuse",
        layers == retInfo->nrOfLayers ? "same" : "different");

  uint16_t *rballoc_mask = nr_mac->common_channels[CC_id].vrb_map[beam_idx];

  bwp_info_t bwp_info = get_pdsch_bwp_start_size(nr_mac, UE);
  int rbStart = bwp_info.bwpStart;
  int rbStop = bwp_info.bwpStart + bwp_info.bwpSize - 1;
  int rbSize = 0;

  if (reuse_old_tda && layers == retInfo->nrOfLayers) {
    /* Check that there are enough resources for retransmission */
    while (rbSize < retInfo->rbSize) {
      rbStart += rbSize; /* last iteration rbSize was not enough, skip it */
      rbSize = 0;

      const uint16_t slbitmap = SL_to_bitmap(retInfo->tda_info.startSymbolIndex, retInfo->tda_info.nrOfSymbols);
      while (rbStart < rbStop && (rballoc_mask[rbStart] & slbitmap))
        rbStart++;

      if (rbStart >= rbStop) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not allocate DL retransmission: no resources\n", UE->rnti, frame, slot);
        return false;
      }

      while (rbStart + rbSize <= rbStop && !(rballoc_mask[rbStart + rbSize] & slbitmap) && rbSize < retInfo->rbSize)
        rbSize++;
    }
  } else {
    /* the retransmission will use a different time domain allocation, check
     * that we have enough resources */
    NR_pdsch_dmrs_t temp_dmrs = get_dl_dmrs_params(scc, dl_bwp, &temp_tda, layers);

    const uint16_t slbitmap = SL_to_bitmap(temp_tda.startSymbolIndex, temp_tda.nrOfSymbols);
    while (rbStart < rbStop && (rballoc_mask[rbStart] & slbitmap))
      rbStart++;

    while (rbStart + rbSize <= rbStop && !(rballoc_mask[rbStart + rbSize] & slbitmap))
      rbSize++;

    uint32_t new_tbs;
    uint16_t new_rbSize;
    bool success = nr_find_nb_rb(retInfo->Qm,
                                 retInfo->R,
                                 1, // no transform precoding for DL
                                 layers,
                                 temp_tda.nrOfSymbols,
                                 temp_dmrs.N_PRB_DMRS * temp_dmrs.N_DMRS_SLOT,
                                 retInfo->tb_size,
                                 1, /* minimum of 1RB: need to find exact TBS, don't preclude any number */
                                 rbSize,
                                 &new_tbs,
                                 &new_rbSize);

    if (!success || new_tbs != retInfo->tb_size) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] allocation of DL retransmission failed: new TBS %d of new TDA does not match old TBS %d\n",
            UE->rnti,
            frame,
            slot,
            new_tbs,
            retInfo->tb_size);
      return false; /* the maximum TBsize we might have is smaller than what we need */
    }

    /* we can allocate it. Overwrite the time_domain_allocation, the number
     * of RBs, and the new TB size. The rest is done below */
    retInfo->tb_size = new_tbs;
    retInfo->rbSize = new_rbSize;
    retInfo->time_domain_allocation = tda;
    retInfo->nrOfLayers = layers;
    retInfo->pm_index = pm_index;
    retInfo->dmrs_parms = temp_dmrs;
    retInfo->tda_info = temp_tda;
  }

  /* Find a free CCE */
  int CCEIndex = get_cce_index(nr_mac,
                               CC_id,
                               slot,
                               UE->rnti,
                               &sched_ctrl->aggregation_level,
                               beam_idx,
                               sched_ctrl->search_space,
                               sched_ctrl->coreset,
                               &sched_ctrl->sched_pdcch,
                               false,
                               sched_ctrl->pdcch_cl_adjust);
  if (CCEIndex<0) {
    LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find free CCE for DL DCI retransmission\n", UE->rnti, frame, slot);
    return false;
  }

  /* Find PUCCH occasion: if it fails, undo CCE allocation (undoing PUCCH
   * allocation after CCE alloc fail would be more complex) */

  int alloc = -1;
  if (!get_FeedbackDisabled(UE->sc_info.downlinkHARQ_FeedbackDisabled_r17, current_harq_pid)) {
    int r_pucch = nr_get_pucch_resource(sched_ctrl->coreset, ul_bwp->pucch_Config, CCEIndex);
    alloc = nr_acknack_scheduling(nr_mac, UE, frame, slot, UE->UE_beam_index, r_pucch, 0);
    if (alloc < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find PUCCH for DL DCI retransmission\n", UE->rnti, frame, slot);
      return false;
    }
  }

  sched_ctrl->cce_index = CCEIndex;
  fill_pdcch_vrb_map(nr_mac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, beam_idx);
  /* just reuse from previous scheduling opportunity, set new start RB */
  sched_ctrl->sched_pdsch = *retInfo;
  sched_ctrl->sched_pdsch.rbStart = rbStart - bwp_info.bwpStart;
  sched_ctrl->sched_pdsch.pucch_allocation = alloc;
  sched_ctrl->sched_pdsch.bwp_info = bwp_info;
  /* retransmissions: directly allocate */
  *n_rb_sched -= sched_ctrl->sched_pdsch.rbSize;

  for (int rb = rbStart; rb < sched_ctrl->sched_pdsch.rbSize; rb++)
    rballoc_mask[rb] |= SL_to_bitmap(retInfo->tda_info.startSymbolIndex, retInfo->tda_info.nrOfSymbols);

  return true;
}

static uint32_t pf_tbs[3][29]; // pre-computed, approximate TBS values for PF coefficient
typedef struct UEsched_s {
  float coef;
  NR_UE_info_t * UE;
} UEsched_t;

static int comparator(const void *p, const void *q) {
  return ((UEsched_t*)p)->coef < ((UEsched_t*)q)->coef;
}

static void scheduler_custom(module_id_t module_id,
                  frame_t frame,
                  slot_t slot,
                  NR_UE_info_t **UE_list,
                  int max_num_ue,
                  int num_beams,
                  int n_rb_sched[num_beams])
{
  gNB_MAC_INST *mac = RC.nrmac[module_id];
  NR_ServingCellConfigCommon_t *scc=mac->common_channels[0].ServingCellConfigCommon;
  // UEs that could be scheduled
  UEsched_t UE_sched[MAX_MOBILES_PER_GNB + 1] = {0};
  int remainUEs[num_beams];
  for (int i = 0; i < num_beams; i++)
    remainUEs[i] = max_num_ue;
  int curUE = 0;
  int CC_id = 0;
  int slots_per_frame = mac->frame_structure.numb_slots_frame;

  /* Loop UE_info->list to check retransmission */
  UE_iterator(UE_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *current_BWP = &UE->current_DL_BWP;
    const uint64_t rlc_dequeued_bytes = get_rlc_dequeued_delta_bytes(UE);
    bool got_new_target_thput = false;

    /* Log DSCP from GTP-U tunnel for all DRBs of this UE */
    // for (int drb_id = 1; drb_id <= 32; drb_id++) {
    for (int drb_id = 1; drb_id <= 1; drb_id++) { // TODO fix: we should loop over the actual DRBs of the UE, but for now we just check DRB 1
      uint8_t dscp = gtpv1u_get_dscp(module_id, UE->uid+1, drb_id); // CHECK if UE->uid+1 is the correct way to get the UE id for GTP-U, as UE ids start from 0 but GTP-U tunnel ids might start from 1
      // uint8_t dscp = gtpv1u_get_dscp(0, 1, drb_id); // TODO fix: we should get the correct UE id, but for now we just want to check that we can read the DSCP value from the GTP-U tunnel
      if (dscp > 0) {
        bool is_i_frame = false;
        bool is_last_packet = false;
        uint8_t size_level = 0;
        float size_desc = 0.0f;
        decode_dscp(dscp, &is_i_frame, &is_last_packet, &size_level, &size_desc);
        const uint64_t now_us = get_monotonic_time_us();

        if ((unsigned int)UE->uid < MAX_MOBILES_PER_GNB && drb_id >= 0 && drb_id <= DSCP_TRACK_MAX_DRB) {
          dscp_transition_tracker_t *track = &dscp_tracker[UE->uid][drb_id];
          const uint8_t prev_size_level = track->last_dscp & 0x0F;
          const bool dscp_changed = !track->valid || (track->last_dscp != dscp);
          const bool size_level_changed = !track->valid || (prev_size_level != size_level);
          const bool new_frame_start =
              track->valid && (track->last_is_last_packet && dscp_changed) || size_level_changed;
          const bool rlc_queue_empty = (sched_ctrl->num_total_bytes == 0);

          bool reset_previous_frame = false;
          if (new_frame_start && track->valid) {
            const bool reached_previous_frame_size =
                track->frame_size_target_bytes > 0 && track->frame_delta_bytes >= track->frame_size_target_bytes;
            reset_previous_frame = track->end_packet_seen && (rlc_queue_empty || reached_previous_frame_size);
          }

          const bool adopt_new_frame = new_frame_start && (!track->valid || reset_previous_frame);

          if (!track->pdu_set_active) {
            track->pdu_set_start_us = now_us;
            track->pdu_set_hol_us = 0;
            track->pdu_set_active = true;
          }

          if (new_frame_start) {
            track->new_frame_starts++;
            LOG_I(NR_MAC,
              "%4d.%2d [dpp] UE %04x DRB %d NEW_FRAME_START detected (prev_size_level=%u, size_level=%u, count=%u, prev_frame_delta=%llu B, prev_reset=%s, adopt_new=%s)\n",
                  frame,
                  slot,
                  UE->rnti,
                  drb_id,
                  prev_size_level,
                  size_level,
              track->new_frame_starts,
              (unsigned long long)track->frame_delta_bytes,
              reset_previous_frame ? "yes" : "no",
              adopt_new_frame ? "yes" : "no");

            if (adopt_new_frame) {
              track->frame_delta_bytes = 0;
              track->frame_size_target_bytes = 0;
              track->end_packet_seen = false;
            }
          }

          const uint64_t decoded_size_target_bytes = (uint64_t)size_desc;
          if (track->frame_size_target_bytes == 0 || adopt_new_frame)
            track->frame_size_target_bytes = decoded_size_target_bytes;
          else if (decoded_size_target_bytes > track->frame_size_target_bytes)
            track->frame_size_target_bytes = decoded_size_target_bytes;

          track->frame_delta_bytes += rlc_dequeued_bytes;

          if (new_frame_start && !adopt_new_frame && !is_last_packet)
            track->end_packet_seen = false;

          if (is_last_packet)
            track->end_packet_seen = true;

          const bool reached_frame_size =
              track->frame_size_target_bytes > 0 && track->frame_delta_bytes >= track->frame_size_target_bytes;
          const bool reset_frame_delta = track->end_packet_seen && (rlc_queue_empty || reached_frame_size);

          if (track->pdu_set_active)
            track->pdu_set_hol_us = now_us > track->pdu_set_start_us ? now_us - track->pdu_set_start_us : 0;
          else
            track->pdu_set_hol_us = 0;

          track->valid = true;
          track->last_dscp = dscp;
          track->last_is_last_packet = is_last_packet;

          // LOG_E(NR_MAC,
          //       "%4d.%2d [dpp] UE %04x DRB %d FRAME_ACC delta=%llu B, target=%llu B, end_seen=%s, rlc_empty=%s, reset=%s, pset_hol=%.3f ms\n",
          //       frame,
          //       slot,
          //       UE->rnti,
          //       drb_id,
          //       (unsigned long long)track->frame_delta_bytes,
          //       (unsigned long long)track->frame_size_target_bytes,
          //       track->end_packet_seen ? "yes" : "no",
          //       rlc_queue_empty ? "yes" : "no",
          //       reset_frame_delta ? "yes" : "no",
          //       track->pdu_set_hol_us / 1000.0f);

          if (reset_frame_delta) {
            if (track->pdu_set_hol_us > 0.0)
              log_pset_hol_before_reset(frame,
                                        slot,
                                        "custom",
                                        UE->rnti,
                                        drb_id,
                                        is_i_frame,
                                        track->frame_delta_bytes,
                                        track->pdu_set_hol_us);
            track->frame_delta_bytes = 0;
            track->frame_size_target_bytes = 0;
            track->end_packet_seen = false;
            track->pdu_set_start_us = 0;
            track->pdu_set_hol_us = 0;
            track->pdu_set_active = false;
          }
        }
        dscp_transition_tracker_t *track = &dscp_tracker[UE->uid][drb_id];
        const float new_target_thput = size_desc * 8.0f / (psdb - 0.001 - track->pdu_set_hol_us/1000000.0 - 0.00057); // psdb - t_n6-rlc - t_hol - t_procUE 
        if (UE->target_thput != new_target_thput){
          reset_virtual_queue(UE);
          // LOG_E(NR_MAC, "Resetting virtual queue for UE %04x DRB %d due to target throughput change: old=%.2f Mbps, new=%.2f Mbps\n",
          //       UE->rnti,
          //       drb_id,
          //       UE->target_thput / 1e6f,
          //       new_target_thput / 1e6f);
        }
        UE->target_thput = new_target_thput;
        got_new_target_thput = true;
        LOG_I(NR_MAC,
            "%4d.%2d [dpp] UE %04x DRB %d DSCP=%u -> type=%s, end=%s, size_level=%u (%.0f B), target_thput = %.2f Mbps, rlc_step_delta=%llu B\n",
              frame,
              slot,
              UE->rnti,
              drb_id,
              dscp,
              is_i_frame ? "I" : "non-I",
              is_last_packet ? "yes" : "no",
              size_level,
              size_desc,
              UE->target_thput / 1e6f,
              (unsigned long long)rlc_dequeued_bytes);
      }
    }

    if (!got_new_target_thput && UE->mac_stats.dl.current_bytes == 0)
      UE->target_thput = 0;

    if (sched_ctrl->ul_failure){
      // // // LOG_I(MAC, "SKIPPED UE%d due to UL failure\n", UE->uid);
      continue;
    }

    const NR_mac_dir_stats_t *stats = &UE->mac_stats.dl;
    NR_sched_pdsch_t *sched_pdsch = &sched_ctrl->sched_pdsch;
    /* get the PID of a HARQ process awaiting retrnasmission, or -1 otherwise */
    sched_pdsch->dl_harq_pid = sched_ctrl->retrans_dl_harq.head;

    UE->dl_thr_ue = UE->mac_stats.dl.current_bytes*8/SLOT_TIME; // FIXME check
    // // // LOG_I(MAC, "UE%d [%4d.%2d] DL throughput = %f Mbps\n", UE->uid, frame, slot, UE->dl_thr_ue/1e6);
    update_virtual_queue(UE);

    int total_rem_ues = 0;
    for (int i = 0; i < num_beams; i++)
      total_rem_ues += remainUEs[i];
    if (total_rem_ues == 0){
      // // // LOG_I(MAC, "SKIPPED UE%d due to total_rem_ues == 0\n", UE->uid);
      continue;
    }

    /* retransmission */
    if (sched_pdsch->dl_harq_pid >= 0) {
      NR_beam_alloc_t beam = beam_allocation_procedure(&mac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame);
      bool sch_ret = beam.idx >= 0;
      /* Allocate retransmission */
      if (sch_ret)
        sch_ret = allocate_dl_retransmission(module_id, frame, slot, &n_rb_sched[beam.idx], UE, beam.idx, sched_pdsch->dl_harq_pid);
      if (!sch_ret) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] DL retransmission could not be allocated\n", UE->rnti, frame, slot);
        reset_beam_status(&mac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
        // // // LOG_I(MAC, "SKIPPED UE%d due to DL retransmission could not be allocated\n", UE->uid);
        continue;
      }
      /* reduce max_num_ue once we are sure UE can be allocated, i.e., has CCE */
      remainUEs[beam.idx]--;

    } else {
      /* skip this UE if there are no free HARQ processes. This can happen e.g.
       * if the UE disconnected in L2sim, in which case the gNB is not notified
       * (this can be considered a design flaw) */
      if (sched_ctrl->available_dl_harq.head < 0) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] UE has no free DL HARQ process, skipping\n",
              UE->rnti,
              frame,
              slot);
        // // // LOG_I(MAC, "SKIPPED UE%d due to UE has no free DL HARQ process\n", UE->uid);
        continue;
      }

      /* Check DL buffer and skip this UE if no bytes and no TA necessary */
      if (sched_ctrl->num_total_bytes == 0 && frame != (sched_ctrl->ta_frame + 100) % 1024){
        // // // LOG_I(MAC, "SKIPPED UE%d after check buffer due to no bytes and no TA necessary. Buffer = %d, TA_frame: %d != %d\n", UE->uid, sched_ctrl->num_total_bytes, frame, (sched_ctrl->ta_frame + 100) % 1024);
        continue;
      }

      /* Calculate coeff */
      const NR_bler_options_t *bo = &mac->dl_bler;
      const int max_mcs_table = current_BWP->mcsTableIdx == 1 ? 27 : 28;
      const int max_mcs = min(sched_ctrl->dl_max_mcs, max_mcs_table);
      if (bo->harq_round_max == 1) {
        sched_pdsch->mcs = min(bo->max_mcs, max_mcs);
        sched_ctrl->dl_bler_stats.mcs = sched_pdsch->mcs;
      } else
        sched_pdsch->mcs = get_mcs_from_bler(bo, stats, &sched_ctrl->dl_bler_stats, max_mcs, frame);
      sched_pdsch->nrOfLayers = get_dl_nrOfLayers(sched_ctrl, current_BWP->dci_format);
      sched_pdsch->pm_index =
          get_pm_index(mac, UE, current_BWP->dci_format, sched_pdsch->nrOfLayers, mac->radio_config.pdsch_AntennaPorts.XP);
        const uint8_t coeff_mcs = get_mcs_for_coeff_tbs(UE, sched_pdsch->mcs);
        const uint8_t Qm = nr_get_Qm_dl(coeff_mcs, current_BWP->mcsTableIdx);
        const uint16_t R = nr_get_code_rate_dl(coeff_mcs, current_BWP->mcsTableIdx);
      uint32_t tbs = nr_compute_tbs(Qm,
                                    R,
                                    1, /* rbSize */
                                    10, /* hypothetical number of slots */
                                    0, /* N_PRB_DMRS * N_DMRS_SLOT */
                                    0 /* N_PRB_oh, 0 for initialBWP */,
                                    0 /* tb_scaling */,
                                    sched_pdsch->nrOfLayers) >> 3;

      /* Calculate coeff_ue (K) */
      // Scheduling parameters: v, wq, wg
      uint64_t v = 0;
      uint32_t wq = 1;
      uint32_t wg = 1;

      // Read scheduling parameters from E2 control if configured, otherwise use default values
      if (DYNAMIC_CONFIGURATION) {
        // v = sched_ctrl->e2_mac_ctrl.v;
        // wq = sched_ctrl->e2_mac_ctrl.wq;
        // wg = sched_ctrl->e2_mac_ctrl.wg;
        if (sched_ctrl->e2_mac_ctrl.v >= 0 && sched_ctrl->e2_mac_ctrl.v <= 3)
          v = V_DICT[sched_ctrl->e2_mac_ctrl.v];
        else
          v = sched_ctrl->e2_mac_ctrl.v*20; // default
        
        if (sched_ctrl->e2_mac_ctrl.wq >= 0 && sched_ctrl->e2_mac_ctrl.wq <= 1)
          wq = WQ_DICT[sched_ctrl->e2_mac_ctrl.wq];
        else
          wq = 1; // default

        if (sched_ctrl->e2_mac_ctrl.wg >= 0 && sched_ctrl->e2_mac_ctrl.wg <= 1)
          wg = WG_DICT[sched_ctrl->e2_mac_ctrl.wg];
        else
          wg = 1; // default
        
        LOG_I(MAC, "[E2 CONTROL] UE%d usando valores E2: v=%lu, wq=%u, wg=%u\n", 
              UE->uid, v, wq, wg);
      } else {
        LOG_D(MAC, "[DEFAULT] UE%d usando valores por defecto: v=%lu, wq=%u, wg=%u\n", 
              UE->uid, v, wq, wg);
      }
      
      float coeff_ue = (float)((wq*1e3*sched_ctrl->num_total_bytes*8 + wg*sched_ctrl->virtual_thput_queue[4]/0.0005)/1e6 * tbs * 8/1e6); // TODO extend to more LCs
      qos_set_coeff_snapshot(UE, coeff_ue);
      LOG_I(MAC, "coef_UE%d = %f\n", UE->uid, coeff_ue);
      LOG_I(MAC, "sched_ctrl->virtual_thput_queue[4] = %ld, tbs=%d (mcs_for_coeff=%u, mcs_current=%d)\n",
        sched_ctrl->virtual_thput_queue[4],
        tbs,
        coeff_mcs,
        sched_pdsch->mcs);

      LOG_D(NR_MAC, "[UE %04x][%4d, %2d] tbs %d, coeff_ue %f\n",
            UE->uid,
            frame,
            slot,
            tbs,
            coeff_ue);
      if (v > coeff_ue && !sched_ctrl->SR && (sched_ctrl->rlc_status[0].bytes_in_buffer + sched_ctrl->rlc_status[1].bytes_in_buffer + sched_ctrl->rlc_status[2].bytes_in_buffer) == 0) { // TODO buffer > 0 o algo que no pierda los SR?
        LOG_I(MAC, "SKIPPED UE%d:  v=%lu > coefficient %.2f\n",
              UE->uid, v, coeff_ue);
        continue;  // Skip UE
      } else {
        LOG_I(MAC, "SCHEDULED UE%d:  v=%lu <= coefficient %.2f\n",
              UE->uid, v, coeff_ue);
      }

      /* Create UE_sched list for UEs eligible for new transmission*/
      UE_sched[curUE].coef=coeff_ue;
      UE_sched[curUE].UE=UE;
      curUE++;
    }
  }

  qsort(UE_sched, sizeofArray(UE_sched), sizeof(UEsched_t), comparator);
  UEsched_t *iterator = UE_sched;

  // LOG_I(MAC, "- UE%d\n", iterator->UE ? iterator->UE->uid : -1);

  const int min_rbSize = 5;

  /* Loop UE_sched to find max coeff and allocate transmission */
  while (iterator->UE != NULL) {

    NR_UE_sched_ctrl_t *sched_ctrl = &iterator->UE->UE_sched_ctrl;
    const uint16_t rnti = iterator->UE->rnti;

    NR_UE_DL_BWP_t *dl_bwp = &iterator->UE->current_DL_BWP;
    NR_UE_UL_BWP_t *ul_bwp = &iterator->UE->current_UL_BWP;

    if (sched_ctrl->available_dl_harq.head < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] UE has no free DL HARQ process, skipping\n",
            iterator->UE->rnti,
            frame,
            slot);
      iterator++;
      continue;
    }

    NR_beam_alloc_t beam = beam_allocation_procedure(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame);

    if (beam.idx < 0) {
      // no available beam
      iterator++;
      continue;
    }
    if (remainUEs[beam.idx] == 0 || n_rb_sched[beam.idx] < min_rbSize) {
      reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    NR_sched_pdsch_t *sched_pdsch = &sched_ctrl->sched_pdsch;
    sched_pdsch->dl_harq_pid = sched_ctrl->available_dl_harq.head;

    /* MCS has been set above */
    sched_pdsch->time_domain_allocation = get_dl_tda(mac, slot);
    AssertFatal(sched_pdsch->time_domain_allocation>=0,"Unable to find PDSCH time domain allocation in list\n");

    const int coresetid = sched_ctrl->coreset->controlResourceSetId;
    sched_pdsch->tda_info = get_dl_tda_info(dl_bwp,
                                            sched_ctrl->search_space->searchSpaceType->present,
                                            sched_pdsch->time_domain_allocation,
                                            scc->dmrs_TypeA_Position,
                                            1,
                                            TYPE_C_RNTI_,
                                            coresetid,
                                            false);
    AssertFatal(sched_pdsch->tda_info.valid_tda, "Invalid TDA from get_dl_tda_info\n");

    NR_tda_info_t *tda_info = &sched_pdsch->tda_info;

    const uint16_t slbitmap = SL_to_bitmap(tda_info->startSymbolIndex, tda_info->nrOfSymbols);

    uint16_t *rballoc_mask = mac->common_channels[CC_id].vrb_map[beam.idx];
    sched_pdsch->bwp_info = get_pdsch_bwp_start_size(mac, iterator->UE);
    int rbStart = 0; // WRT BWP start
    int rbStop = sched_pdsch->bwp_info.bwpSize - 1;
    int bwp_start = sched_pdsch->bwp_info.bwpStart;
    // Freq-demain allocation
    while (rbStart < rbStop && (rballoc_mask[rbStart + bwp_start] & slbitmap))
      rbStart++;

    uint16_t max_rbSize = 1;

    while (rbStart + max_rbSize <= rbStop && !(rballoc_mask[rbStart + max_rbSize + bwp_start] & slbitmap))
      max_rbSize++;

    if (max_rbSize < min_rbSize) {
      LOG_D(NR_MAC,
            "(%d.%d) Cannot schedule RNTI %04x, rbStart %d, rbSize %d, rbStop %d\n",
            frame,
            slot,
            rnti,
            rbStart,
            max_rbSize,
            rbStop);
      iterator++;
      continue;
    }

    int CCEIndex = get_cce_index(mac,
                                 CC_id,
                                 slot,
                                 iterator->UE->rnti,
                                 &sched_ctrl->aggregation_level,
                                 beam.idx,
                                 sched_ctrl->search_space,
                                 sched_ctrl->coreset,
                                 &sched_ctrl->sched_pdcch,
                                 false,
                                 sched_ctrl->pdcch_cl_adjust);
    if (CCEIndex < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find free CCE for DL DCI\n", rnti, frame, slot);
      reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    /* Find PUCCH occasion: if it fails, undo CCE allocation (undoing PUCCH
    * allocation after CCE alloc fail would be more complex) */

    int alloc = -1;
    if (!get_FeedbackDisabled(iterator->UE->sc_info.downlinkHARQ_FeedbackDisabled_r17, sched_pdsch->dl_harq_pid)) {
      int r_pucch = nr_get_pucch_resource(sched_ctrl->coreset, ul_bwp->pucch_Config, CCEIndex);
      alloc = nr_acknack_scheduling(mac, iterator->UE, frame, slot, iterator->UE->UE_beam_index, r_pucch, 0);
      if (alloc < 0) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find PUCCH for DL DCI\n", rnti, frame, slot);
        reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
        iterator++;
        continue;
      }
    }

    sched_ctrl->cce_index = CCEIndex;
    fill_pdcch_vrb_map(mac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, beam.idx);

    sched_pdsch->dmrs_parms = get_dl_dmrs_params(scc, dl_bwp, tda_info, sched_pdsch->nrOfLayers);
    sched_pdsch->Qm = nr_get_Qm_dl(sched_pdsch->mcs, dl_bwp->mcsTableIdx);
    sched_pdsch->R = nr_get_code_rate_dl(sched_pdsch->mcs, dl_bwp->mcsTableIdx);
    sched_pdsch->pucch_allocation = alloc;
    uint32_t TBS = 0;
    uint16_t rbSize;
    // Fix me: currently, the RLC does not give us the total number of PDUs
    // awaiting. Therefore, for the time being, we put a fixed overhead of 12
    // (for 4 PDUs) and optionally + 2 for TA. Once RLC gives the number of
    // PDUs, we replace with 3 * numPDUs
    const int oh = 3 * 4 + 2 * (frame == (sched_ctrl->ta_frame + 100) % 1024);
    //const int oh = 3 * sched_ctrl->dl_pdus_total + 2 * (frame == (sched_ctrl->ta_frame + 100) % 1024);
    nr_find_nb_rb(sched_pdsch->Qm,
                  sched_pdsch->R,
                  1, // no transform precoding for DL
                  sched_pdsch->nrOfLayers,
                  tda_info->nrOfSymbols,
                  sched_pdsch->dmrs_parms.N_PRB_DMRS * sched_pdsch->dmrs_parms.N_DMRS_SLOT,
                  sched_ctrl->num_total_bytes + oh,
                  min_rbSize,
                  max_rbSize,
                  &TBS,
                  &rbSize);
    sched_pdsch->rbSize = rbSize;
    sched_pdsch->rbStart = rbStart;
    sched_pdsch->tb_size = TBS;
    /* transmissions: directly allocate */
    n_rb_sched[beam.idx] -= sched_pdsch->rbSize;

    for (int rb = bwp_start; rb < sched_pdsch->rbSize; rb++)
      rballoc_mask[rb + sched_pdsch->rbStart] |= slbitmap;

    remainUEs[beam.idx]--;
    iterator++;
  }
}

static void pf_dl(module_id_t module_id,
                  frame_t frame,
                  slot_t slot,
                  NR_UE_info_t **UE_list,
                  int max_num_ue,
                  int num_beams,
                  int n_rb_sched[num_beams])
{
  gNB_MAC_INST *mac = RC.nrmac[module_id];
  NR_ServingCellConfigCommon_t *scc=mac->common_channels[0].ServingCellConfigCommon;
  // UEs that could be scheduled
  UEsched_t UE_sched[MAX_MOBILES_PER_GNB + 1] = {0};
  int remainUEs[num_beams];
  for (int i = 0; i < num_beams; i++)
    remainUEs[i] = max_num_ue;
  int curUE = 0;
  int CC_id = 0;
  int slots_per_frame = mac->frame_structure.numb_slots_frame;

  /* Loop UE_info->list to check retransmission */
  UE_iterator(UE_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *current_BWP = &UE->current_DL_BWP;
    const uint64_t rlc_dequeued_bytes = get_rlc_dequeued_delta_bytes(UE);
    bool got_new_target_thput = false;

    /* Log DSCP from GTP-U tunnel for all DRBs of this UE */
    // for (int drb_id = 1; drb_id <= 32; drb_id++) {
    for (int drb_id = 1; drb_id <= 1; drb_id++) { // TODO fix: we should loop over the actual DRBs of the UE, but for now we just check DRB 1
      uint8_t dscp = gtpv1u_get_dscp(module_id, UE->uid+1, drb_id); // CHECK if UE->uid+1 is the correct way to get the UE id for GTP-U, as UE ids start from 0 but GTP-U tunnel ids might start from 1
      // uint8_t dscp = gtpv1u_get_dscp(0, 1, drb_id); // TODO fix: we should get the correct UE id, but for now we just want to check that we can read the DSCP value from the GTP-U tunnel
      if (dscp > 0) {
        bool is_i_frame = false;
        bool is_last_packet = false;
        uint8_t size_level = 0;
        float size_desc = 0.0f;
        decode_dscp(dscp, &is_i_frame, &is_last_packet, &size_level, &size_desc);
        const uint64_t now_us = get_monotonic_time_us();

        if ((unsigned int)UE->uid < MAX_MOBILES_PER_GNB && drb_id >= 0 && drb_id <= DSCP_TRACK_MAX_DRB) {
          dscp_transition_tracker_t *track = &dscp_tracker[UE->uid][drb_id];
          const uint8_t prev_size_level = track->last_dscp & 0x0F;
          const bool dscp_changed = !track->valid || (track->last_dscp != dscp);
          const bool size_level_changed = !track->valid || (prev_size_level != size_level);
          const bool new_frame_start =
              track->valid && (track->last_is_last_packet && dscp_changed) || size_level_changed;
          const bool rlc_queue_empty = (sched_ctrl->num_total_bytes == 0);

          bool reset_previous_frame = false;
          if (new_frame_start && track->valid) {
            const bool reached_previous_frame_size =
                track->frame_size_target_bytes > 0 && track->frame_delta_bytes >= track->frame_size_target_bytes;
            reset_previous_frame = track->end_packet_seen && (rlc_queue_empty || reached_previous_frame_size);
          }

          const bool adopt_new_frame = new_frame_start && (!track->valid || reset_previous_frame);

          if (!track->pdu_set_active) {
            track->pdu_set_start_us = now_us;
            track->pdu_set_hol_us = 0;
            track->pdu_set_active = true;
          }

          if (new_frame_start) {
            track->new_frame_starts++;
            LOG_I(NR_MAC,
              "%4d.%2d [pf_dl] UE %04x DRB %d NEW_FRAME_START detected (prev_size_level=%u, size_level=%u, count=%u, prev_frame_delta=%llu B, prev_reset=%s, adopt_new=%s)\n",
                  frame,
                  slot,
                  UE->rnti,
                  drb_id,
                  prev_size_level,
                  size_level,
              track->new_frame_starts,
              (unsigned long long)track->frame_delta_bytes,
              reset_previous_frame ? "yes" : "no",
              adopt_new_frame ? "yes" : "no");

            if (adopt_new_frame) {
              track->frame_delta_bytes = 0;
              track->frame_size_target_bytes = 0;
              track->end_packet_seen = false;
            }
          }

          const uint64_t decoded_size_target_bytes = (uint64_t)size_desc;
          if (track->frame_size_target_bytes == 0 || adopt_new_frame)
            track->frame_size_target_bytes = decoded_size_target_bytes;
          else if (decoded_size_target_bytes > track->frame_size_target_bytes)
            track->frame_size_target_bytes = decoded_size_target_bytes;

          track->frame_delta_bytes += rlc_dequeued_bytes;

          if (new_frame_start && !adopt_new_frame && !is_last_packet)
            track->end_packet_seen = false;

          if (is_last_packet)
            track->end_packet_seen = true;

          const bool reached_frame_size =
              track->frame_size_target_bytes > 0 && track->frame_delta_bytes >= track->frame_size_target_bytes;
          const bool reset_frame_delta = track->end_packet_seen && (rlc_queue_empty || reached_frame_size);

          if (track->pdu_set_active)
            track->pdu_set_hol_us = now_us > track->pdu_set_start_us ? now_us - track->pdu_set_start_us : 0;
          else
            track->pdu_set_hol_us = 0;

          track->valid = true;
          track->last_dscp = dscp;
          track->last_is_last_packet = is_last_packet;

          LOG_I(NR_MAC,
                "%4d.%2d [pf_dl] UE %04x DRB %d FRAME_ACC delta=%llu B, target=%llu B, end_seen=%s, rlc_empty=%s, reset=%s, pset_hol=%.3f ms\n",
                frame,
                slot,
                UE->rnti,
                drb_id,
                (unsigned long long)track->frame_delta_bytes,
                (unsigned long long)track->frame_size_target_bytes,
                track->end_packet_seen ? "yes" : "no",
                rlc_queue_empty ? "yes" : "no",
                reset_frame_delta ? "yes" : "no",
                track->pdu_set_hol_us / 1000.0f);

          if (reset_frame_delta) {
            if (track->pdu_set_hol_us > 0.0)
              log_pset_hol_before_reset(frame,
                                        slot,
                                        "pf",
                                        UE->rnti,
                                        drb_id,
                                        is_i_frame,
                                        track->frame_delta_bytes,
                                        track->pdu_set_hol_us);
            track->frame_delta_bytes = 0;
            track->frame_size_target_bytes = 0;
            track->end_packet_seen = false;
            track->pdu_set_start_us = 0;
            track->pdu_set_hol_us = 0;
            track->pdu_set_active = false;
          }
        }
        dscp_transition_tracker_t *track = &dscp_tracker[UE->uid][drb_id];
        const float new_target_thput = size_desc * 8.0f / (psdb - 0.001 - track->pdu_set_hol_us/1000000.0 - 0.00057); // psdb - t_n6-rlc - t_hol - t_procUE
        if (UE->target_thput != new_target_thput){
          reset_virtual_queue(UE);
          // LOG_E(NR_MAC, "Resetting virtual queue for UE %04x DRB %d due to target throughput change: old=%.2f Mbps, new=%.2f Mbps\n",
          //       UE->rnti,
          //       drb_id,
          //       UE->target_thput / 1e6f,
          //       new_target_thput / 1e6f);
        }
        UE->target_thput = new_target_thput;
        got_new_target_thput = true;

        LOG_I(NR_MAC,
            "%4d.%2d [pf_dl] UE %04x DRB %d DSCP=%u -> type=%s, end=%s, size_level=%u (%.0f B), target_thput = %.2f Mbps, rlc_step_delta=%llu B\n",
              frame,
              slot,
              UE->rnti,
              drb_id,
              dscp,
              is_i_frame ? "I" : "non-I",
              is_last_packet ? "yes" : "no",
              size_level,
              size_desc,
              (size_desc*8 / psdb / 1e6f),
              (unsigned long long)rlc_dequeued_bytes);
      }
    }

    if (!got_new_target_thput && UE->mac_stats.dl.current_bytes == 0)
      UE->target_thput = 0;

    if (sched_ctrl->ul_failure)
      continue;

    const NR_mac_dir_stats_t *stats = &UE->mac_stats.dl;
    NR_sched_pdsch_t *sched_pdsch = &sched_ctrl->sched_pdsch;
    /* get the PID of a HARQ process awaiting retrnasmission, or -1 otherwise */
    sched_pdsch->dl_harq_pid = sched_ctrl->retrans_dl_harq.head;

    /* Calculate Throughput */
    const float a = 0.01f;
    // const float a = 1.0f; // instantaneous throughput
    const uint32_t b = UE->mac_stats.dl.current_bytes;
    UE->dl_thr_ue = (1 - a) * UE->dl_thr_ue + a * b;

    int total_rem_ues = 0;
    for (int i = 0; i < num_beams; i++)
      total_rem_ues += remainUEs[i];
    if (total_rem_ues == 0)
      continue;

    /* retransmission */
    if (sched_pdsch->dl_harq_pid >= 0) {
      NR_beam_alloc_t beam = beam_allocation_procedure(&mac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame);
      bool sch_ret = beam.idx >= 0;
      /* Allocate retransmission */
      if (sch_ret)
        sch_ret = allocate_dl_retransmission(module_id, frame, slot, &n_rb_sched[beam.idx], UE, beam.idx, sched_pdsch->dl_harq_pid);
      if (!sch_ret) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] DL retransmission could not be allocated\n", UE->rnti, frame, slot);
        reset_beam_status(&mac->beam_info, frame, slot, UE->UE_beam_index, slots_per_frame, beam.new_beam);
        continue;
      }
      /* reduce max_num_ue once we are sure UE can be allocated, i.e., has CCE */
      remainUEs[beam.idx]--;

    } else {
      /* skip this UE if there are no free HARQ processes. This can happen e.g.
       * if the UE disconnected in L2sim, in which case the gNB is not notified
       * (this can be considered a design flaw) */
      if (sched_ctrl->available_dl_harq.head < 0) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] UE has no free DL HARQ process, skipping\n",
              UE->rnti,
              frame,
              slot);
        continue;
      }

      /* Check DL buffer and skip this UE if no bytes and no TA necessary */
      if (sched_ctrl->num_total_bytes == 0 && frame != (sched_ctrl->ta_frame + 100) % 1024)
        continue;

      /* Calculate coeff */
      const NR_bler_options_t *bo = &mac->dl_bler;
      const int max_mcs_table = current_BWP->mcsTableIdx == 1 ? 27 : 28;
      const int max_mcs = min(sched_ctrl->dl_max_mcs, max_mcs_table);
      if (bo->harq_round_max == 1) {
        sched_pdsch->mcs = min(bo->max_mcs, max_mcs);
        sched_ctrl->dl_bler_stats.mcs = sched_pdsch->mcs;
      } else
        sched_pdsch->mcs = get_mcs_from_bler(bo, stats, &sched_ctrl->dl_bler_stats, max_mcs, frame);
      sched_pdsch->nrOfLayers = get_dl_nrOfLayers(sched_ctrl, current_BWP->dci_format);
      sched_pdsch->pm_index =
          get_pm_index(mac, UE, current_BWP->dci_format, sched_pdsch->nrOfLayers, mac->radio_config.pdsch_AntennaPorts.XP);
        const uint8_t coeff_mcs = get_mcs_for_coeff_tbs(UE, sched_pdsch->mcs);
        const uint8_t Qm = nr_get_Qm_dl(coeff_mcs, current_BWP->mcsTableIdx);
        const uint16_t R = nr_get_code_rate_dl(coeff_mcs, current_BWP->mcsTableIdx);
      uint32_t tbs = nr_compute_tbs(Qm,
                                    R,
                                    1, /* rbSize */
                                    10, /* hypothetical number of slots */
                                    0, /* N_PRB_DMRS * N_DMRS_SLOT */
                                    0 /* N_PRB_oh, 0 for initialBWP */,
                                    0 /* tb_scaling */,
                                    sched_pdsch->nrOfLayers) >> 3;
      float coeff_ue = (float) tbs / UE->dl_thr_ue;
      qos_set_coeff_snapshot(UE, coeff_ue);
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] b %d, thr_ue %f, tbs %d, coeff_ue %f\n",
            UE->rnti,
            frame,
            slot,
            b,
            UE->dl_thr_ue,
            tbs,
            coeff_ue);
      /* Create UE_sched list for UEs eligible for new transmission*/
      UE_sched[curUE].coef=coeff_ue;
      UE_sched[curUE].UE=UE;
      curUE++;
    }
  }

  qsort(UE_sched, sizeofArray(UE_sched), sizeof(UEsched_t), comparator);
  UEsched_t *iterator = UE_sched;

  const int min_rbSize = 5;

  /* Loop UE_sched to find max coeff and allocate transmission */
  while (iterator->UE != NULL) {

    NR_UE_sched_ctrl_t *sched_ctrl = &iterator->UE->UE_sched_ctrl;
    const uint16_t rnti = iterator->UE->rnti;

    NR_UE_DL_BWP_t *dl_bwp = &iterator->UE->current_DL_BWP;
    NR_UE_UL_BWP_t *ul_bwp = &iterator->UE->current_UL_BWP;

    if (sched_ctrl->available_dl_harq.head < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] UE has no free DL HARQ process, skipping\n",
            iterator->UE->rnti,
            frame,
            slot);
      iterator++;
      continue;
    }

    NR_beam_alloc_t beam = beam_allocation_procedure(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame);

    if (beam.idx < 0) {
      // no available beam
      iterator++;
      continue;
    }
    if (remainUEs[beam.idx] == 0 || n_rb_sched[beam.idx] < min_rbSize) {
      reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    NR_sched_pdsch_t *sched_pdsch = &sched_ctrl->sched_pdsch;
    sched_pdsch->dl_harq_pid = sched_ctrl->available_dl_harq.head;

    /* MCS has been set above */
    sched_pdsch->time_domain_allocation = get_dl_tda(mac, slot);
    AssertFatal(sched_pdsch->time_domain_allocation>=0,"Unable to find PDSCH time domain allocation in list\n");

    const int coresetid = sched_ctrl->coreset->controlResourceSetId;
    sched_pdsch->tda_info = get_dl_tda_info(dl_bwp,
                                            sched_ctrl->search_space->searchSpaceType->present,
                                            sched_pdsch->time_domain_allocation,
                                            scc->dmrs_TypeA_Position,
                                            1,
                                            TYPE_C_RNTI_,
                                            coresetid,
                                            false);
    AssertFatal(sched_pdsch->tda_info.valid_tda, "Invalid TDA from get_dl_tda_info\n");

    NR_tda_info_t *tda_info = &sched_pdsch->tda_info;

    const uint16_t slbitmap = SL_to_bitmap(tda_info->startSymbolIndex, tda_info->nrOfSymbols);

    uint16_t *rballoc_mask = mac->common_channels[CC_id].vrb_map[beam.idx];
    sched_pdsch->bwp_info = get_pdsch_bwp_start_size(mac, iterator->UE);
    int rbStart = 0; // WRT BWP start
    int rbStop = sched_pdsch->bwp_info.bwpSize - 1;
    int bwp_start = sched_pdsch->bwp_info.bwpStart;
    // Freq-demain allocation
    while (rbStart < rbStop && (rballoc_mask[rbStart + bwp_start] & slbitmap))
      rbStart++;

    uint16_t max_rbSize = 1;

    while (rbStart + max_rbSize <= rbStop && !(rballoc_mask[rbStart + max_rbSize + bwp_start] & slbitmap))
      max_rbSize++;

    if (max_rbSize < min_rbSize) {
      LOG_D(NR_MAC,
            "(%d.%d) Cannot schedule RNTI %04x, rbStart %d, rbSize %d, rbStop %d\n",
            frame,
            slot,
            rnti,
            rbStart,
            max_rbSize,
            rbStop);
      iterator++;
      continue;
    }

    int CCEIndex = get_cce_index(mac,
                                 CC_id,
                                 slot,
                                 iterator->UE->rnti,
                                 &sched_ctrl->aggregation_level,
                                 beam.idx,
                                 sched_ctrl->search_space,
                                 sched_ctrl->coreset,
                                 &sched_ctrl->sched_pdcch,
                                 false,
                                 sched_ctrl->pdcch_cl_adjust);
    if (CCEIndex < 0) {
      LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find free CCE for DL DCI\n", rnti, frame, slot);
      reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
      iterator++;
      continue;
    }

    /* Find PUCCH occasion: if it fails, undo CCE allocation (undoing PUCCH
    * allocation after CCE alloc fail would be more complex) */

    int alloc = -1;
    if (!get_FeedbackDisabled(iterator->UE->sc_info.downlinkHARQ_FeedbackDisabled_r17, sched_pdsch->dl_harq_pid)) {
      int r_pucch = nr_get_pucch_resource(sched_ctrl->coreset, ul_bwp->pucch_Config, CCEIndex);
      alloc = nr_acknack_scheduling(mac, iterator->UE, frame, slot, iterator->UE->UE_beam_index, r_pucch, 0);
      if (alloc < 0) {
        LOG_D(NR_MAC, "[UE %04x][%4d.%2d] could not find PUCCH for DL DCI\n", rnti, frame, slot);
        reset_beam_status(&mac->beam_info, frame, slot, iterator->UE->UE_beam_index, slots_per_frame, beam.new_beam);
        iterator++;
        continue;
      }
    }

    sched_ctrl->cce_index = CCEIndex;
    fill_pdcch_vrb_map(mac, CC_id, &sched_ctrl->sched_pdcch, CCEIndex, sched_ctrl->aggregation_level, beam.idx);

    sched_pdsch->dmrs_parms = get_dl_dmrs_params(scc, dl_bwp, tda_info, sched_pdsch->nrOfLayers);
    sched_pdsch->Qm = nr_get_Qm_dl(sched_pdsch->mcs, dl_bwp->mcsTableIdx);
    sched_pdsch->R = nr_get_code_rate_dl(sched_pdsch->mcs, dl_bwp->mcsTableIdx);
    sched_pdsch->pucch_allocation = alloc;
    uint32_t TBS = 0;
    uint16_t rbSize;
    // Fix me: currently, the RLC does not give us the total number of PDUs
    // awaiting. Therefore, for the time being, we put a fixed overhead of 12
    // (for 4 PDUs) and optionally + 2 for TA. Once RLC gives the number of
    // PDUs, we replace with 3 * numPDUs
    const int oh = 3 * 4 + 2 * (frame == (sched_ctrl->ta_frame + 100) % 1024);
    //const int oh = 3 * sched_ctrl->dl_pdus_total + 2 * (frame == (sched_ctrl->ta_frame + 100) % 1024);
    nr_find_nb_rb(sched_pdsch->Qm,
                  sched_pdsch->R,
                  1, // no transform precoding for DL
                  sched_pdsch->nrOfLayers,
                  tda_info->nrOfSymbols,
                  sched_pdsch->dmrs_parms.N_PRB_DMRS * sched_pdsch->dmrs_parms.N_DMRS_SLOT,
                  sched_ctrl->num_total_bytes + oh,
                  min_rbSize,
                  max_rbSize,
                  &TBS,
                  &rbSize);
    sched_pdsch->rbSize = rbSize;
    sched_pdsch->rbStart = rbStart;
    sched_pdsch->tb_size = TBS;
    /* transmissions: directly allocate */
    n_rb_sched[beam.idx] -= sched_pdsch->rbSize;

    for (int rb = bwp_start; rb < sched_pdsch->rbSize; rb++)
      rballoc_mask[rb + sched_pdsch->rbStart] |= slbitmap;

    remainUEs[beam.idx]--;
    iterator++;
  }
}

static void nr_dlsch_preprocessor(module_id_t module_id, frame_t frame, slot_t slot)
{
  init_pset_hol_file_once();

  gNB_MAC_INST *mac = RC.nrmac[module_id];
  NR_UEs_t *UE_info = &mac->UE_info;

  if (UE_info->connected_ue_list[0] == NULL)
    return;

  NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  int bw = scc->downlinkConfigCommon->frequencyInfoDL->scs_SpecificCarrierList.list.array[0]->carrierBandwidth;
  int num_beams = mac->beam_info.beam_allocation ? mac->beam_info.beams_per_period : 1;
  int n_rb_sched[num_beams];
  for (int i = 0; i < num_beams; i++)
    n_rb_sched[i] = bw;

  qos_reset_coeff_snapshot();

  /* Retrieve amount of data to send for this UE */
  nr_store_dlsch_buffer(module_id, frame, slot);

  int average_agg_level = 4; // TODO find a better estimation
  int max_sched_ues = bw / (average_agg_level * NR_NB_REG_PER_CCE);

  // FAPI cannot handle more than MAX_DCI_CORESET DCIs
  max_sched_ues = min(max_sched_ues, MAX_DCI_CORESET);

  if (mac->radio_config.dl_scheduler_type == NR_DL_SCHED_CUSTOM) {
    scheduler_custom(module_id, frame, slot, UE_info->connected_ue_list, max_sched_ues, num_beams, n_rb_sched);
  } else {
    /* Default scheduler */
    pf_dl(module_id, frame, slot, UE_info->connected_ue_list, max_sched_ues, num_beams, n_rb_sched);
  }

  /* Write QoS statistics to file every x slots */
  // write_qos_stats_to_file(module_id, frame, slot);

}

nr_pp_impl_dl nr_init_dlsch_preprocessor(int CC_id) {
  /* during initialization: no mutex needed */
  /* in the PF algorithm, we have to use the TBsize to compute the coefficient.
   * This would include the number of DMRS symbols, which in turn depends on
   * the time domain allocation. In case we are in a mixed slot, we do not want
   * to recalculate all these values just, and therefore we provide a look-up
   * table which should approximately give us the TBsize */
  for (int mcsTableIdx = 0; mcsTableIdx < 3; ++mcsTableIdx) {
    for (int mcs = 0; mcs < 29; ++mcs) {
      if (mcs > 27 && mcsTableIdx == 1)
        continue;

      const uint8_t Qm = nr_get_Qm_dl(mcs, mcsTableIdx);
      const uint16_t R = nr_get_code_rate_dl(mcs, mcsTableIdx);
      pf_tbs[mcsTableIdx][mcs] = nr_compute_tbs(Qm,
                                                R,
                                                1, /* rbSize */
                                                10, /* hypothetical number of slots */
                                                0, /* N_PRB_DMRS * N_DMRS_SLOT */
                                                0 /* N_PRB_oh, 0 for initialBWP */,
                                                0 /* tb_scaling */,
                                                1 /* nrOfLayers */) >> 3;
    }
  }

  return nr_dlsch_preprocessor;
}

nfapi_nr_dl_tti_pdsch_pdu_rel15_t *prepare_pdsch_pdu(nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdsch_pdu,
                                                     const gNB_MAC_INST *mac,
                                                     const NR_UE_info_t *UE,
                                                     const NR_sched_pdsch_t *sched_pdsch,
                                                     const NR_PDSCH_Config_t *pdsch_Config,
                                                     bool is_sib1,
                                                     int harq_round,
                                                     int rnti,
                                                     int beam_index,
                                                     int nl_tbslbrm,
                                                     int pdu_index)
{
  const NR_UE_DL_BWP_t *dl_bwp = UE ? &UE->current_DL_BWP : NULL;
  const NR_ServingCellConfigCommon_t *scc = mac->common_channels[0].ServingCellConfigCommon;
  nfapi_nr_dl_tti_pdsch_pdu_rel15_t *pdsch_pdu = &dl_tti_pdsch_pdu->pdsch_pdu.pdsch_pdu_rel15;
  pdsch_pdu->pduBitmap = 0;
  pdsch_pdu->rnti = rnti;
  pdsch_pdu->pduIndex = pdu_index;
  pdsch_pdu->BWPSize  = sched_pdsch->bwp_info.bwpSize;
  pdsch_pdu->BWPStart = sched_pdsch->bwp_info.bwpStart;
  pdsch_pdu->SubcarrierSpacing = dl_bwp ? dl_bwp->scs : *scc->ssbSubcarrierSpacing;
  pdsch_pdu->CyclicPrefix = dl_bwp && dl_bwp->cyclicprefix ? *dl_bwp->cyclicprefix : 0;
  // Codeword information
  pdsch_pdu->NrOfCodewords = 1;
  pdsch_pdu->targetCodeRate[0] = sched_pdsch->R;
  pdsch_pdu->qamModOrder[0] = sched_pdsch->Qm;
  pdsch_pdu->mcsIndex[0] = sched_pdsch->mcs;
  pdsch_pdu->mcsTable[0] = dl_bwp ? dl_bwp->mcsTableIdx : 0;
  pdsch_pdu->rvIndex[0] = nr_get_rv(harq_round % 4);
  pdsch_pdu->TBSize[0] = sched_pdsch->tb_size;
  pdsch_pdu->dataScramblingId = pdsch_Config && pdsch_Config->dataScramblingIdentityPDSCH ? *pdsch_Config->dataScramblingIdentityPDSCH : *scc->physCellId;
  pdsch_pdu->nrOfLayers = sched_pdsch->nrOfLayers;
  pdsch_pdu->transmissionScheme = 0;
  pdsch_pdu->refPoint = is_sib1;
  // DMRS
  const NR_pdsch_dmrs_t *dmrs_parms = &sched_pdsch->dmrs_parms;
  pdsch_pdu->dlDmrsSymbPos = dmrs_parms->dl_dmrs_symb_pos;
  pdsch_pdu->dmrsConfigType = dmrs_parms->dmrsConfigType;
  pdsch_pdu->SCID = dmrs_parms->n_scid;
  pdsch_pdu->dlDmrsScramblingId = dmrs_parms->scrambling_id;
  pdsch_pdu->numDmrsCdmGrpsNoData = dmrs_parms->numDmrsCdmGrpsNoData;
  pdsch_pdu->dmrsPorts = (1 << sched_pdsch->nrOfLayers) - 1;  // FIXME with a better implementation
  // Pdsch Allocation in frequency domain
  pdsch_pdu->resourceAlloc = 1;
  pdsch_pdu->rbStart = sched_pdsch->rbStart;
  pdsch_pdu->rbSize = sched_pdsch->rbSize;
  pdsch_pdu->VRBtoPRBMapping = 0; // non-interleaved
  // Resource Allocation in time domain
  const NR_tda_info_t *tda_info = &sched_pdsch->tda_info;
  pdsch_pdu->StartSymbolIndex = tda_info->startSymbolIndex;
  pdsch_pdu->NrOfSymbols = tda_info->nrOfSymbols;
  /* Check and validate PTRS values */
  if (dmrs_parms->phaseTrackingRS) {
    bool valid_ptrs_setup = set_dl_ptrs_values(dmrs_parms->phaseTrackingRS,
                                               pdsch_pdu->rbSize,
                                               pdsch_pdu->mcsIndex[0],
                                               pdsch_pdu->mcsTable[0],
                                               &pdsch_pdu->PTRSFreqDensity,
                                               &pdsch_pdu->PTRSTimeDensity,
                                               &pdsch_pdu->PTRSPortIndex,
                                               &pdsch_pdu->nEpreRatioOfPDSCHToPTRS,
                                               &pdsch_pdu->PTRSReOffset,
                                               pdsch_pdu->NrOfSymbols);
    if (valid_ptrs_setup)
      pdsch_pdu->pduBitmap |= 0x1; // Bit 0: pdschPtrs - Indicates PTRS included (FR2)
  }
  int dl_bw_tbslbrm = UE ? UE->sc_info.dl_bw_tbslbrm : sched_pdsch->bwp_info.bwpSize;
  pdsch_pdu->maintenance_parms_v3.tbSizeLbrmBytes = nr_compute_tbslbrm(pdsch_pdu->mcsTable[0], dl_bw_tbslbrm, nl_tbslbrm);
  pdsch_pdu->maintenance_parms_v3.ldpcBaseGraph = get_BG(sched_pdsch->tb_size << 3, sched_pdsch->R);
  // Precoding and beamforming
  pdsch_pdu->precodingAndBeamforming.num_prgs = 0;
  pdsch_pdu->precodingAndBeamforming.prg_size = pdsch_pdu->rbSize;
  pdsch_pdu->precodingAndBeamforming.dig_bf_interfaces = 0;
  pdsch_pdu->precodingAndBeamforming.prgs_list[0].pm_idx = sched_pdsch->pm_index;
  pdsch_pdu->precodingAndBeamforming.prgs_list[0].dig_bf_interface_list[0].beam_idx = beam_index;
  return pdsch_pdu;
}

void nr_schedule_ue_spec(module_id_t module_id,
                         frame_t frame,
                         slot_t slot,
                         nfapi_nr_dl_tti_request_t *DL_req,
                         nfapi_nr_tx_data_request_t *TX_req)
{
  gNB_MAC_INST *gNB_mac = RC.nrmac[module_id];
  /* already mutex protected: held in gNB_dlsch_ulsch_scheduler() */
  AssertFatal(pthread_mutex_trylock(&gNB_mac->sched_lock) == EBUSY,
              "this function should be called with the scheduler mutex locked\n");

  if (!is_dl_slot(slot, &gNB_mac->frame_structure))
    return;

  /* PREPROCESSOR */
  gNB_mac->pre_processor_dl(module_id, frame, slot);
  const int CC_id = 0;
  NR_ServingCellConfigCommon_t *scc = gNB_mac->common_channels[CC_id].ServingCellConfigCommon;
  NR_UEs_t *UE_info = &gNB_mac->UE_info;
  nfapi_nr_dl_tti_request_body_t *dl_req = &DL_req->dl_tti_request_body;

  const NR_BWP_t *initialDL = &scc->downlinkConfigCommon->initialDownlinkBWP->genericParameters;
  gNB_mac->mac_stats.total_prb_aggregate += NRRIV2BW(initialDL->locationAndBandwidth, MAX_BWP_SIZE);

  UE_iterator(UE_info->connected_ue_list, UE) {
    NR_UE_sched_ctrl_t *sched_ctrl = &UE->UE_sched_ctrl;
    NR_UE_DL_BWP_t *current_BWP = &UE->current_DL_BWP;

    if (sched_ctrl->ul_failure && !get_softmodem_params()->phy_test)
      continue;

    NR_sched_pdsch_t *sched_pdsch = &sched_ctrl->sched_pdsch;
    UE->mac_stats.dl.current_bytes = 0;
    UE->mac_stats.dl.current_rbs = 0;

    /* update TA and set ta_apply every 100 frames.
     * Possible improvement: take the periodicity from input file.
     * If such UE is not scheduled now, it will be by the preprocessor later.
     * If we add the CE, ta_apply will be reset */
    if (frame == ((sched_ctrl->ta_frame + 100) % 1024) && !nr_timer_is_active(&sched_ctrl->transm_interrupt)) {
      sched_ctrl->ta_apply = true; /* the timer is reset once TA CE is scheduled */
      LOG_D(NR_MAC, "[UE %04x][%d.%d] UL timing alignment procedures: setting flag for Timing Advance command\n", UE->rnti, frame, slot);
    }

    if (sched_pdsch->rbSize <= 0)
      continue;

    const rnti_t rnti = UE->rnti;

    /* POST processing */
    const uint8_t nrOfLayers = sched_pdsch->nrOfLayers;
    const uint32_t TBS = sched_pdsch->tb_size;
    int8_t current_harq_pid = sched_pdsch->dl_harq_pid;

    if (current_harq_pid < 0) {
      /* PP has not selected a specific HARQ Process, get a new one */
      current_harq_pid = sched_ctrl->available_dl_harq.head;
      AssertFatal(current_harq_pid >= 0,
                  "no free HARQ process available for UE %04x\n",
                  UE->rnti);
      remove_front_nr_list(&sched_ctrl->available_dl_harq);
      sched_pdsch->dl_harq_pid = current_harq_pid;
    } else {
      /* PP selected a specific HARQ process. Check whether it will be a new
       * transmission or a retransmission, and remove from the corresponding
       * list */
      if (sched_ctrl->harq_processes[current_harq_pid].round == 0)
        remove_nr_list(&sched_ctrl->available_dl_harq, current_harq_pid);
      else
        remove_nr_list(&sched_ctrl->retrans_dl_harq, current_harq_pid);
    }

    NR_tda_info_t *tda_info = &sched_pdsch->tda_info;
    NR_pdsch_dmrs_t *dmrs_parms = &sched_pdsch->dmrs_parms;
    NR_UE_harq_t *harq = &sched_ctrl->harq_processes[current_harq_pid];
    NR_sched_pucch_t *pucch = NULL;
    DevAssert(!harq->is_waiting);
    if (sched_pdsch->pucch_allocation < 0) {
      finish_nr_dl_harq(sched_ctrl, current_harq_pid);
    } else {
      pucch = &sched_ctrl->sched_pucch[sched_pdsch->pucch_allocation];
      add_tail_nr_list(&sched_ctrl->feedback_dl_harq, current_harq_pid);
      harq->feedback_frame = pucch->frame;
      harq->feedback_slot = pucch->ul_slot;
      harq->is_waiting = true;
    }
    UE->mac_stats.dl.rounds[harq->round]++;
    LOG_D(NR_MAC,
          "%4d.%2d [DLSCH/PDSCH/PUCCH] RNTI %04x DCI L %d start %3d RBs %3d startSymbol %2d nb_symbol %2d dmrspos %x MCS %2d nrOfLayers %d TBS %4d HARQ PID %2d round %d RV %d NDI %d dl_data_to_ULACK %d (%d.%d) PUCCH allocation %d TPC %d\n",
          frame,
          slot,
          rnti,
          sched_ctrl->aggregation_level,
          sched_pdsch->rbStart,
          sched_pdsch->rbSize,
          tda_info->startSymbolIndex,
          tda_info->nrOfSymbols,
          dmrs_parms->dl_dmrs_symb_pos,
          sched_pdsch->mcs,
          nrOfLayers,
          TBS,
          current_harq_pid,
          harq->round,
          nr_get_rv(harq->round % 4),
          harq->ndi,
          pucch ? pucch->timing_indicator : 0,
          pucch ? pucch->frame : 0,
          pucch ? pucch->ul_slot : 0,
          sched_pdsch->pucch_allocation,
          sched_ctrl->tpc1);

    const int bwp_id = current_BWP->bwp_id;
    const int coresetid = sched_ctrl->coreset->controlResourceSetId;

    /* look up the PDCCH PDU for this CC, BWP, and CORESET. If it does not exist, create it */
    nfapi_nr_dl_tti_pdcch_pdu_rel15_t *pdcch_pdu = gNB_mac->pdcch_pdu_idx[CC_id][coresetid];

    if (!pdcch_pdu) {
      LOG_D(NR_MAC, "creating pdcch pdu, pdcch_pdu = NULL. \n");
      nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdcch_pdu = &dl_req->dl_tti_pdu_list[dl_req->nPDUs];
      memset(dl_tti_pdcch_pdu, 0, sizeof(nfapi_nr_dl_tti_request_pdu_t));
      dl_tti_pdcch_pdu->PDUType = NFAPI_NR_DL_TTI_PDCCH_PDU_TYPE;
      dl_tti_pdcch_pdu->PDUSize = (uint8_t)(2+sizeof(nfapi_nr_dl_tti_pdcch_pdu));
      dl_req->nPDUs += 1;
      pdcch_pdu = &dl_tti_pdcch_pdu->pdcch_pdu.pdcch_pdu_rel15;
      LOG_D(NR_MAC,"Trying to configure DL pdcch for UE %04x, bwp %d, cs %d\n", UE->rnti, bwp_id, coresetid);
      NR_ControlResourceSet_t *coreset = sched_ctrl->coreset;
      nr_configure_pdcch(pdcch_pdu, coreset, &sched_ctrl->sched_pdcch);
      gNB_mac->pdcch_pdu_idx[CC_id][coresetid] = pdcch_pdu;
    }

    nfapi_nr_dl_tti_request_pdu_t *dl_tti_pdsch_pdu = &dl_req->dl_tti_pdu_list[dl_req->nPDUs];
    memset(dl_tti_pdsch_pdu, 0, sizeof(nfapi_nr_dl_tti_request_pdu_t));
    dl_tti_pdsch_pdu->PDUType = NFAPI_NR_DL_TTI_PDSCH_PDU_TYPE;
    dl_tti_pdsch_pdu->PDUSize = (uint8_t)(2 + sizeof(nfapi_nr_dl_tti_pdsch_pdu));
    dl_req->nPDUs += 1;
    /* SCF222: PDU index incremented for each PDSCH PDU sent in TX control
     * message. This is used to associate control information to data and is
     * reset every slot. */
    const int pduindex = gNB_mac->pdu_index[CC_id]++;
    // TODO: verify the case where maxMIMO_Layers is NULL, in which case
    //       in principle maxMIMO_layers should be given by the maximum number of layers
    //       for PDSCH supported by the UE for the serving cell (5.4.2.1 of 38.212)
    long maxMIMO_Layers = UE->sc_info.maxMIMO_Layers_PDSCH ? *UE->sc_info.maxMIMO_Layers_PDSCH : 1;
    const int nl_tbslbrm = min(maxMIMO_Layers, 4);
    nfapi_nr_dl_tti_pdsch_pdu_rel15_t *pdsch_pdu = prepare_pdsch_pdu(dl_tti_pdsch_pdu,
                                                                     gNB_mac,
                                                                     UE,
                                                                     sched_pdsch,
                                                                     current_BWP->pdsch_Config,
                                                                     false,
                                                                     harq->round,
                                                                     rnti,
                                                                     UE->UE_beam_index,
                                                                     nl_tbslbrm,
                                                                     pduindex);


    LOG_D(NR_MAC,"Configuring DCI/PDCCH in %d.%d at CCE %d, rnti %x\n", frame,slot,sched_ctrl->cce_index,rnti);
    /* Fill PDCCH DL DCI PDU */
    nfapi_nr_dl_dci_pdu_t *dci_pdu = prepare_dci_pdu(pdcch_pdu,
                                                     scc,
                                                     sched_ctrl->search_space,
                                                     sched_ctrl->coreset,
                                                     sched_ctrl->aggregation_level,
                                                     sched_ctrl->cce_index,
                                                     UE->UE_beam_index,
                                                     rnti);
    pdcch_pdu->numDlDci++;

    /* DCI payload */
    const int rnti_type = TYPE_C_RNTI_;
    dci_pdu_rel15_t dci_payload = prepare_dci_dl_payload(gNB_mac,
                                                         UE,
                                                         rnti_type,
                                                         sched_ctrl->search_space->searchSpaceType->present,
                                                         pdsch_pdu,
                                                         sched_pdsch,
                                                         pucch,
                                                         current_harq_pid,
                                                         0,
                                                         false);

    // Reset TPC to 0 dB to not request new gain multiple times before computing new value for SNR
    sched_ctrl->tpc1 = 1;
    NR_PDSCH_Config_t *pdsch_Config = current_BWP->pdsch_Config;
    AssertFatal(pdsch_Config == NULL
                || pdsch_Config->resourceAllocation == NR_PDSCH_Config__resourceAllocation_resourceAllocationType1,
                "Only frequency resource allocation type 1 is currently supported\n");

    LOG_D(NR_MAC,
          "%4d.%2d DCI type 1 payload: freq_alloc %d (%d,%d,%d), "
          "nrOfLayers %d, time_alloc %d, vrb to prb %d, mcs %d tb_scaling %d ndi %d rv %d tpc %d ti %d\n",
          frame,
          slot,
          dci_payload.frequency_domain_assignment.val,
          pdsch_pdu->rbStart,
          pdsch_pdu->rbSize,
          pdsch_pdu->BWPSize,
          pdsch_pdu->nrOfLayers,
          dci_payload.time_domain_assignment.val,
          dci_payload.vrb_to_prb_mapping.val,
          dci_payload.mcs,
          dci_payload.tb_scaling,
          dci_payload.ndi,
          dci_payload.rv,
          dci_payload.tpc,
          pucch ? pucch->timing_indicator : 0);

    fill_dci_pdu_rel15(&UE->sc_info,
                       current_BWP,
                       &UE->current_UL_BWP,
                       dci_pdu,
                       &dci_payload,
                       current_BWP->dci_format,
                       rnti_type,
                       bwp_id,
                       sched_ctrl->search_space,
                       sched_ctrl->coreset,
                       UE->pdsch_HARQ_ACK_Codebook,
                       gNB_mac->cset0_bwp_size);

    LOG_D(NR_MAC,
          "coreset params: FreqDomainResource %llx, start_symbol %d  n_symb %d\n",
          (unsigned long long)pdcch_pdu->FreqDomainResource,
          pdcch_pdu->StartSymbolIndex,
          pdcch_pdu->DurationSymbols);

    if (harq->round != 0) { /* retransmission */
      /* we do not have to do anything, since we do not require to get data
       * from RLC or encode MAC CEs. The TX_req structure is filled below
       * or copy data to FAPI structures */
      LOG_D(NR_MAC,
            "%d.%2d DL retransmission RNTI %04x HARQ PID %d round %d NDI %d\n",
            frame,
            slot,
            rnti,
            current_harq_pid,
            harq->round,
            harq->ndi);
      AssertFatal(harq->sched_pdsch.tb_size == TBS,
                  "UE %04x mismatch between scheduled TBS and buffered TB for HARQ PID %d\n",
                  UE->rnti,
                  current_harq_pid);
      T(T_GNB_MAC_RETRANSMISSION_DL_PDU_WITH_DATA, T_INT(module_id), T_INT(CC_id), T_INT(rnti),
        T_INT(frame), T_INT(slot), T_INT(current_harq_pid), T_INT(harq->round), T_BUFFER(harq->transportBlock.buf, TBS));
      UE->mac_stats.dl.total_rbs_retx += sched_pdsch->rbSize;
      gNB_mac->mac_stats.used_prb_aggregate += sched_pdsch->rbSize;
    } else { /* initial transmission */
      LOG_D(NR_MAC, "Initial HARQ transmission in %d.%d\n", frame, slot);
      uint8_t *buf = allocate_transportBlock_buffer(&harq->transportBlock, TBS);
      /* first, write all CEs that might be there */
      int written = nr_write_ce_dlsch_pdu(module_id,
                                          sched_ctrl,
                                          (unsigned char *)buf,
                                          255, // no drx
                                          NULL); // contention res id
      buf += written;
      uint8_t *bufEnd = buf + TBS - written;
      DevAssert(TBS > written);
      int dlsch_total_bytes = 0;
      /* next, get RLC data */
      start_meas(&gNB_mac->rlc_data_req);
      int sdus = 0;

      if (sched_ctrl->num_total_bytes > 0) {
        /* loop over all activated logical channels */
        for (int i = 0; i < seq_arr_size(&sched_ctrl->lc_config); ++i) {
          const nr_lc_config_t *c = seq_arr_at(&sched_ctrl->lc_config, i);
          const int lcid = c->lcid;

          for (int qfi = 0; qfi < NR_MAX_NUM_QFI; qfi++) {
            if (c->qos_config[qfi].fiveQI != 0) {
              // LOG_I(MAC, "UE %d -> 5QI = %d \n", UE->rnti, c->qos_config[qfi].fiveQI);
              break;
            }
          }

          if (sched_ctrl->rlc_status[lcid].bytes_in_buffer == 0)
            continue; // no data for this LC        tbs_size_t len = 0;

          int lcid_bytes=0;
          while (bufEnd-buf > sizeof(NR_MAC_SUBHEADER_LONG) + 1 ) {
            // we do not know how much data we will get from RLC, i.e., whether it
            // will be longer than 256B or not. Therefore, reserve space for long header, then
            // fetch data, then fill real length
            NR_MAC_SUBHEADER_LONG *header = (NR_MAC_SUBHEADER_LONG *) buf;
            /* limit requested number of bytes to what preprocessor specified, or
             * such that TBS is full */
            const rlc_buffer_occupancy_t ndata = min(sched_ctrl->rlc_status[lcid].bytes_in_buffer,
                                                     bufEnd-buf-sizeof(NR_MAC_SUBHEADER_LONG));
            tbs_size_t len = nr_mac_rlc_data_req(module_id,
                                                 rnti,
                                                 true,
                                                 lcid,
                                                 ndata,
                                                 (char *)buf+sizeof(NR_MAC_SUBHEADER_LONG));
            LOG_D(NR_MAC,
                  "%4d.%2d RNTI %04x: %d bytes from %s %d (ndata %d, remaining size %ld)\n",
                  frame,
                  slot,
                  rnti,
                  len,
                  lcid < 4 ? "DCCH" : "DTCH",
                  lcid,
                  ndata,
                  bufEnd-buf-sizeof(NR_MAC_SUBHEADER_LONG));

            if (len == 0)
              break;

            header->R = 0;
            header->F = 1;
            header->LCID = lcid;
            header->L = htons(len);
            buf += len+sizeof(NR_MAC_SUBHEADER_LONG);
            dlsch_total_bytes += len;
            lcid_bytes += len;
            sdus += 1;
          }

          UE->mac_stats.dl.lc_bytes[lcid] += lcid_bytes;
        }
      } else if (get_softmodem_params()->phy_test || get_softmodem_params()->do_ra) {
        /* we will need the large header, phy-test typically allocates all
         * resources and fills to the last byte below */
        LOG_D(NR_MAC, "Configuring DL_TX in %d.%d: TBS %d of random data\n", frame, slot, TBS);

        if (bufEnd-buf > sizeof(NR_MAC_SUBHEADER_LONG) ) {
          NR_MAC_SUBHEADER_LONG *header = (NR_MAC_SUBHEADER_LONG *) buf;
          // fill dlsch_buffer with random data
          header->R = 0;
          header->F = 1;
          header->LCID = DL_SCH_LCID_PADDING;
          buf += sizeof(NR_MAC_SUBHEADER_LONG);
          header->L = htons(bufEnd-buf);

          for (; ((intptr_t)buf) % 4; buf++)
            *buf = lrand48() & 0xff;
          for (; buf < bufEnd - 3; buf += 4) {
            uint32_t *buf32 = (uint32_t *)buf;
            *buf32 = lrand48();
          }
          for (; buf < bufEnd; buf++)
            *buf = lrand48() & 0xff;
          sdus +=1;
        }
      }

      stop_meas(&gNB_mac->rlc_data_req);

      // Add padding header and zero rest out if there is space left
      if (bufEnd-buf > 0) {
        NR_MAC_SUBHEADER_FIXED *padding = (NR_MAC_SUBHEADER_FIXED *) buf;
        padding->R = 0;
        padding->LCID = DL_SCH_LCID_PADDING;
        buf += 1;
        memset(buf,0,bufEnd-buf);
        buf=bufEnd;
      }

      UE->mac_stats.dl.total_bytes += TBS;
      UE->mac_stats.dl.current_bytes = TBS;
      UE->mac_stats.dl.total_rbs += sched_pdsch->rbSize;
      UE->mac_stats.dl.num_mac_sdu += sdus;
      UE->mac_stats.dl.current_rbs = sched_pdsch->rbSize;
      UE->mac_stats.dl.total_sdu_bytes += dlsch_total_bytes;
      gNB_mac->mac_stats.used_prb_aggregate += sched_pdsch->rbSize;

      /* save retransmission information */
      harq->sched_pdsch = *sched_pdsch;
      /* save which time allocation has been used, to be used on
       * retransmissions */
      harq->sched_pdsch.time_domain_allocation = sched_pdsch->time_domain_allocation;

      // ta command is sent, values are reset
      if (sched_ctrl->ta_apply) {
        sched_ctrl->ta_apply = false;
        sched_ctrl->ta_frame = frame;
        LOG_D(NR_MAC, "%d.%2d UE %04x TA scheduled, resetting TA frame\n", frame, slot, UE->rnti);
      }

      T(T_GNB_MAC_DL_PDU_WITH_DATA, T_INT(module_id), T_INT(CC_id), T_INT(rnti),
        T_INT(frame), T_INT(slot), T_INT(current_harq_pid), T_BUFFER(harq->transportBlock.buf, TBS));
    }

    const int ntx_req = TX_req->Number_of_PDUs;
    nfapi_nr_pdu_t *tx_req = &TX_req->pdu_list[ntx_req];
    tx_req->PDU_index  = pduindex;
    tx_req->num_TLV = 1;
    tx_req->TLVs[0].length = TBS;
    tx_req->PDU_length = compute_PDU_length(tx_req->num_TLV, tx_req->TLVs[0].length);
    memcpy(tx_req->TLVs[0].value.direct, harq->transportBlock.buf, TBS);
    TX_req->Number_of_PDUs++;
    TX_req->SFN = frame;
    TX_req->Slot = slot;
    /* mark UE as scheduled */
    sched_pdsch->rbSize = 0;
  }
}
