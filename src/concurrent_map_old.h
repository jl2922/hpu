#pragma once

#include <omp.h>
#include <functional>
#include <memory>
#include <vector>
#include "parallel.h"
#include "reducer.h"

namespace hpmr {

template <class K, class V, class H>
class DistMap;

template <class K, class V, class H = std::hash<K>>
class ConcurrentMap {
 public:
  ConcurrentMap();

  ConcurrentMap(const ConcurrentMap& m);

  ~ConcurrentMap();

  void reserve(const size_t n_buckets_min);

  size_t get_n_buckets() const { return n_buckets; };

  double get_load_factor() const { return static_cast<double>(n_keys) / n_buckets; }

  double get_max_load_factor() const { return max_load_factor; }

  void set_max_load_factor(const double max_load_factor) {
    this->max_load_factor = max_load_factor;
  }

  size_t get_n_keys() const { return n_keys; }

  void set(
      const K& key,
      const V& value,
      const std::function<void(V&, const V&)>& reducer = Reducer<V>::overwrite);

  void get(const K& key, const std::function<void(const V&)>& handler);

  V get(const K& key, const V& default_value = V());

  void unset(const K& key);

  bool has(const K& key);

  void clear();

  void clear_and_shrink();

  template <class KR, class VR, class HR = std::hash<KR>>
  DistMap<KR, VR, HR> mapreduce(
      const std::function<
          void(const K&, const V&, const std::function<void(const KR&, const VR&)>&)>& mapper,
      const std::function<void(VR&, const VR&)>& reducer,
      const bool verbose = false);

 private:
  size_t n_keys;

  size_t n_buckets;

  double max_load_factor;

  size_t n_segments;

  H hasher;

  std::vector<omp_lock_t> segment_locks;

  // For parallel rehashing (Require omp_set_nested(1)).
  std::vector<omp_lock_t> rehashing_segment_locks;

  struct hash_node {
    K key;
    V value;
    std::unique_ptr<hash_node> next;
    hash_node(const K& key, const V& value) : key(key), value(value){};
  };

  std::vector<std::unique_ptr<hash_node>> buckets;

  constexpr static size_t N_INITIAL_BUCKETS = 11;

  constexpr static size_t N_SEGMENTS_PER_THREAD = 7;

  constexpr static double DEFAULT_MAX_LOAD_FACTOR = 1.0;

  size_t get_hash_value(const K& key);

  // Set with the specified hash value, which shall be consistent with the key.
  void set_with_hash(
      const K& key,
      const size_t hash_value,
      const V& value,
      const std::function<void(V&, const V&)>& reducer = Reducer<V>::overwrite);

  void get_with_hash(
      const K& key, const size_t hash_value, const std::function<void(const V&)>& handler);

  V get_with_hash(const K& key, const size_t hash_value, const V& default_value = V());

  void unset_with_hash(const K& key, const size_t hash_value);

  bool has_with_hash(const K& key, const size_t hash_value);

  void rehash();

  void rehash(const size_t n_rehashing_buckets);

  // Get the number of hash buckets to use.
  // This number shall be larger than or equal to the specified number.
  size_t get_n_rehashing_buckets(const size_t n_buckets_min) const;

  // Apply node_handler to the hash node which has the specific key.
  // If the key does not exist, apply to the unassociated node from the corresponding bucket.
  // The hash value shall be consistent with the specified key.
  void hash_node_apply(
      const K& key,
      const size_t hash_value,
      const std::function<void(std::unique_ptr<hash_node>&)>& node_handler);

  // Apply node_handler to all the hash nodes.
  void hash_node_all_apply(const std::function<void(std::unique_ptr<hash_node>&)>& node_handler);

  // Recursively find the node with the specified key on the list starting from the node specified.
  // Then apply the specified handler to that node.
  // If the key does not exist, apply the handler to the unassociated node at the end of the list.
  void hash_node_apply_recursive(
      std::unique_ptr<hash_node>& node,
      const K& key,
      const std::function<void(std::unique_ptr<hash_node>&)>& node_handler);

