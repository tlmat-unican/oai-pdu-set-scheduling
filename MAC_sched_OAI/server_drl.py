from fastapi import FastAPI, Request
from pydantic import BaseModel
from threading import Condition, Thread
from stable_baselines3 import PPO, DQN
import random as rand
import numpy as np
import json
import os
from stable_baselines3.common.monitor import Monitor
from stable_baselines3.common.evaluation import evaluate_policy
from stable_baselines3.common.callbacks import BaseCallback
import csv
import time
from env.env_PPO_Basic import CustomEnv as CustomEnvBasic
from env.env_PPO_Basic import MeanRewardLogCallback as MeanRewardLogCallback
from env.env_DQN_Basic import CustomEnvDQN as CustomEnvDQNBasic
from env.env_DQN_Basic import MeanRewardLogCallbackDQN as MeanRewardLogCallbackDQNBasic

app = FastAPI()

# Configuration flags
LOAD_MODEL = False  # Set to True to load a pretrained model, False to train from scratch
PREDICT = False  # Set to True to use the model for prediction only, False to train
CONTINUE_TRAINING = False  # Set to True to continue training when /configure is called during active training

# Training hyperparameters
total_timesteps = 100_000
n_steps = 512
batch_size = 64
use_optimized_hyperparameters = True  # Use optimized hyperparameters from Optuna

# Global state variables
model_type = None
model = None
env = None
env_type = "Basic"  # Default environment type
condition = None
agent_thread = None
callback_class = MeanRewardLogCallback

# Training tracking variables
steps_counter = 0
csv_index = 1
pending_reward = None

# Transition mode variables (used when continuing training with compatible config)
transition_mode = False
transition_steps_remaining = 0
transition_action = None

def generate_timestamp(endpoint: str, step: int = None, note: str = None):
    """Save a simple timestamp to request_timing.csv for performance logging (does not modify payload)."""
    try:
        header_needed = not os.path.exists('request_timing.csv')
        with open('request_timing.csv', 'a', newline='') as tf:
            tw = csv.writer(tf)
            if header_needed:
                tw.writerow(['log_time', 'endpoint', 'step', 'note'])
            log_time_us = int(time.time() * 1_000_000)  # microseconds
            tw.writerow([log_time_us, endpoint, step if step is not None else '', note if note else ''])
    except Exception as e:
        print(f"[TIMESTAMP LOG ERROR] {e}")

class ConfigureRequest(BaseModel):
    model: str
    num_ues: int
    env_type: str

class InferRequest(BaseModel):
    state: dict
    reward: float

def random_tuning(num_ues):
    global env
    return {
        "v": rand.randint(0, 4),
        "wq": [rand.randint(0, 0) for _ in range(num_ues)],
        "wg": [rand.randint(0, 1) for _ in range(num_ues)]
    }

def static_tuning(type, mcs):
    return {
        "v": 1,
        "wq": 0,
        "wg": 1
    }

def ppo(state, reward):
    """Process state and reward through PPO/DQN model. Returns action for prediction or training."""
    global model, env, condition
    with condition:
        env.update_data((np.array(state), reward))
        if PREDICT:
            print(f"[PREDICT MODE] state = {state}")
            action, _ = model.predict(np.array(state), deterministic=True)
            print(f"[PREDICT MODE] action = {action}")
            return action
        print(f"[TRAIN MODE] state = {state}")
        condition.notify()
        condition.wait()
        return env.get_action()

def agent_loop():
    """Main training loop that runs in a separate thread."""
    global model, condition, callback_class
    with condition:
        model.learn(total_timesteps=total_timesteps, reset_num_timesteps=False, callback=callback_class(verbose=1))

def is_agent_thread_running():
    """Check if the training thread is currently active."""
    global agent_thread
    return agent_thread is not None and agent_thread.is_alive()

