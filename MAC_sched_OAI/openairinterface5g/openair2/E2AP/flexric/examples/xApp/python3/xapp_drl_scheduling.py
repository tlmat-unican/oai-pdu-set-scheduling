import ctypes
import xapp_sdk as ric
import time
import signal
import requests
import numpy as np

# Global list to maintain references to SWIG objects
_keep_alive_refs = []

def send_control_mac(node_id, n_users, action):
    global _keep_alive_refs

    try:
        # 1. Create the base control message
        ctrl = ric.mac_ctrl_msg_t()
        ctrl.n_users = n_users
        ctrl.v = action["v"]

        # 2. Convert integer lists to 'bytes'
        wq_bytes = bytes(action["wq"])
        wg_bytes = bytes(action["wg"])
        wq_str = wq_bytes.decode('latin-1')
        wg_str = wg_bytes.decode('latin-1')

        # 4. Call the constructor function
        # Now it will accept the argument because it's a 'str'
        ba_wq = ric.cp_str_to_ba(wq_str)
        ba_wg = ric.cp_str_to_ba(wg_str)

        # 5. Assign the internal pointer (.buf) to the control structure
        # .buf returns the 'uint8_t *' expected by the MAC message
        ctrl.wq = ba_wq.buf
        ctrl.wg = ba_wg.buf

        # 6. Keep references to prevent Python from freeing the memory
        # If 'ba_wq' is garbage collected, 'ctrl.wq' will point to garbage.
        _keep_alive_refs.append((ba_wq, ba_wg))

        # Clean up very old references to avoid filling RAM indefinitely
        if len(_keep_alive_refs) > 20:
            _keep_alive_refs.pop(0)

        print(f"[CONTROL] Sending: v={action['v']}, wq={action['wq']}, wg={action['wg']}")

        # 7. Send message
        result = ric.control_mac_sm(node_id, ctrl)
        # result = ric.control_mac_sched_sm(node_id, ctrl)

        if result >= 0:
            print(f"[CONTROL] Sent successfully (handle={result})")
            return 0
        else:
            print(f"[CONTROL] Failed: error code {result}")
            return -1

    except Exception as e:
        print(f"  [ERROR] send_control_mac: {e}")
        import traceback
        traceback.print_exc()
        return -1

#########################################################
# HELPER: Convert SWIG pointer to Python array
#########################################################
def swig_ptr_to_array(swig_ptr, size, dtype=ctypes.c_uint8):
    """
    Efficiently converts a SWIG pointer to a Python list.
    """
    if not swig_ptr or size <= 0:
        return []

    try:
        # 1. Get the real memory address (C pointer)
        # Most SWIG objects return the raw address when calling int()
        addr = int(swig_ptr)

        # 2. Define the ctypes array type of exact size
        # (uint8_t * size)
        ArrayType = dtype * size

        # 3. "Map" that memory address to our array definition
        c_array = ArrayType.from_address(addr)

        # 4. Convert to native Python list (makes a copy of the values)
        return list(c_array)

    except Exception as e:
        print(f"  [ERROR] swig_ptr_to_array: {e}")
        return [0] * size

#########################################################
# 1. DRL MODEL DEFINITION (Logic)
#########################################################
def configure_drl(node_id):
    config = {"num_ues": 4, "model": "DQN", "env_type": "Basic"}
    # config = {"num_ues": 4, "model": "Random", "env_type": "Basic"}
    session = requests.Session()
    try:
        response = session.post("http://localhost:8000/configure", json=config)
    except Exception as e:
        print(f"Error during configuration: {e}")
    return session


