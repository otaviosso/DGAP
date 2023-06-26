// Copyright (c) 2015, The Regents of the University of California (Regents)
// See LICENSE.txt for license details

#include <functional>
#include <iostream>
#include <vector>

#include "benchmark.h"
#include "bitmap.h"
#include "command_line.h"
#include "graph.h"
#include "platform_atomics.h"
#include "pvector.h"
#include "sliding_queue.h"


/*
GAP Benchmark Suite
Kernel: Betweenness Centrality (BC)
Author: Scott Beamer

Will return array of approx betweenness centrality scores for each vertex

This BC implementation makes use of the Brandes [1] algorithm with
implementation optimizations from Madduri et al. [2]. It is only an approximate
because it does not compute the paths from every start vertex, but only a small
subset of them. Additionally, the scores are normalized to the range [0,1].

As an optimization to save memory, this implementation uses a Bitmap to hold
succ (list of successors) found during the BFS phase that are used in the back-
propagation phase.

[1] Ulrik Brandes. "A faster algorithm for betweenness centrality." Journal of
    Mathematical Sociology, 25(2):163–177, 2001.

[2] Kamesh Madduri, David Ediger, Karl Jiang, David A Bader, and Daniel
    Chavarria-Miranda. "A faster parallel algorithm and efficient multithreaded
    implementations for evaluating betweenness centrality on massive datasets."
    International Symposium on Parallel & Distributed Processing (IPDPS), 2009.
*/


using namespace std;
typedef float ScoreT;

static void ParallelPrefixSum(const pvector<NodeID> &degrees, pvector<SGOffset> &prefix) {
  const size_t block_size = 1 << 20;
  const size_t num_blocks = (degrees.size() + block_size - 1) / block_size;
  pvector<SGOffset> local_sums(num_blocks);
#pragma omp parallel for
  for (size_t block = 0; block < num_blocks; block++) {
    SGOffset lsum = 0;
    size_t block_end = std::min((block + 1) * block_size, degrees.size());
    for (size_t i = block * block_size; i < block_end; i++) lsum += degrees[i];
    local_sums[block] = lsum;
  }
  pvector<SGOffset> bulk_prefix(num_blocks + 1);
  SGOffset total = 0;
  for (size_t block = 0; block < num_blocks; block++) {
    bulk_prefix[block] = total;
    total += local_sums[block];
  }
  bulk_prefix[num_blocks] = total;
#pragma omp parallel for
  for (size_t block = 0; block < num_blocks; block++) {
    SGOffset local_total = bulk_prefix[block];
    size_t block_end = std::min((block + 1) * block_size, degrees.size());
    for (size_t i = block * block_size; i < block_end; i++) {
      prefix[i] = local_total;
      local_total += degrees[i];
    }
  }
  prefix[degrees.size()] = bulk_prefix[num_blocks];
}

static void ParallelLoadDegrees(const Graph &g, pvector<NodeID> &degrees) {
#pragma omp parallel for schedule(dynamic, 16384)
  for (NodeID u = 0; u < g.num_nodes(); u++) {
    degrees[u] = g.out_degree(u);
  }
}

inline int64_t GetEdgeId(const pvector<SGOffset> &prefix, NodeID u, NodeID local_edge_id) {
  return prefix[u] + local_edge_id;
}

void PBFS(const Graph &g, NodeID source, pvector<NodeID> &path_counts,
          Bitmap &succ, vector <SlidingQueue<NodeID>::iterator> &depth_index,
          SlidingQueue<NodeID> &queue, const pvector<SGOffset> &prefix) {
  pvector<NodeID> depths(g.num_nodes(), -1);
  depths[source] = 0;
  path_counts[source] = 1;
  queue.push_back(source);
  depth_index.push_back(queue.begin());
  queue.slide_window();

#pragma omp parallel
  {
    NodeID depth = 0;
    QueueBuffer<NodeID> lqueue(queue);
    while (!queue.empty()) {
#pragma omp single
      depth_index.push_back(queue.begin());
      depth++;
#pragma omp for schedule(dynamic, 64)
      for (auto q_iter = queue.begin(); q_iter < queue.end(); q_iter++) {
        NodeID u = *q_iter;
        NodeID local_edge_id = 0;
        for (NodeID v: g.out_neigh(u)) {
          if ((depths[v] == -1) &&
              (compare_and_swap(depths[v], static_cast<NodeID>(-1), depth))) {
            lqueue.push_back(v);
          }
          if (depths[v] == depth) {
            succ.set_bit_atomic(GetEdgeId(prefix, u, local_edge_id));
            fetch_and_add(path_counts[v], path_counts[u]);
          }
          local_edge_id += 1;
        }
      }
      lqueue.flush();
#pragma omp barrier
#pragma omp single
      queue.slide_window();
    }
  }
  depth_index.push_back(queue.begin());
}


