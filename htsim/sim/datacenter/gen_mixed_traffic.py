#!/usr/bin/env python3
"""
Generate connection matrices for the mixed NSCC + TCP Cubic experiment.

Produces 3 scenarios:
  A) Permutation with uniform 2MB flows (reuses existing if available)
  B) Random permutation with log-uniform flow sizes (10KB - 10MB)
  C) 16-to-1 incast + random background traffic

Usage:
  python3 gen_mixed_traffic.py [--nodes 128] [--outdir connection_matrices]
"""

import argparse
import random
import math
import os


def write_cm(filepath, nodes, connections):
    """Write a connection matrix file.

    connections: list of (src, dst, start_us, size_bytes, flow_id)
    """
    with open(filepath, 'w') as f:
        f.write(f"Nodes {nodes}\n")
        f.write(f"Connections {len(connections)}\n")
        for src, dst, start_us, size_bytes, fid in connections:
            f.write(f"{src}->{dst} id {fid} start {start_us} size {size_bytes}\n")
    print(f"  Written {len(connections)} connections to {filepath}")


def gen_permutation_uniform(nodes, flow_size, seed=42):
    """Scenario A: Random 1-to-1 permutation, uniform flow size."""
    random.seed(seed)
    dests = list(range(nodes))
    random.shuffle(dests)
    # Avoid self-loops
    for i in range(nodes):
        if dests[i] == i:
            j = (i + 1) % nodes
            dests[i], dests[j] = dests[j], dests[i]

    conns = []
    for i in range(nodes):
        conns.append((i, dests[i], 0, flow_size, i + 1))
    return conns


def gen_mixed_sizes(nodes, num_conns, min_size, max_size, seed=42):
    """Scenario B: Random src/dst with log-uniform flow sizes."""
    random.seed(seed)
    conns = []
    log_min = math.log(min_size)
    log_max = math.log(max_size)
    for fid in range(1, num_conns + 1):
        src = random.randint(0, nodes - 1)
        dst = random.randint(0, nodes - 2)
        if dst >= src:
            dst += 1
        size = int(math.exp(random.uniform(log_min, log_max)))
        conns.append((src, dst, 0, size, fid))
    return conns


def gen_incast_plus_background(nodes, incast_degree, incast_target,
                               incast_size, bg_conns, bg_size, seed=42):
    """Scenario C: incast_degree-to-1 incast + random background.

    incast_degree senders all target incast_target node.
    bg_conns random background flows.
    """
    random.seed(seed)
    conns = []
    fid = 1

    # Incast flows: pick incast_degree random senders
    senders = random.sample([n for n in range(nodes) if n != incast_target],
                            incast_degree)
    for s in senders:
        conns.append((s, incast_target, 0, incast_size, fid))
        fid += 1

    # Background flows: random pairs, start at time 0
    for _ in range(bg_conns):
        src = random.randint(0, nodes - 1)
        dst = random.randint(0, nodes - 2)
        if dst >= src:
            dst += 1
        conns.append((src, dst, 0, bg_size, fid))
        fid += 1

    return conns


def main():
    parser = argparse.ArgumentParser(description="Generate mixed experiment traffic matrices")
    parser.add_argument("--nodes", type=int, default=128, help="Number of nodes")
    parser.add_argument("--outdir", type=str, default="connection_matrices",
                        help="Output directory")
    parser.add_argument("--seed", type=int, default=42, help="Random seed")
    args = parser.parse_args()

    nodes = args.nodes
    outdir = args.outdir
    os.makedirs(outdir, exist_ok=True)

    print(f"Generating traffic matrices for {nodes} nodes...")

    # Scenario A: Permutation, 2MB uniform
    conns_a = gen_permutation_uniform(nodes, flow_size=2_000_000, seed=args.seed)
    write_cm(os.path.join(outdir, f"mixed_scenA_perm_{nodes}n_2MB.cm"),
             nodes, conns_a)

    # Scenario B: Mixed sizes, log-uniform 10KB-10MB, 256 flows
    conns_b = gen_mixed_sizes(nodes, num_conns=256,
                              min_size=10_000, max_size=10_000_000,
                              seed=args.seed + 1)
    write_cm(os.path.join(outdir, f"mixed_scenB_mixed_{nodes}n_256c.cm"),
             nodes, conns_b)

    # Scenario C: 16-to-1 incast + 64 background flows
    conns_c = gen_incast_plus_background(
        nodes,
        incast_degree=16, incast_target=0,
        incast_size=1_000_000,
        bg_conns=64, bg_size=2_000_000,
        seed=args.seed + 2)
    write_cm(os.path.join(outdir, f"mixed_scenC_incast_{nodes}n.cm"),
             nodes, conns_c)

    print("Done generating traffic matrices.")


if __name__ == "__main__":
    main()
