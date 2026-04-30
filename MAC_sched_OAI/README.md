# OAI (+ FlexRIC) MAC AI Scheduler

This repository extends **OpenAirInterface (OAI)** with a **drift-plus-penalty MAC scheduler** and a **FlexRIC** xApp and extended **MAC E2 Service Model** to connect to external AI modules, enabling **dynamic reconfiguration of the scheduler** based on real-time network conditions.

## Main Components

- **OAI:**
    
    - Contains a modified **MAC scheduler** implementing the **drift-plus-penalty** algorithm (resource eficiency, throughput optimization and queue stability).
        
    - Includes new variables for collecting runtime statistics and scheduler-related metrics used for analysis or learning feedback.
        
- **FlexRIC:**
    
    - Integrates an **xApp** that acts as a _connector_ between the OAI testbed and an **external AI module**.
        
    - The AI module trains an agent capable of inferring suitable scheduling weights.
        
    - The **MAC E2 Service Model** has been extended to support additional subscription and control functionality:
        
        - The xApp can subscribe to relevant RAN data and provide it to the AI module.
            
        - The xApp can compute and report a reward associated with the agent's performance.
            
        - The xApp delivers the updated scheduling weights back to the MAC scheduler via the FlexRIC E2 interface.
            

## AI Module

- The xApp communicates with the external AI module via **REST** API.

- AI module training can be performed using ns-3 simulations (see [RBIS repo](https://github.com/tlmat-unican/RBIS)).  

## Repository Structure

```
oai-mac-ai-scheduler/
├── openairinterface5g/
│   ├── openair2/LAYER2/NR_MAC_gNB/
│   │    └─── gNB_scheduler_dlsch.c                 # DPP MAC scheduler implementation
│   └── openair2/E2AP/
│        ├── flexric/
│        │     ├── examples/xApp/python3/
│        │     │    └─── xapp_drl_scheduling.py     # Custom xApp (RAN-AI connector)
│        │     └─── src/sm/mac_sm/                  # Extended MAC Service Model
│        └── RAN_FUNCTION/CUSTOMIZED/
│             └─── ran_func_mac.c                   # E2-MAC interface
├── env/                                            # DRL environments
└─── server_drl.py                                   # REST API server (AI module)
```

## Build and Run

1. **Clone the repository**

```bash
git clone https://github.com/tlmat-unican/oai-mac-ai-scheduler.git
cd oai-mac-ai-scheduler
```

2. **Setup OAI RAN + FlexRIC + Core**

See [References](#references) for complete installation.

3. **Enable dynamic AI reconfiguration (optional)**

    **3.1 Start AI Module (DRL server)**

    ```bash
    uvicorn server_drl:app --host 0.0.0.0 --port 8000
    ```

    **3.2 Run xApp**

    ```bash
    export PYTHONPATH=openairinterface5g/openair2/E2AP/flexric/build/src/xApp/swig:$PYTHONPATH
    python3 openairinterface5g/openair2/E2AP/flexric/examples/xApp/python3/xapp_drl_scheduling.py
    ```

4. **Plot collected metrics**

Open: ``openairinterface5g/figs/plot_metrics.ipynb`` in Jupyter to visualize results.

## References

- [OpenAirInterface (OAI)](https://openairinterface.org/)

- [FlexRIC](https://gitlab.eurecom.fr/mosaic5g/flexric)