@app.post("/configure")
async def configure(req: ConfigureRequest):
    """Configure the RL model, environment, and training parameters."""
    global model_type, model, env_type, env, condition, agent_thread, callback_class
    global steps_counter, csv_index
    global transition_mode, transition_steps_remaining, transition_action

    model_type = req.model
    num_ues = req.num_ues
    new_env_type = req.env_type

    print(f"MODEL TYPE: {model_type}")
    print(f"Configuring model: {model_type} with {num_ues} UE(s)")
    
    # Check if training is already running and should continue
    if is_agent_thread_running() and CONTINUE_TRAINING:
        print(f"Active training thread detected")
        
        # Check if configuration is compatible with current training
        if (env is not None and 
            env_type == new_env_type and 
            hasattr(env, 'get_num_ues') and 
            env.get_num_ues() == num_ues and
            model_type == "PPO"):
            
            print(f"Compatible configuration - continuing existing training")
            print(f"Current state: steps_counter={steps_counter}, csv_index={csv_index}")
            
            # Activate transition mode with neutral action for 20 steps
            transition_mode = True
            transition_steps_remaining = 20
            transition_action = {
                "v": 0,
                "wq": [0 for _ in range(num_ues)],
                "wg": [0 for _ in range(num_ues)]
            }
            print(f"ACTIVATING TRANSITION MODE: 20 steps with action {transition_action}")
            
            return {
                "status": "success", 
                "message": f"Training continued from step {steps_counter} with 20-step transition",
                "training_active": True,
                "current_step": steps_counter,
                "transition_mode": True,
                "transition_steps": 20
            }
        else:
            print(f"Incompatible configuration - training restart required")
            print(f"   Env type: {env_type} -> {new_env_type}")
            print(f"   UE count: {env.get_num_ues() if env else 'None'} -> {num_ues}")
            print(f"   Model: {model_type}")
            
            transition_mode = False
            transition_steps_remaining = 0
            transition_action = None
    
    env_type = new_env_type
    
    # Reset training counters
    csv_index = 0
    steps_counter = 0
    print(f"Starting new training: csv_index={csv_index}, steps_counter={steps_counter}")
    
    # Check if we can reuse existing environment
    if (env is not None and 
        hasattr(env, 'get_num_ues') and 
        env.get_num_ues() == num_ues):
        print(f"Reusing existing environment ({env_type})")
    else:
        print(f"Creating new environment ({env_type})")
        
        # Create new environment based on model type
        if model_type == "PPO":
            condition = Condition()
            if env_type == "ISAC":
                env = CustomEnvISAC(num_ues, condition)
                callback_class = MeanRewardLogCallbackISAC
                print("Using ISAC environment")
            elif env_type == "Basic":
                env = CustomEnvBasic(num_ues, condition)
                callback_class = MeanRewardLogCallback
                print("Using basic environment")
            else:
                print(f"Invalid env_type: {env_type}")
                return {"status": "error", "message": f"Invalid env_type: {env_type}"}
        elif model_type == "DQN":
            condition = Condition()
            if env_type == "ISAC":
                env = CustomEnvDQN(num_ues, condition)
                callback_class = MeanRewardLogCallbackDQN
                print("Using DQN ISAC environment")
            elif env_type == "Basic":
                env = CustomEnvDQNBasic(num_ues, condition)
                callback_class = MeanRewardLogCallbackDQNBasic
                print("Using DQN Basic environment")
        else:
            return {"status": "error", "message": f"Invalid model_type: {model_type}"}
    
    # Create CSV file for logging learning curve
    csv_filename = f'learning_curve_{env_type}.csv'
    with open(csv_filename, 'w') as f:
        f.write('index,step,reward\n')
    print(f"Created new CSV file: {csv_filename}")

    if model_type == "PPO" or model_type == "DQN":
        # Reuse model if it exists and is compatible (only if no training is active)
        if (model is not None and 
            hasattr(model, 'env') and 
            not is_agent_thread_running()):
            print(f"Reusing existing model")
            model.env = env
        elif not is_agent_thread_running():  # Only create new model if no training is active
            print(f"Creating new model")
            
            # Load pretrained model if LOAD_MODEL is True
            if LOAD_MODEL:
                if env_type == "Basic":
                    pretrained_model_path = f"./dqn_model_20k_steps_Basic_TNSM26_29_01"
                elif env_type == "ISAC":
                    if model_type == "DQN":
                        pretrained_model_path = f"./saved_models/dqn_model_20k_steps_ISAC_sample"
                    elif model_type == "PPO":
                        pretrained_model_path = f"./saved_models/ppo_model_20k_steps_ISAC_sample"
                print(f"Attempting to load pretrained model from: {pretrained_model_path}")
                try:
                    if model_type == "PPO":
                        model = PPO.load(pretrained_model_path, env=env)
                    elif model_type == "DQN":
                        model = DQN.load(pretrained_model_path, env=env)
                    print(f"Pretrained model loaded from: {pretrained_model_path}")
                    
                    # Check observation space compatibility
                    if hasattr(model, 'observation_space'):
                        obs_shape = model.observation_space.shape
                        obs_size = obs_shape[0] if len(obs_shape) > 0 else 0
                        print(f"✓ Model observation space: {model.observation_space}")
                        print(f"✓ Expected state size: {obs_size} dimensions")
                        
                        # Calculate expected number of UEs
                        if env_type == "Basic":
                            expected_ues = obs_size // 3
                            print(f"✓ Model trained with: {expected_ues} UE(s) (3 features per UE)")
                        elif env_type == "ISAC":
                            expected_ues = obs_size // 4
                            print(f"✓ Model trained with: {expected_ues} UE(s) (4 features per UE)")
                    
                    if not PREDICT:
                        agent_thread = Thread(target=agent_loop, daemon=True)
                        agent_thread.start()
                    return {"status": "success", "message": "Pretrained model loaded"}
                except Exception as e:
                    print(f"Error loading pretrained model: {e}")
                    print("Proceeding with training from scratch...")
            
            # Create new model
            if model_type == "DQN":
                model = DQN(
                    "MlpPolicy", 
                    env, 
                    learning_rate=1e-4,
                    buffer_size=50000,
                    learning_starts=1000,
                    batch_size=64,
                    tau=1.0,
                    gamma=0.99,
                    train_freq=4,
                    gradient_steps=1,
                    target_update_interval=1000,
                    exploration_fraction=0.1,
                    exploration_initial_eps=1.0,
                    exploration_final_eps=0.02,
                    verbose=0
                )
                print("DQN model created")
            elif use_optimized_hyperparameters and model_type == "PPO":
                # Try to load optimized hyperparameters from Optuna tuning
                try:
                    best_params_file = "./optuna_results/icc_params.json"
                    if os.path.exists(best_params_file):
                        with open(best_params_file, 'r') as f:
                            best_params = json.load(f)
                        
                        policy_kwargs = best_params.pop("policy_kwargs", {})

                        if "activation_fn" in best_params:
                            best_params.pop("activation_fn")

                        if "activation_fn" in policy_kwargs:
                            act_fn_name = policy_kwargs["activation_fn"]
                            if act_fn_name == "tanh":
                                from torch.nn import Tanh
                                policy_kwargs["activation_fn"] = Tanh
                            elif act_fn_name == "relu":
                                from torch.nn import ReLU
                                policy_kwargs["activation_fn"] = ReLU
                            elif act_fn_name == "elu":
                                from torch.nn import ELU
                                policy_kwargs["activation_fn"] = ELU
                        
                        model = PPO("MlpPolicy", env, policy_kwargs=policy_kwargs, **best_params, verbose=0)
                        print("Model created with optimized hyperparameters")
                    else:
                        model = PPO("MlpPolicy", env, n_steps=n_steps, batch_size=batch_size, verbose=0)
                        print("Model created with default parameters")
                except Exception as e:
                    print(f"Error with optimized hyperparameters: {e}")
                    model = PPO("MlpPolicy", env, n_steps=n_steps, batch_size=batch_size, verbose=0)
            else:
                model = PPO("MlpPolicy", env, n_steps=n_steps, batch_size=batch_size, verbose=0)
        
        # Start training thread if not in prediction mode and not already running
        if not PREDICT and not is_agent_thread_running():
            agent_thread = Thread(target=agent_loop, daemon=True)
            agent_thread.start()
            print(f"Training thread initialized (new)")
        elif is_agent_thread_running():
            print(f"Training thread already active — no new thread started")

    elif model_type in ["Random", "Static"]:
        print(f"Using {model_type.lower()} model")

    # Reset transition mode if not continuing training
    if not (is_agent_thread_running() and CONTINUE_TRAINING):
        transition_mode = False
        transition_steps_remaining = 0
        transition_action = None
    
    # Prepare final status message
    training_status = "active" if is_agent_thread_running() else "initialized"
    status_message = f"Training {training_status} since step {steps_counter}"
    
    return {
        "status": "success", 
        "message": status_message,
        "training_active": is_agent_thread_running(),
        "current_step": steps_counter,
        "transition_mode": transition_mode
    }

