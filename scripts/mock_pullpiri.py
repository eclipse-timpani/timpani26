# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT
"""
Pullpiri mock: AddSchedInfo gRPC call to timpani-o SchedInfoService.

Usage (from TIMPANI root):
    .venv/bin/python scripts/mock_pullpiri.py [--host HOST] [--port PORT]
        [--config scripts/mock_pullpiri.yaml]

Default: localhost:50052
"""

import sys
import os
import argparse
import socket

try:
    import yaml
except ImportError as exc:
    raise SystemExit(
        "PyYAML is required. Install it with '.venv/bin/pip install pyyaml'."
    ) from exc

# Generated stubs live in scripts/
sys.path.insert(0, os.path.dirname(__file__))
import schedinfo_pb2
import schedinfo_pb2_grpc
import grpc


DEFAULT_CONFIG_PATH = os.path.join(os.path.dirname(__file__), "mock_pullpiri.yaml")


def parse_sched_policy(policy_name: str) -> int:
    policy_map = {
        "NORMAL": schedinfo_pb2.NORMAL,
        "FIFO": schedinfo_pb2.FIFO,
        "RR": schedinfo_pb2.RR,
    }
    try:
        return policy_map[policy_name.upper()]
    except KeyError as exc:
        raise ValueError(f"Unsupported policy '{policy_name}'") from exc


def parse_temporal_class(class_name: str) -> int:
    if not hasattr(schedinfo_pb2, "TEMPORAL_PERIODIC"):
        raise ValueError(
            "temporal_class is not available in the generated schedinfo_pb2 stubs"
        )

    class_map = {
        "TEMPORAL_PERIODIC": schedinfo_pb2.TEMPORAL_PERIODIC,
        "TEMPORAL_SPORADIC": schedinfo_pb2.TEMPORAL_SPORADIC,
    }
    try:
        return class_map[class_name.upper()]
    except KeyError as exc:
        raise ValueError(f"Unsupported temporal_class '{class_name}'") from exc


def load_config(config_path: str) -> dict:
    with open(config_path, "r", encoding="utf-8") as config_file:
        config = yaml.safe_load(config_file) or {}

    workloads = config.get("workloads")
    if not isinstance(workloads, list) or not workloads:
        raise ValueError("Config must contain a non-empty 'workloads' list")

    return config


def build_request(workload_cfg: dict, default_node_id: str) -> schedinfo_pb2.SchedInfo:
    req = schedinfo_pb2.SchedInfo()
    req.workload_id = workload_cfg["workload_id"]

    if hasattr(req, "temporal_class"):
        req.temporal_class = parse_temporal_class(
            workload_cfg.get("temporal_class", "TEMPORAL_PERIODIC")
        )

    for task_cfg in workload_cfg.get("tasks", []):
        task = req.tasks.add()
        task.name = task_cfg["name"]
        task.priority = int(task_cfg["priority"])
        task.policy = parse_sched_policy(task_cfg.get("policy", "FIFO"))
        task.cpu_affinity = int(task_cfg.get("cpu_affinity", 0), 0) \
            if isinstance(task_cfg.get("cpu_affinity", 0), str) \
            else int(task_cfg.get("cpu_affinity", 0))
        task.period = int(task_cfg["period"])
        task.release_time = int(task_cfg.get("release_time", 0))
        task.runtime = int(task_cfg["runtime"])
        task.deadline = int(task_cfg["deadline"])
        task.node_id = task_cfg.get("node_id", default_node_id)
        task.max_dmiss = int(task_cfg.get("max_dmiss", 0))

    return req


def main():
    parser = argparse.ArgumentParser(description="Pullpiri mock — AddSchedInfo")
    parser.add_argument("--host", default="localhost", help="timpani-o host")
    parser.add_argument("--port", type=int, default=50052, help="SchedInfoService port")
    parser.add_argument(
        "--config",
        default=DEFAULT_CONFIG_PATH,
        help="YAML config file describing one or more workloads",
    )
    args = parser.parse_args()

    addr = f"{args.host}:{args.port}"
    print(f"[mock_pullpiri] Connecting to {addr} ...")

    channel = grpc.insecure_channel(addr)
    stub = schedinfo_pb2_grpc.SchedInfoServiceStub(channel)

    default_node_id = socket.gethostname()
    print(f"[mock_pullpiri] node_id: '{default_node_id}'")

    try:
        config = load_config(args.config)
        for workload_cfg in config["workloads"]:
            req = build_request(workload_cfg, default_node_id)
            print(
                f"[mock_pullpiri] Sending AddSchedInfo: workload_id='{req.workload_id}' "
                f"tasks={[t.name for t in req.tasks]}"
            )
            resp = stub.AddSchedInfo(req, timeout=5)
            if resp.status == 0:
                print(f"[mock_pullpiri] ✓ Success for '{req.workload_id}' (status=0)")
            else:
                print(
                    f"[mock_pullpiri] ✗ Failed for '{req.workload_id}' (status={resp.status})",
                    file=sys.stderr,
                )
                sys.exit(1)
    except (ValueError, OSError) as e:
        print(f"[mock_pullpiri] ✗ Config error: {e}", file=sys.stderr)
        sys.exit(1)
    except grpc.RpcError as e:
        print(f"[mock_pullpiri] ✗ gRPC error: {e.code()} — {e.details()}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