  // Recursively apply the handler to each node on the list from the node specified (post-order).
  void hash_node_all_apply_recursive(
      std::unique_ptr<hash_node>& node,
      const std::function<void(std::unique_ptr<hash_node>&)>& node_handler);

  void lock_all_segments();

  void unlock_all_segments();

  friend class DistMap<K, V, H>;
};


template <class K, class V, class H>
ConcurrentMap<K, V, H>::ConcurrentMap() {
  n_keys = 0;
  n_buckets = N_INITIAL_BUCKETS;
  buckets.resize(n_buckets);
  set_max_load_factor(DEFAULT_MAX_LOAD_FACTOR);
  n_segments = Parallel::get_n_threads() * N_SEGMENTS_PER_THREAD;
  segment_locks.resize(n_segments);
  rehashing_segment_locks.resize(n_segments);
  for (auto& lock : segment_locks) omp_init_lock(&lock);
  for (auto& lock : rehashing_segment_locks) omp_init_lock(&lock);
  omp_set_nested(1);  // For parallel rehashing.
}

template <class K, class V, class H>
ConcurrentMap<K, V, H>::ConcurrentMap(const ConcurrentMap<K, V, H>& m) {
  n_keys = m.n_keys;
  n_buckets = m.n_buckets;
  buckets.resize(n_buckets);
  max_load_factor = m.max_load_factor;
  n_segments = m.n_segments;
#pragma omp parallel for
  for (size_t i = 0; i < n_buckets; i++) {
    hash_node* ptr_src = m.buckets[i].get();
    hash_node* ptr_dest = buckets[i].get();
    while (ptr_src != nullptr) {
      ptr_dest = new hash_node(ptr_src->key, ptr_src->value);
      ptr_src = ptr_src->next.get();
      ptr_dest = ptr_dest->next.get();
    }
  }
  segment_locks.resize(n_segments);
  rehashing_segment_locks.resize(n_segments);
  for (auto& lock : segment_locks) omp_init_lock(&lock);
  for (auto& lock : rehashing_segment_locks) omp_init_lock(&lock);
}

template <class K, class V, class H>
ConcurrentMap<K, V, H>::~ConcurrentMap() {
  clear();
  for (auto& lock : segment_locks) omp_destroy_lock(&lock);
  for (auto& lock : rehashing_segment_locks) omp_destroy_lock(&lock);
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::reserve(const size_t n_buckets_min) {
  if (n_buckets >= n_buckets_min) return;
  const size_t n_rehashing_buckets = get_n_rehashing_buckets(n_buckets_min);
  rehash(n_rehashing_buckets);
};

template <class K, class V, class H>
size_t ConcurrentMap<K, V, H>::get_hash_value(const K& key) {
  static size_t n_procs_cache = static_cast<size_t>(Parallel::get_n_procs());
  return hasher(key) / n_procs_cache;
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::rehash() {
  const size_t n_buckets_min = static_cast<size_t>(n_keys / max_load_factor);
  reserve(n_buckets_min);
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::rehash(const size_t n_rehashing_buckets) {
  auto& first_lock = segment_locks[0];
  omp_set_lock(&first_lock);
  if (n_buckets >= n_rehashing_buckets) {
    omp_unset_lock(&first_lock);
    return;
  }
  omp_unset_lock(&first_lock);

  lock_all_segments();

  if (n_buckets >= n_rehashing_buckets) {
    unlock_all_segments();
    return;
  }

  // Rehash.
  std::vector<std::unique_ptr<hash_node>> rehashing_buckets(n_rehashing_buckets);
  const auto& node_handler = [&](std::unique_ptr<hash_node>& node) {
    const auto& rehashing_node_handler = [&](std::unique_ptr<hash_node>& rehashing_node) {
      rehashing_node = std::move(node);
      rehashing_node->next.reset();
    };
    const K& key = node->key;
    const size_t hash_value = get_hash_value(key);
    const size_t bucket_id = hash_value % n_rehashing_buckets;
    const size_t segment_id = bucket_id % n_segments;
    auto& lock = rehashing_segment_locks[segment_id];
    omp_set_lock(&lock);
    hash_node_apply_recursive(rehashing_buckets[bucket_id], key, rehashing_node_handler);
    omp_unset_lock(&lock);
  };
#pragma omp parallel for
  for (size_t i = 0; i < n_buckets; i++) {
    hash_node_all_apply_recursive(buckets[i], node_handler);
  }

  buckets = std::move(rehashing_buckets);
  n_buckets = n_rehashing_buckets;
  unlock_all_segments();
}

template <class K, class V, class H>
size_t ConcurrentMap<K, V, H>::get_n_rehashing_buckets(const size_t n_buckets_min) const {
  // Returns a number that is greater than or equal to n_buckets_min.
  // That number is either a prime number or the product of several prime numbers.
  constexpr size_t PRIMES[] = {
      11, 17, 29, 47, 79, 127, 211, 337, 547, 887, 1433, 2311, 3739, 6053, 9791, 15859};
  constexpr size_t N_PRIMES = sizeof(PRIMES) / sizeof(size_t);
  constexpr size_t LAST_PRIME = PRIMES[N_PRIMES - 1];
  size_t remaining_factor = n_buckets_min;
  size_t n_rehashing_buckets = 1;
  while (remaining_factor > LAST_PRIME) {
    remaining_factor /= LAST_PRIME;
    n_rehashing_buckets *= LAST_PRIME;
  }

  // Find a prime larger than or equal to the remaining factor.
  size_t left = 0, right = N_PRIMES - 1;
  while (left < right) {
    size_t mid = (left + right) / 2;
    if (PRIMES[mid] < remaining_factor) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }

  n_rehashing_buckets *= PRIMES[left];
  return n_rehashing_buckets;
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::set(
    const K& key, const V& value, const std::function<void(V&, const V&)>& reducer) {
  set_with_hash(key, get_hash_value(key), value, reducer);
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::set_with_hash(
    const K& key,
    const size_t hash_value,
    const V& value,
    const std::function<void(V&, const V&)>& reducer) {
  const auto& node_handler = [&](std::unique_ptr<hash_node>& node) {
    if (!node) {
      node.reset(new hash_node(key, value));
#pragma omp atomic
      n_keys++;
    } else {
      reducer(node->value, value);
    }
  };
  hash_node_apply(key, hash_value, node_handler);
  if (n_keys >= n_buckets * max_load_factor) rehash();
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::get(const K& key, const std::function<void(const V&)>& handler) {
  get_with_hash(key, get_hash_value(key), handler);
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::get_with_hash(
    const K& key, const size_t hash_value, const std::function<void(const V&)>& handler) {
  const auto& node_handler = [&](std::unique_ptr<hash_node>& node) {
    if (node) handler(node->value);
  };
  hash_node_apply(key, hash_value, node_handler);
}

template <class K, class V, class H>
V ConcurrentMap<K, V, H>::get(const K& key, const V& default_value) {
  return get_with_hash(key, get_hash_value(key), default_value);
}

template <class K, class V, class H>
V ConcurrentMap<K, V, H>::get_with_hash(
    const K& key, const size_t hash_value, const V& default_value) {
  V value(default_value);
  const auto& node_handler = [&](const std::unique_ptr<hash_node>& node) {
    if (node) value = node->value;
  };
  hash_node_apply(key, hash_value, node_handler);
  return value;
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::unset(const K& key) {
  unset_with_hash(key, get_hash_value(key));
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::unset_with_hash(const K& key, const size_t hash_value) {
  const auto& node_handler = [&](std::unique_ptr<hash_node>& node) {
    if (node) {
      node = std::move(node->next);
#pragma omp atomic
      n_keys--;
    }
  };
  hash_node_apply(key, hash_value, node_handler);
}

template <class K, class V, class H>
bool ConcurrentMap<K, V, H>::has(const K& key) {
  return has_with_hash(key, get_hash_value(key));
}

template <class K, class V, class H>
bool ConcurrentMap<K, V, H>::has_with_hash(const K& key, const size_t hash_value) {
  bool has_key = false;
  const auto& node_handler = [&](const std::unique_ptr<hash_node>& node) {
    if (node) has_key = true;
  };
  hash_node_apply(key, hash_value, node_handler);
  return has_key;
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::clear() {
  lock_all_segments();
#pragma omp parallel for
  for (size_t i = 0; i < n_buckets; i++) {
    buckets[i].reset();
  }
  n_keys = 0;
  unlock_all_segments();
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::clear_and_shrink() {
  lock_all_segments();
#pragma omp parallel for
  for (size_t i = 0; i < n_buckets; i++) {
    buckets[i].reset();
  }
  n_keys = 0;
  n_buckets = N_INITIAL_BUCKETS;
  buckets.resize(n_buckets);
  unlock_all_segments();
}

template <class K, class V, class H>
template <class KR, class VR, class HR>
DistMap<KR, VR, HR> ConcurrentMap<K, V, H>::mapreduce(
    const std::function<void(const K&, const V&, const std::function<void(const KR&, const VR&)>&)>&
        mapper,
    const std::function<void(VR&, const VR&)>& reducer,
    const bool verbose) {
  DistMap<KR, VR, HR> res;
  const int proc_id = Parallel::get_proc_id();
  const int n_procs = Parallel::get_n_procs();
  const int n_threads = Parallel::get_n_threads();
  double target_progress = 0.1;

  const auto& emit = [&](const KR& key, const VR& value) { res.set(key, value, reducer); };
  if (verbose && proc_id == 0) {
    printf("MapReduce on %d node(s) (%d threads): ", n_procs, n_threads * n_procs);
  }

#pragma omp parallel for schedule(static, 1)
  for (size_t i = proc_id; i < n_buckets; i += n_procs) {
    hash_node* ptr = buckets[i].get();
    while (ptr != nullptr) {
      mapper(ptr->key, ptr->value, emit);
      ptr = ptr->next.get();
    }
    const int thread_id = Parallel::get_thread_id();
    if (verbose && proc_id == 0 && thread_id == 0) {
      const double current_progress = i * 100.0 / n_buckets;
      while (target_progress <= current_progress) {
        printf("%.1f%% ", target_progress);
        target_progress *= 2;
      }
    }
  }
  res.sync(verbose);

  if (verbose && proc_id == 0) printf("Done\n");

  return res;
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::hash_node_apply(
    const K& key,
    const size_t hash_value,
    const std::function<void(std::unique_ptr<hash_node>&)>& node_handler) {
  bool applied = false;
  while (!applied) {
    const size_t n_buckets_snapshot = n_buckets;
    const size_t bucket_id = hash_value % n_buckets_snapshot;
    const size_t segment_id = bucket_id % n_segments;
    auto& lock = segment_locks[segment_id];
    omp_set_lock(&lock);
    if (n_buckets_snapshot != n_buckets) {
      omp_unset_lock(&lock);
      continue;
    }
    hash_node_apply_recursive(buckets[bucket_id], key, node_handler);
    omp_unset_lock(&lock);
    applied = true;
  }
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::hash_node_all_apply(
    const std::function<void(std::unique_ptr<hash_node>&)>& node_handler) {
  lock_all_segments();
#pragma omp parallel for
  for (size_t i = 0; i < n_buckets; i++) {
    hash_node_all_apply_recursive(buckets[i], node_handler);
  }
  unlock_all_segments();
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::hash_node_apply_recursive(
    std::unique_ptr<hash_node>& node,
    const K& key,
    const std::function<void(std::unique_ptr<hash_node>&)>& node_handler) {
  if (node) {
    if (node->key == key) {
      node_handler(node);
    } else {
      hash_node_apply_recursive(node->next, key, node_handler);
    }
  } else {
    node_handler(node);
  }
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::hash_node_all_apply_recursive(
    std::unique_ptr<hash_node>& node,
    const std::function<void(std::unique_ptr<hash_node>&)>& node_handler) {
  if (node) {
    // Post-order traversal for rehashing.
    hash_node_all_apply_recursive(node->next, node_handler);
    node_handler(node);
  }
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::lock_all_segments() {
  for (auto& lock : segment_locks) omp_set_lock(&lock);
}

template <class K, class V, class H>
void ConcurrentMap<K, V, H>::unlock_all_segments() {
  for (auto& lock : segment_locks) omp_unset_lock(&lock);
}


}  // namespace hpmr