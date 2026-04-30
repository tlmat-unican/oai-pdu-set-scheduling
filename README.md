# OpenAirInterface 5G NR Testbed

## Overview

This repository documents a research testbed built around OpenAirInterface for evaluating PDU set-aware MAC scheduling algorithms. The repository is intended for reproducible experimentation with multi-UE RF simulator deployments, custom MAC schedulers, and optional video-quality evaluation workflows.

## Features

- 5G NR SA deployment with OpenAirInterface gNB and nrUE
- Multi-UE execution using Linux network namespaces
- RF simulator-based experiments
- Custom MAC scheduler integration, including baseline and research schedulers
- Optional XR/video traffic generation and evaluation pipeline
- Reproducible launch scripts for testbed execution

## Requirements

The following components are recommended:

- OpenAirInterface (gNB and nrUE)
- Open5GS Core, although other 5G cores could also be used
- `iperf3` for throughput and background traffic testing
- `ffmpeg` for media transmission and processing
- `python3` for the RTP proxy
- VMAF: `ffmpeg-git-20240629-amd64-static` (download from [johnvansickle.com](https://johnvansickle.com/ffmpeg/))
- A `.sdp` session description file matching the video stream RTP ports must match the proxy (`50002` for RX by default)
- A frame metadata `.csv` generated from the reference video using `ffprobe`:

```bash
ffprobe -v quiet -select_streams v:0 \
  -show_entries frame=pkt_pts_time,pkt_size,pict_type \
  -of csv=p=0 video_ref_30M.mp4 > ffprobe_data_30M.csv
```

## Installation process

The exact installation commands may vary depending on the branch, OAI version, and host setup. The process below is intended as a practical high-level guide that can be adapted to the local environment.

### 1. Install Open5GS

Any 5G SA-compatible core could be used, but this testbed was designed with Open5GS.

The Open5GS source code is not included in this repository. Please follow the official quickstart guide:

[Open5GS Quickstart](https://open5gs.org/open5gs/docs/guide/01-quickstart/)

After installation, subscriber profiles should be registered for all UEs used in the experiments. In Open5GS, this is typically done through the WebUI, where each subscriber entry includes the IMSI and security parameters such as K, OPc, and AMF. These values must match the credentials configured in the OAI `nrUE` instances.

### 2. Install OpenAirInterface

Follow the official OAI tutorial and build guide:

[OAI Tutorial](https://gitlab.eurecom.fr/oai/openairinterface5g/-/blob/develop/doc/NR_SA_Tutorial_OAI_nrUE.md)

A typical build process is:

```bash
cd openairinterface5g
source oaienv
cd cmake_targets
./build_oai -I
./build_oai --gNB --nrUE --ninja
```

The generated binaries are typically available under:

```bash
cmake_targets/ran_build/build/
```

When using RF simulator mode, OAI runs the gNB and `nrUE` with the `--rfsim` option.

### 3. Run

A typical workflow is:

1. Start Open5GS services.
2. Launch the OAI gNB.
3. Launch one or more `nrUE` instances.
4. Attach each UE to its corresponding Linux network namespace.
5. Start the RTP proxy.
6. Start traffic generators or media streams.

In this repository, the process is automated through helper scripts.

First, launch OAI:

```bash
./scripts/launch_oai.sh
```

Then run the test script with the reference video:

```bash
sudo ./run_test.sh <video.mp4>
```

This setup creates two UEs:
- UE1 is used for video transmission and reception
- UE2 is used to generate background traffic with `iperf3`

The multi-UE namespace configuration is handled by the repository scripts, including the namespace setup used to isolate traffic flows per UE.

During execution:
- the reference video must already be downloaded locally,
- the RTP proxy is started automatically,
- the received stream is recorded,
- VMAF is computed against the reference video,
- and the results are stored in `vmaf_results.txt`.

## Notes

- This repository does not include Open5GS source code.
- `iperf3`, `ffmpeg`, and VMAF-related tools are only needed for evaluation workflows and are not strictly required for basic OAI/Open5GS connectivity tests.


> **Work in progress**. This README is under construction.
> For questions or issues, feel free to reach out: [neco.villegas@unican.es](mailto:neco.villegas@unican.es)