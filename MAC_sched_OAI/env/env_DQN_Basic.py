import gymnasium as gym
from gymnasium import spaces
import numpy as np
from stable_baselines3.common.callbacks import BaseCallback

class CustomEnvDQN(gym.Env):
    """
    Environment compatible with DQN (discrete action space).
    """
    STATE_SPACE = 3
    """Number of components in the state per UE: MCS, Q, G
    """
    NUM_SETTINGS = 3
    """Number of settings that are to be set up by the environment.
    Effectively defines the size of the action space.
    """
    MCS_STATES = 3
    """Number of MCS states recognized. Assumed to be 0-indexed.
    """
    V_STATES = 4
    """Number of values for V, 0-indexed.
    """
    WQ_STATES = 1
    """Values for w_q in a list.
    """
    WG_STATES = 2
    """Values for w_g in a list.
    """

    # Ranges for state components
    MCS_MIN, MCS_MAX = 0, 2          # MCS: [0, 1, 2]
    QUEUE_MIN, QUEUE_MAX = 0, 2      # Queue: [0, 1, 2] 
    OBSTACLE_MIN, OBSTACLE_MAX = 0, 1 # Obstacle: [0, 1] (binary)

    def __init__(self, num_ues, condition):
        super(CustomEnvDQN, self).__init__()

        self.num_ues = num_ues
        self.condition = condition
        self.state = np.zeros(self.STATE_SPACE * num_ues, dtype=np.int32)
        self.reward = 0
        self.action = None

        # Observation space
        low_bounds = np.array(
            [self.MCS_MIN] * num_ues +
            [self.QUEUE_MIN] * num_ues +
            [self.QUEUE_MIN] * num_ues
        )
        high_bounds = np.array(
            [self.MCS_MAX] * num_ues +
            [self.QUEUE_MAX] * num_ues +
            [self.QUEUE_MAX] * num_ues
        )

        self.observation_space = spaces.Box(
            low=low_bounds, 
            high=high_bounds, 
            shape=(self.STATE_SPACE * num_ues,), 
            dtype=np.int32
        )

        # Action space: v × wq_combinations × wg_combinations
        total_wq_combinations = self.WQ_STATES ** num_ues
        total_wg_combinations = self.WG_STATES ** num_ues
        self.action_space = spaces.Discrete(
            self.V_STATES * total_wq_combinations * total_wg_combinations
        )
        
        print(f"Action space size: {self.action_space.n} "
              f"(v={self.V_STATES} × wq={total_wq_combinations} × wg={total_wg_combinations})")

    def decode_action(self, action_idx):
        """
        Decodifica acción a (v, [wq0, wq1, ...], [wg0, wg1, ...])
        """
        total_wq_combinations = self.WQ_STATES ** self.num_ues
        total_wg_combinations = self.WG_STATES ** self.num_ues
        
        v = action_idx // (total_wq_combinations * total_wg_combinations)
        remainder = action_idx % (total_wq_combinations * total_wg_combinations)
        
        wq_combination_idx = remainder // total_wg_combinations
        wq_list = []
        temp = wq_combination_idx
        for _ in range(self.num_ues):
            wq_list.append(temp % self.WQ_STATES)
            temp //= self.WQ_STATES
        wq_list = wq_list[::-1]
        
        wg_combination_idx = remainder % total_wg_combinations
        wg_list = []
        temp = wg_combination_idx
        for _ in range(self.num_ues):
            wg_list.append(temp % self.WG_STATES)
            temp //= self.WG_STATES
        wg_list = wg_list[::-1]
        
        return int(v), [int(wq) for wq in wq_list], [int(wg) for wg in wg_list]

    def get_num_ues(self):
        """Return the number of UEs in this environment"""
        return self.num_ues
    
    def update_data(self, data):
        """Update state and reward from external source (NS-3 or training_client)"""
        self.state, self.reward = data 
    
    def _get_obs(self) -> np.array:
        return self.state
    
    def _get_reward(self) -> float:
        return self.reward

    def get_action(self):
        """
        Return the last action taken (converted to expected format).

        Formato esperado: [v, wq0, wq1, wq2, wq3, wg0, wg1, wg2, wg3]
        - v: global value para todos los UEs
        - wq_i: specific value for w_q for UE i
        - wg_i: specific value for w_g for UE i
        """
        if self.action is None:
            return None
        
        # Decode action to get v, wq_list, wg_list
        v, wg_list, wq_list = self.decode_action(self.action)
        
        # Format: [v, wg0, wg1, wg2, wg3]
        action_array = [v]
        for i in range(self.num_ues):
            action_array.append(wg_list[i]) # wg
        for i in range(self.num_ues):
            action_array.append(wq_list[i]) # wq
        
        return np.array(action_array)

    def reset(self, seed=None, options=None):
        """Reset the environment"""
        if seed is not None:
            np.random.seed(seed)
        
        self.current_idx = 0
        self.action = None
        state = self._get_obs()
        return state, {}

    def step(self, action):
        """
        Execute one step in the environment.
        
        Args:
            action: int en [0, V_STATES × WG_STATES^num_ues - 1]
        
        Returns:
            tuple: (next_state, reward, done, truncated, info)
        """
        # Store the action for external use (e.g., NS-3 or training_client)
        self.action = action
        
        # Wait until the action is processed by the external system before returning the next state and reward
        self.condition.wait()
        
        next_state = self._get_obs()
        reward = self._get_reward()
        done = False
        truncated = False
        
        # Additional info with decoded action
        v, wg_list, wq_list = self.decode_action(action)
        info = {
            'v': v,
            'wq': wq_list,
            'wg': wg_list,
            'action_idx': action
        }
        
        # Notify that the state has been processed
        self.condition.notify()

        return next_state, reward, done, truncated, info

    def close(self):
        """Clean up resources"""
        pass


class MeanRewardLogCallbackDQN(BaseCallback):
    """
    Callback for logging during training with DQN.
    """
    def __init__(self, verbose=0, log_interval=100):
        super(MeanRewardLogCallbackDQN, self).__init__(verbose)
        self.log_interval = log_interval
        self.episode_rewards = []
        self.episode_lengths = []
        self.current_episode_reward = 0
        self.current_episode_length = 0

    def _on_step(self) -> bool:
        """
        Called after each step during training.
        """
        # Accumulate reward from the current step
        reward = self.locals.get('rewards', [0])[0]
        self.current_episode_reward += reward
        self.current_episode_length += 1

        # Check if the episode has ended
        done = self.locals.get('dones', [False])[0]
        if done:
            self.episode_rewards.append(self.current_episode_reward)
            self.episode_lengths.append(self.current_episode_length)
            self.current_episode_reward = 0
            self.current_episode_length = 0

        # Logging periódico
        if self.n_calls % self.log_interval == 0:
            if len(self.episode_rewards) > 0:
                mean_reward = np.mean(self.episode_rewards[-100:])
                mean_length = np.mean(self.episode_lengths[-100:])
                
                if self.verbose > 0:
                    print(f"Step: {self.n_calls}, "
                          f"Mean Reward (last 100 eps): {mean_reward:.2f}, "
                          f"Mean Length: {mean_length:.1f}")

        return True

    def _on_training_end(self) -> None:
        """Se llama al final del entrenamiento"""
        if self.verbose > 0:
            print(f"\nEntrenamiento completado!")
            print(f"Total episodes: {len(self.episode_rewards)}")
            if len(self.episode_rewards) > 0:
                print(f"Mean reward: {np.mean(self.episode_rewards):.2f}")