@app.post("/infer_sched_config")
async def infer_sched_config(req: InferRequest):
    """Main inference endpoint - receives state and reward, returns scheduling configuration action."""
    global model_type, env_type
    global steps_counter, csv_index, pending_reward
    global transition_mode, transition_steps_remaining, transition_action
    
    print(f"[DEBUG] env_type: {env_type}")
    print(f"[DEBUG] Step {steps_counter} -> Received state: {req.state}")
    print(f"[DEBUG] Step {steps_counter} -> Received reward: {req.reward}")

    # Detect number of connected UEs
    num_connected_ues = len([mcs for mcs in req.state["mcs"] if mcs > 0])
    sum_mcs = sum(req.state["mcs"])
    print(f"[DEBUG] Step {steps_counter} -> Connected UEs: {num_connected_ues} and sum_mcs={sum_mcs}")
    
    # Hardcoded swap for testing
    # aux = req.state["mcs"][3]
    # req.state["mcs"][3] = req.state["mcs"][0]
    # req.state["mcs"][0] = aux
    # print(f"[DEBUG] Step {steps_counter} -> hardcoded state mcs swap: {req.state['mcs']}")

    # Extract state components
    mcs_values = np.array(req.state["mcs"], dtype=np.int32)
    q_values = np.array(req.state["q"], dtype=np.int32)
    g_values = np.array(req.state["g"], dtype=np.int32)
    obs_values = np.array(req.state["closeToObstacle"], dtype=np.int32)
    
    # Reset auxiliary values to zero for testing
    # obs_values = np.zeros(len(mcs_values), dtype=np.int32)
    # q_values = np.zeros(len(mcs_values), dtype=np.int32)
    # g_values = np.zeros(len(mcs_values), dtype=np.int32)
    
    # Construct state based on environment type
    if env_type == "ISAC":
        state = np.concatenate([mcs_values, q_values, g_values, obs_values])
        # Testing code - manipulate MCS based on obstacle proximity
        # num_elements = min(len(mcs_values), len(obs_values))
        # mcs_values[:num_elements] = 1 - obs_values[:num_elements]
    elif env_type == "Basic":
        state = np.concatenate([mcs_values, q_values, g_values])
    else:
        print(f"Invalid env_type: {env_type}")
        state = "<invalid>"
    
    # Use pending reward if available (from evaluation mode)
    if pending_reward is not None:
        print(f"[PENDING REWARD] Using pending reward: {pending_reward} instead of current reward: {req.reward} in step {steps_counter}")
        reward = pending_reward
    else:
        reward = req.reward
    
    steps_counter += 1
    
    # Handle transition mode (used when continuing training with compatible config)
    if transition_mode:
        transition_steps_remaining -= 1
        print(f"[TRANSITION] Step {steps_counter}: Returning static action {transition_action} ({transition_steps_remaining} steps remaining)")
        
        with open(f'learning_curve_{env_type}.csv', 'a', newline='') as f:
            writer = csv.writer(f)
            writer.writerow([csv_index, steps_counter, reward])
            csv_index += 1
        
        if transition_steps_remaining <= 0:
            transition_mode = False
            transition_action = None
            print(f"[TRANSITION] COMPLETED at step {steps_counter}. Resuming normal training.")
        
        if model_type == "PPO" and env is not None:
            num_ues = env.get_num_ues()
        else:
            num_ues = len(mcs_values)
        
        return {
            "v": 0,
            "wq": [0 for _ in range(num_ues)],
            "wg": [0 for _ in range(num_ues)]
        }
    
    # Process training step
    print(f'[TRAIN] state: {state}, reward: {reward}')
    pending_reward = None

    print(f"MODEL TYPE: {model_type}")
    
    # Save model checkpoint at 20k steps
    if csv_index == 20_000 and model_type == "PPO":
        model_save_path = f"./saved_models/ppo_model_20k_steps_{env_type}.zip"
        os.makedirs("./saved_models", exist_ok=True)
        model.save(model_save_path)
        print(f"Model saved at {model_save_path} at 20,000 steps")
    
    if csv_index == 20_000 and model_type == "DQN":
        model_save_path = f"./saved_models/dqn_model_20k_steps_{env_type}.zip"
        os.makedirs("./saved_models", exist_ok=True)
        model.save(model_save_path)
        print(f"Model saved at {model_save_path} at 20,000 steps")

    # Log reward to CSV
    with open(f'learning_curve_{env_type}.csv', 'a', newline='') as f:
        writer = csv.writer(f)
        writer.writerow([csv_index, steps_counter, reward])
        csv_index += 1

    # Generate action based on model type
    if model_type == "PPO":
        action = ppo(state, reward)
        print(f'Action: {action}')
        num_ues = env.get_num_ues()
        return {
            "v": int(action[0]),
            "wq": [int(x) for x in action[1 : 1 + num_ues]],
            "wg": [int(x) for x in action[1 + num_ues : 1 + 2 * num_ues]]
        }
    elif model_type == "DQN":
        print(f'[DQN] state: {state}, reward: {reward}')
        action = ppo(state, reward)  # Uses same function but DQN returns single action index
        print(f'DQN Action: {action}')
        
        if PREDICT:
            v, wq_list, wg_list = env.decode_action(action)
            print(f'DQN Full Action from env: v={v}, wq={wq_list}, wg={wg_list}')
            return {
                "v": int(v),
                "wq": [int(x) for x in wq_list],
                "wg": [int(x) for x in wg_list]
            } 

        action_full = env.get_action()
        print(f'DQN Full Action from env: {action_full}')
        
        if action_full is not None:
            num_ues = env.get_num_ues()
            return {
                "v": int(action_full[0]),
                "wq": [int(x) for x in action_full[1 : 1 + num_ues]],
                "wg": [int(x) for x in action_full[1 + num_ues : 1 + 2 * num_ues]]
            }
        else:
            num_ues = len(mcs_values)
            return {
                "v": 0,
                "wq": [0 for _ in range(num_ues)],
                "wg": [0 for _ in range(num_ues)]
            }
    elif model_type == "Random":
        return random_tuning(num_ues)
    elif model_type == "Static":
        return static_tuning(env_type, mcs_values[0])

