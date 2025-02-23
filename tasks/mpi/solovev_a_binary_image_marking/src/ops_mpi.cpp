
#include "mpi/solovev_a_binary_image_marking/include/ops_mpi.hpp"

#include <algorithm>
#include <boost/mpi/collectives.hpp>
#include <boost/mpi/collectives/broadcast.hpp>
#include <boost/mpi/collectives/gatherv.hpp>
#include <boost/mpi/collectives/scatterv.hpp>
#include <boost/mpi/communicator.hpp>
#include <ranges>
#include <queue>
#include <vector>
#include <utility>

bool solovev_a_binary_image_marking::TestMPITaskSequential::PreProcessingImpl() {
  int m_tmp = *reinterpret_cast<int*>(task_data->inputs[0]);
  int n_tmp = *reinterpret_cast<int*>(task_data->inputs[1]);
  auto* tmp_data = reinterpret_cast<int*>(task_data->inputs[2]);
  data_seq_.assign(tmp_data, tmp_data + task_data->inputs_count[2]);
  m_seq_ = m_tmp;
  n_seq_ = n_tmp;
  labels_seq_.resize(m_seq_ * n_seq_);
  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskSequential::ValidationImpl() {
  int rows_check = *reinterpret_cast<int*>(task_data->inputs[0]);
  int coloms_check = *reinterpret_cast<int*>(task_data->inputs[1]);

  std::vector<int> input_check;

  int* input_check_data = reinterpret_cast<int*>(task_data->inputs[2]);
  int input_check_size = static_cast<int>(task_data->inputs_count[2]);
  input_check.assign(input_check_data, input_check_data + input_check_size);

  return (rows_check > 0 && coloms_check > 0 && !input_check.empty());
}

bool solovev_a_binary_image_marking::TestMPITaskSequential::RunImpl() {
  std::vector<Point> directions = {{.x = -1, .y = 0}, {.x = 1, .y = 0}, {.x = 0, .y = -1}, {.x = 0, .y = 1}};
  int label = 1;

  std::queue<Point> q;

  for (int i = 0; i < m_seq_; ++i) {
    for (int j = 0; j < n_seq_; ++j) {
      if (data_seq_[(i * n_seq_) + j] == 1 && labels_seq_[(i * n_seq_) + j] == 0) {
        q.push({i, j});
        labels_seq_[(i * n_seq_) + j] = label;

        while (!q.empty()) {
          Point current = q.front();
          q.pop();

          for (const Point& dir : directions) {
            int new_x = current.x + dir.x;
            int new_y = current.y + dir.y;

            if (new_x >= 0 && new_x < m_seq_ && new_y >= 0 && new_y < n_seq_) {
              int new_idx = (new_x * n_seq_) + new_y;
              if (data_seq_[new_idx] == 1 && labels_seq_[new_idx] == 0) {
                labels_seq_[new_idx] = label;
                q.push({new_x, new_y});
              }
            }
          }
        }
        ++label;
      }
    }
  }

  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskSequential::PostProcessingImpl() {
  int* output_ = reinterpret_cast<int*>(task_data->outputs[0]);
  std::ranges::copy(labels_seq_, output_);

  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskParallel::PreProcessingImpl() {
  if (world_.rank() == 0) {
    int m_count = *reinterpret_cast<int*>(task_data->inputs[0]);
    int n_count = *reinterpret_cast<int*>(task_data->inputs[1]);
    auto* data_tmp = reinterpret_cast<int*>(task_data->inputs[2]);
    data_.assign(data_tmp, data_tmp + task_data->inputs_count[2]);
    m_ = m_count;
    n_ = n_count;
  }
  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskParallel::ValidationImpl() {
  if (world_.rank() == 0) {
    int m_check = *reinterpret_cast<int*>(task_data->inputs[0]);
    int n_check = *reinterpret_cast<int*>(task_data->inputs[1]);
    int input_check_size = task_data->inputs_count[2];
    return (m_check > 0 && n_check > 0 && input_check_size > 0);
  }
  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskParallel::RunImpl() {
  std::vector<Point> directions = {{.x = -1, .y = 0}, {.x = 1, .y = 0}, {.x = 0, .y = -1}, {.x = 0, .y = 1}};
  boost::mpi::broadcast(world_, m_, 0);
  boost::mpi::broadcast(world_, n_, 0);

  int proc_rank = world_.rank();
  int proc_count = world_.size();

  std::vector<int> counts(proc_count, 0);
  std::vector<int> displacements(proc_count, 0);

  int current_row_offset = 0;

  for (int proc = 0; proc < proc_count; ++proc) {
    int proc_rows = m_ / proc_count;
    counts[proc] = proc_rows * n_;
    displacements[proc] = current_row_offset * n_;
    current_row_offset += proc_rows;
    if (proc == proc_count - 1) counts[proc] += (n_ * m_ - (m_ / proc_count) * n_ * proc_count);
  }

  int local_pixel_count = counts[proc_rank];
  std::vector<int> local_image(local_pixel_count);
  boost::mpi::scatterv(world_, data_.data(), counts, displacements, local_image.data(), local_pixel_count, 0);
  std::vector<int> local_labels(local_pixel_count, 0);
  int base_label = displacements[proc_rank] + 1;
  int curr_label = base_label;
  auto to_coordinates = [this](int idx) -> std::pair<int, int> { return {idx / this->n_, idx % this->n_}; };
  int* p_local_image = local_image.data();
  int* p_local_labels = local_labels.data();

  for (int i = 0; i < local_pixel_count; ++i) {
    if (p_local_image[i] == 1 && p_local_labels[i] == 0) {
      std::queue<Point> bfs_queue;
      auto coord = to_coordinates(i);
      bfs_queue.push({coord.first, coord.second});
      p_local_labels[i] = curr_label;
      while (!bfs_queue.empty()) {
        Point cp = bfs_queue.front();
        bfs_queue.pop();
        for (const auto& step : directions) {
          int nr = cp.x + step.x, nc = cp.y + step.y;
          if (nr >= 0 && nr < (local_pixel_count / n_) && nc >= 0 && nc < n_) {
            int ni = (nr * n_) + nc;
            if (p_local_image[ni] == 1 && p_local_labels[ni] == 0) {
              p_local_labels[ni] = curr_label;
              bfs_queue.push({nr, nc});
            }
          }
        }
      }
      curr_label++;
    }
  }

  std::vector<int> global_labels;

  if (proc_rank == 0) global_labels.resize(m_ * n_);

  boost::mpi::gatherv(world_, local_labels, global_labels.data(), counts, displacements, 0);

  if (proc_rank == 0) {
    int total = m_ * n_;
    int* p_global = global_labels.data();
    std::vector<int> parent(total + 1);
    for (int i = 1; i <= total; ++i) parent[i] = i;
    auto find_rep = [&parent](int x) -> int {
      while (x != parent[x]) x = parent[x] = parent[parent[x]];
      return x;
    };
    auto union_rep = [&find_rep, &parent](int a, int b) {
      int ra = find_rep(a), rb = find_rep(b);
      if (ra != rb) {
        int newRep = (ra < rb) ? ra : rb;
        int obsolete = (ra < rb) ? rb : ra;
        parent[obsolete] = newRep;
      }
    };
    for (int row = 0; row < m_; ++row) {
      for (int col = 0; col < n_; ++col) {
        int idx = (row * n_) + col;
        if (p_global[idx] == 0) continue;
        int label = p_global[idx];
        if (col > 0 && p_global[(row * n_ + col) - 1] != 0) union_rep(label, p_global[(row * n_) + col - 1]);
        if (row > 0 && p_global[((row - 1) * n_) + col] != 0) union_rep(label, p_global[((row - 1) * n_) + col]);
      }
    }
    for (int i = 0; i < total; ++i)
      if (p_global[i] != 0) p_global[i] = find_rep(p_global[i]);
    std::unordered_map<int, int> norm;
    int nextLabel = 1;
    for (int i = 0; i < total; ++i) {
      if (p_global[i] != 0) {
        int rep = p_global[i];
        if (norm.find(rep) == norm.end()) norm[rep] = nextLabel++;
        p_global[i] = norm[rep];
      }
    }
    labels_ = std::move(global_labels);
  }
  return true;
}

bool solovev_a_binary_image_marking::TestMPITaskParallel::PostProcessingImpl() {
  if (world_.rank() == 0) {
    int* output_ = reinterpret_cast<int*>(task_data->outputs[0]);
    std::ranges::copy(labels_, output_);
  }
  return true;
}