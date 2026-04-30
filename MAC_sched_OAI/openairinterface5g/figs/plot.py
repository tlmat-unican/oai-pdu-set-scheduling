import pandas as pd
import matplotlib.pyplot as plt

# Load the CSV file into a DataFrame
file_path = '~/MAC_sched_OAI/openairinterface5g/cmake_targets/ran_build/build/ue_qos_stats.csv'
df = pd.read_csv(file_path)

# Ensure the required columns exist
if 'UID' not in df.columns or 'Current_throughput' not in df.columns:
    raise ValueError("The CSV file must contain 'UID' and 'Current_throughput' columns.")

# Group data by 'UID' and plot each UE's throughput
plt.figure(figsize=(10, 6))
for uid, group in df.groupby('UID'):
    plt.plot(group.index, group['Current_throughput'], label=f'UE {uid+1}')

# Add labels, legend, and title
plt.xlabel('Av. Window (2s)')
plt.ylabel('Av. Throughput (Mbps)')
plt.legend()
plt.grid(True)

# Show the plot
plt.tight_layout()
plt.savefig('thput.png')

############################

# Plot the stability of Q for each UE
plt.figure(figsize=(10, 6))
for uid, group in df.groupby('UID'):
    # Get unique timestamps and aggregate buffer bytes per timestamp
    unique_times = group.groupby('Time').agg({
        'Q': 'sum',  # Sum all LCID buffers per timestamp
        'Current_throughput': 'first'  # Take first (they should be the same)
    }).reset_index()
    
    # Calculate relative time starting from 0
    if len(unique_times) > 0:
        start_time = unique_times['Time'].iloc[0]
        unique_times['Time_ms'] = ((unique_times['Time'] - start_time) + 2) * 1000
        
        # Calculate the cumulative sum of the queue size in bits
        unique_times['Cumulative_bits'] = (unique_times['Q'] * 8).cumsum()
        
        # print(f"UE {uid}:")
        # print(unique_times[['Time_ms', 'Cumulative_bits']])

        # Calculate the stability (bits per millisecond)
        # Avoid division by zero
        unique_times['Stability'] = unique_times['Cumulative_bits'] / (unique_times['Time_ms'])
        
        # Plot the stability for the current UE
        plt.plot(unique_times['Time_ms'], unique_times['Stability'], label=f'UE {uid+1}')

        # print(f"UE {uid}:")
        # print(unique_times[['Time_ms', 'Cumulative_bits', 'Stability']])

# Add labels, legend, and title
plt.xlabel('Av. Window (2s)')
plt.ylabel('Q Stability')
plt.legend()
plt.grid(True)

# Show the plot
plt.tight_layout()
plt.savefig('Q_stability.png')

############################

# Plot the stability of G for each UE
plt.figure(figsize=(10, 6))
for uid, group in df.groupby('UID'):
    # Get unique timestamps and aggregate buffer bytes per timestamp
    unique_times = group.groupby('Time').agg({
        'G': 'sum',  # Sum all LCID buffers per timestamp
        'Current_throughput': 'first'  # Take first (they should be the same)
    }).reset_index()
    
    # Calculate relative time starting from 0
    if len(unique_times) > 0:
        start_time = unique_times['Time'].iloc[0]
        unique_times['Time_ms'] = ((unique_times['Time'] - start_time) + 2) * 1000
        
        # Calculate the cumulative sum of the queue size in bits
        unique_times['Cumulative_bits'] = (unique_times['G'] * 8).cumsum()
        
        # print(f"UE {uid}:")
        # print(unique_times[['Time_ms', 'Cumulative_bits']])

        # Calculate the stability (bits per millisecond)
        # Avoid division by zero
        unique_times['Stability'] = unique_times['Cumulative_bits'] / (unique_times['Time_ms'])
        
        # Plot the stability for the current UE
        plt.plot(unique_times['Time_ms'], unique_times['Stability'], label=f'UE {uid+1}')

        # print(f"UE {uid}:")
        # print(unique_times[['Time_ms', 'Cumulative_bits', 'Stability']])

# Add labels, legend, and title
plt.xlabel('Av. Window (2s)')
plt.ylabel('G Stability')
plt.legend()
plt.grid(True)

# Show the plot
plt.tight_layout()
plt.savefig('G_stability.png')

############################

# RBs allocated per UE
plt.figure(figsize=(10, 6))
for uid, group in df.groupby('UID'):
    plt.plot(group.index, group['RBs'], label=f'UE {uid+1}')
# Add labels, legend, and title
plt.xlabel('Av. Window (2s)')
plt.ylabel('RBs Allocated')
plt.legend()
plt.grid(True)
# Show the plot
plt.tight_layout()
plt.savefig('RBs_allocated.png')