@app.get("/training_status")
async def training_status():
    """Endpoint to check the current training status."""
    global steps_counter, csv_index, model, env_type, agent_thread
    global transition_mode, transition_steps_remaining
    
    return {
        "status": "active" if is_agent_thread_running() else "inactive",
        "env_type": env_type,
        "current_steps": steps_counter,
        "csv_index": csv_index,
        "model_loaded": model is not None,
        "thread_alive": is_agent_thread_running(),
        "thread_id": agent_thread.ident if is_agent_thread_running() else None,
        "transition_mode": transition_mode,
        "transition_steps_remaining": transition_steps_remaining
    }

@app.post("/stop_training")
async def stop_training():
    """Force stop the training process and save model checkpoint."""
    global agent_thread, model, env_type, csv_index
    
    if is_agent_thread_running():
        # Save model before stopping
        if model is not None:
            try:
                model_save_path = f"./saved_models/{model_type}_model_stop_{csv_index}_steps_{env_type}.zip"
                os.makedirs("./saved_models", exist_ok=True)
                model.save(model_save_path)
                print(f"Model saved before stopping: {model_save_path}")
            except Exception as e:
                print(f"Error saving model: {e}")
        agent_thread = None
        
        return {"status": "success", "message": "Training stopped"}
    else:
        return {"status": "info", "message": "No training in progress to stop"}