pvector<ScoreT> Brandes(const Graph &g, SourcePicker<Graph> &sp,
                        NodeID num_iters) {
  pvector<ScoreT> scores(g.num_nodes(), 0);
  pvector<NodeID> path_counts(g.num_nodes());
  Bitmap succ(g.num_edges_directed());
  vector <SlidingQueue<NodeID>::iterator> depth_index;
  SlidingQueue<NodeID> queue(g.num_nodes());

  pvector<int32_t> degrees(g.num_nodes());
  ParallelLoadDegrees(g, degrees);
  pvector<SGOffset> prefix(degrees.size() + 1);
  ParallelPrefixSum(degrees, prefix);

  for (NodeID iter = 0; iter < num_iters; iter++) {
    NodeID source = sp.PickNext();
    path_counts.fill(0);
    depth_index.resize(0);
    queue.reset();
    succ.reset();
    PBFS(g, source, path_counts, succ, depth_index, queue, prefix);
    pvector<ScoreT> deltas(g.num_nodes(), 0);

    for (int d = depth_index.size() - 2; d >= 0; d--) {
#pragma omp parallel for schedule(dynamic, 64)
      for (auto it = depth_index[d]; it < depth_index[d + 1]; it++) {
        NodeID u = *it;
        ScoreT delta_u = 0;
        NodeID local_edge_id = 0;
        for (NodeID v: g.out_neigh(u)) {
          if (succ.get_bit(GetEdgeId(prefix, u, local_edge_id))) {
            delta_u += static_cast<ScoreT>(path_counts[u]) /
                       static_cast<ScoreT>(path_counts[v]) * (1 + deltas[v]);
          }
          local_edge_id += 1;
        }
        deltas[u] = delta_u;
        scores[u] += delta_u;
      }
    }
  }
  // normalize scores
  ScoreT biggest_score = 0;
#pragma omp parallel for reduction(max : biggest_score)
  for (NodeID n = 0; n < g.num_nodes(); n++)
    biggest_score = max(biggest_score, scores[n]);
#pragma omp parallel for
  for (NodeID n = 0; n < g.num_nodes(); n++)
    scores[n] = scores[n] / biggest_score;
  return scores;
}


void PrintTopScores(const Graph &g, const pvector<ScoreT> &scores) {
  vector <pair<NodeID, ScoreT>> score_pairs(g.num_nodes());
  for (NodeID n: g.vertices())
    score_pairs[n] = make_pair(n, scores[n]);
  int k = 5;
  vector <pair<ScoreT, NodeID>> top_k = TopK(score_pairs, k);
  for (auto kvp: top_k)
    cout << kvp.second << ":" << kvp.first << endl;
}


// Still uses Brandes algorithm, but has the following differences:
// - serial (no need for atomics or dynamic scheduling)
// - uses vector for BFS queue
// - regenerates farthest to closest traversal order from depths
// - regenerates successors from depths
bool BCVerifier(const Graph &g, SourcePicker<Graph> &sp, NodeID num_iters,
                const pvector<ScoreT> &scores_to_test) {
  pvector<ScoreT> scores(g.num_nodes(), 0);
  for (int iter = 0; iter < num_iters; iter++) {
    NodeID source = sp.PickNext();
    // BFS phase, only records depth & path_counts
    pvector<int> depths(g.num_nodes(), -1);
    depths[source] = 0;
    vector <NodeID> path_counts(g.num_nodes(), 0);
    path_counts[source] = 1;
    vector <NodeID> to_visit;
    to_visit.reserve(g.num_nodes());
    to_visit.push_back(source);
    for (auto it = to_visit.begin(); it != to_visit.end(); it++) {
      NodeID u = *it;
      for (NodeID v: g.out_neigh(u)) {
        if (depths[v] == -1) {
          depths[v] = depths[u] + 1;
          to_visit.push_back(v);
        }
        if (depths[v] == depths[u] + 1)
          path_counts[v] += path_counts[u];
      }
    }
    // Get lists of vertices at each depth
    vector <vector<NodeID>> verts_at_depth;
    for (NodeID n: g.vertices()) {
      if (depths[n] != -1) {
        if (depths[n] >= static_cast<int>(verts_at_depth.size()))
          verts_at_depth.resize(depths[n] + 1);
        verts_at_depth[depths[n]].push_back(n);
      }
    }
    // Going from farthest to clostest, compute "depencies" (deltas)
    pvector<ScoreT> deltas(g.num_nodes(), 0);
    for (int depth = verts_at_depth.size() - 1; depth >= 0; depth--) {
      for (NodeID u: verts_at_depth[depth]) {
        for (NodeID v: g.out_neigh(u)) {
          if (depths[v] == depths[u] + 1) {
            deltas[u] += static_cast<ScoreT>(path_counts[u]) /
                         static_cast<ScoreT>(path_counts[v]) * (1 + deltas[v]);
          }
        }
        scores[u] += deltas[u];
      }
    }
  }
  // Normalize scores
  ScoreT biggest_score = *max_element(scores.begin(), scores.end());
  for (NodeID n: g.vertices())
    scores[n] = scores[n] / biggest_score;
  // Compare scores
  bool all_ok = true;
  for (NodeID n: g.vertices()) {
    if (scores[n] != scores_to_test[n]) {
      cout << n << ": " << scores[n] << " != " << scores_to_test[n] << endl;
      all_ok = false;
    }
  }
  return all_ok;
}


int main(int argc, char *argv[]) {
  CLIterApp cli(argc, argv, "betweenness-centrality", 1);
  if (!cli.ParseArgs())
    return -1;
  if (cli.num_iters() > 1 && cli.start_vertex() != -1)
    cout << "Warning: iterating from same source (-r & -i)" << endl;
  Builder b(cli);
  Graph g = b.MakeGraph();
  SourcePicker<Graph> sp(g, cli.start_vertex());
  auto BCBound =
      [&sp, &cli](const Graph &g) { return Brandes(g, sp, cli.num_iters()); };
  SourcePicker<Graph> vsp(g, cli.start_vertex());
  auto VerifierBound = [&vsp, &cli](const Graph &g,
                                    const pvector<ScoreT> &scores) {
    return BCVerifier(g, vsp, cli.num_iters(), scores);
  };
  BenchmarkKernel(cli, g, BCBound, PrintTopScores, VerifierBound);
  return 0;
}