def step_drl(node_id, ue_stats, t_diff, self):
    print(f"--- [DRL Step] Delay: {t_diff:.2f} μs ---")

    state_reward = {
    "state": {
        "q": [],
        "g": [],
        "mcs": [],
        "closeToObstacle": []
    },
    "reward": 0
    }

    # Iterate over users reported by MAC
    n_users = len(ue_stats)
    gfbr = [3, 3, 6, 6] # TODO include in SM and get from there instead
    reward = 0
    current_rbs = 0
    current_thput_ue = np.zeros(4) # 4 UEs max, TODO make dynamic based on n_users and include in SM
    for i, ue in enumerate(ue_stats):
        rnti = ue.rnti
        n_flows = ue.n_flows

        try:
            queues = swig_ptr_to_array(ue.queues, n_flows, ctypes.c_uint8)
            virtual_queues = swig_ptr_to_array(ue.virtual_queues, n_flows, ctypes.c_uint8)
            mcs = swig_ptr_to_array(ue.mcs, n_flows, ctypes.c_uint8)
            sensing = swig_ptr_to_array(ue.sensing, n_flows, ctypes.c_uint8)
            bytes_ue = swig_ptr_to_array(ue.bytes, n_flows, ctypes.c_uint64)
            rbs = swig_ptr_to_array(ue.rbs, n_flows, ctypes.c_uint32)
            print(f"[DEBUG] UE RNTI {rnti:#x} | n_flows: {n_flows} | Queues: {queues} | Virtual Queues: {virtual_queues} | MCS: {mcs} | Sensing: {sensing}")
            print(f"[DEBUG] Bytes: {bytes_ue} | RBs: {rbs}")
        except Exception as e:
            print(f"  Error accessing queues: {e}")
            queues = [0] * n_flows
            virtual_queues = [0] * n_flows
            mcs = [0] * n_flows
            sensing = [0] * n_flows

        state_reward["state"]["q"].extend(queues)
        state_reward["state"]["g"].extend(virtual_queues)
        state_reward["state"]["mcs"].extend(mcs)
        state_reward["state"]["closeToObstacle"].extend(sensing)

        interval = 1e6  # Subscription interval in us
        current_rbs += sum(rbs)  # We sum over UEs
        current_thput_ue[i] = (sum(bytes_ue) - self.bytes_old[i]) * 8 / interval  # Mbps
        print(f"In the last period ({interval} us), the UE had {current_thput_ue[i]} Mbps")
        reward += (1/n_users) * min(1, current_thput_ue[i]/gfbr[i])
        self.bytes_old[i] = sum(bytes_ue)

    # max_rbs = 1890192
    max_rbs = 212000 # TODO add in SM and get from there instead
    rbs_delta = current_rbs - self.rbs_old
    print(f"In the last period ({interval} us), {rbs_delta} RBs allocated")
    reward *= 0.2
    if reward == 0.2:
        reward += 0.3 + 0.5 * (1 - min(1, (rbs_delta/max_rbs)))
        # reward += b * (1-t-b) * (1 - min(1, (rbs_delta/max_rbs)))
    state_reward["reward"] = reward
    print(f"Current reward: {reward}")
    self.rbs_old = current_rbs

    print(f"State/Reward = {state_reward}")

    # --- DRL MODEL PLACEHOLDER ---
    try:
        response = self.session.post("http://localhost:8000/infer_sched_config", json=state_reward)
        action = response.json()
    except Exception as e:
        print(f"Error during inference: {e}")
        action = None

    # Fallback if action is None or invalid
    if action is None or not isinstance(action, dict):
        print(f"Warning: Invalid action received, using default")
        action = {
            "v": 0,
            "wq": [1 + i for i in range(n_users)],
            "wg": [1 + i for i in range(n_users)]
        }

    print(f"Action = {action}")

    # Send control
    send_control_mac(node_id, n_users, action)

#########################################################
# 2. CALLBACK DEFINITION (C++ -> Python Interface)
#########################################################
class MACCallback(ric.mac_cb):
    def __init__(self, node_id):
        ric.mac_cb.__init__(self)
        self.counter = 0
        self.node_id = node_id
        self.session = configure_drl(node_id)
        self.bytes_old = np.zeros(4)
        self.rbs_old = 0

    def handle(self, ind):
        try:
            self.counter += 1

            if len(ind.ue_stats) > 0:
                t_now = time.time_ns() / 1000.0
                t_mac = ind.tstamp / 1.0
                t_diff = t_now - t_mac
                step_drl(self.node_id, ind.ue_stats, t_diff, self)
        except Exception as e:
            print(f"[ERROR] Exception in MAC callback: {e}")
            import traceback
            traceback.print_exc()

#########################################################
# 3. MAIN (Configuration and Loop)
#########################################################
def main():
    print("=== Starting DRL xApp (MAC Only) ===")

    ric.init()

    # Restaurar el handler por defecto de Python
    signal.signal(signal.SIGINT, signal.default_int_handler)

    conn = ric.conn_e2_nodes()

    if len(conn) == 0:
        print("Error: No connected gNB found.")
        return

    print(f"Connected to {len(conn)} E2 nodes.")

    mac_handlers = []

    for i in range(len(conn)):
        node_id = conn[i].id
        plmn = f"{node_id.plmn.mcc}{node_id.plmn.mnc}"
        print(f"Subscribing to MAC SM on node [{i}] PLMN: {plmn}")

        mac_cb = MACCallback(node_id)
        hndlr = ric.report_mac_sm(node_id, ric.Interval_ms_1000, mac_cb)
        # hndlr = ric.report_mac_sched_sm(node_id, ric.Interval_ms_1000, mac_cb)
        mac_handlers.append(hndlr)

    print("=== xApp Running. Press Ctrl+C to stop ===")

    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        print("\nCtrl+C detected. Closing...")

    print("Cleaning up subscriptions...")
    for h in mac_handlers:
        ric.rm_report_mac_sm(h)
        # ric.rm_report_mac_sched_sm(h)

    print("xApp finished successfully.")

if __name__ == '__main__':
    main()