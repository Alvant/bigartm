// Copyright 2014, Additive Regularization of Topic Models.

#include "artm/core/processor.h"

#include <stdlib.h>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "boost/lexical_cast.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/uuid/uuid_io.hpp"

#include "glog/logging.h"

#include "artm/regularizer_interface.h"
#include "artm/score_calculator_interface.h"

#include "artm/core/protobuf_helpers.h"
#include "artm/core/call_on_destruction.h"
#include "artm/core/helpers.h"
#include "artm/core/instance_schema.h"
#include "artm/core/merger.h"
#include "artm/core/topic_model.h"

#include "artm/utility/blas.h"

namespace util = artm::utility;
namespace fs = boost::filesystem;

namespace {

template<typename T>
class DenseMatrix {
 public:
  DenseMatrix(int no_rows = 0, int no_columns = 0, bool store_by_rows = true)
    : no_rows_(no_rows),
    no_columns_(no_columns),
    store_by_rows_(store_by_rows),
    data_(nullptr) {
    if (no_rows > 0 && no_columns > 0) {
      data_ = new T[no_rows_ * no_columns_];
    }
  }

  DenseMatrix(const DenseMatrix<T>& src_matrix) {
    no_rows_ = src_matrix.no_rows();
    no_columns_ = src_matrix.no_columns();
    store_by_rows_ = src_matrix.store_by_rows_;
    if (no_columns_ >0 && no_rows_ > 0) {
      data_ = new T[no_rows_ * no_columns_];
      for (int i = 0; i < no_rows_ * no_columns_; ++i) {
        data_[i] = src_matrix.get_data()[i];
      }
    } else {
      data_ = nullptr;
    }
  }

  ~DenseMatrix() {
    delete[] data_;
  }

  void InitializeZeros() {
    memset(data_, 0, sizeof(T)* no_rows_ * no_columns_);
  }

  T& operator() (int index_row, int index_col) {
    assert(index_row < no_rows_);
    assert(index_col < no_columns_);
    if (store_by_rows_) {
      return data_[index_row * no_columns_ + index_col];
    }
    return data_[index_col * no_rows_ + index_row];
  }

  const T& operator() (int index_row, int index_col) const {
    assert(index_row < no_rows_);
    assert(index_col < no_columns_);
    if (store_by_rows_) {
      return data_[index_row * no_columns_ + index_col];
    }
    return data_[index_col * no_rows_ + index_row];
  }

  DenseMatrix<T>& operator= (const DenseMatrix<T>& src_matrix) {
    no_rows_ = src_matrix.no_rows();
    no_columns_ = src_matrix.no_columns();
    store_by_rows_ = src_matrix.store_by_rows_;
    if (data_ != nullptr) {
      delete[] data_;
    }
    if (no_columns_ >0 && no_rows_ > 0) {
      data_ = new T[no_rows_ * no_columns_];
      for (int i = 0; i < no_rows_ * no_columns_; ++i) {
        data_[i] = src_matrix.get_data()[i];
      }
    } else {
      data_ = nullptr;
    }

    return *this;
  }

  int no_rows() const { return no_rows_; }
  int no_columns() const { return no_columns_; }

  T* get_data() {
    return data_;
  }

  const T* get_data() const {
    return data_;
  }

 private:
  int no_rows_;
  int no_columns_;
  bool store_by_rows_;
  T* data_;
};

template<typename T>
class CsrMatrix {
 public:
  explicit CsrMatrix(int m, int n, int nnz) : m_(m), n_(n), nnz_(nnz) {
    assert(m > 0 && n > 0 && nnz > 0);
    val_.resize(nnz);
    col_ind_.resize(nnz);
    row_ptr_.resize(m + 1);
  }

  explicit CsrMatrix(int n, std::vector<T>* val, std::vector<int>* row_ptr, std::vector<int>* col_ind) {
    assert(val != nullptr && row_ptr != nullptr && col_ind != nullptr);
    m_ = static_cast<int>(row_ptr->size()) - 1;
    n_ = n;  // this parameter can't be deduced automatically
    nnz_ = static_cast<int>(val->size());
    val_.swap(*val);
    row_ptr_.swap(*row_ptr);
    col_ind_.swap(*col_ind);
  }

  void Transpose(artm::utility::Blas* blas) {
    std::vector<int> row_ptr_new_(n_ + 1);
    blas->scsr2csc(m_, n_, nnz_, val(), row_ptr(), col_ind(), val(), col_ind(), &row_ptr_new_[0]);
    int tmp = m_; m_ = n_; n_ = tmp;  // swat(m, n)
    row_ptr_.swap(row_ptr_new_);
  }

  T* val() { return &val_[0]; }
  const T* val() const { return &val_[0]; }

  int* row_ptr() { return &row_ptr_[0]; }
  const int* row_ptr() const { return &row_ptr_[0]; }

  int* col_ind() { return &col_ind_[0]; }
  const int* col_ind() const { return &col_ind_[0]; }

  int m() const { return m_; }
  int n() const { return n_; }
  int nnz() const { return nnz_; }

 private:
  int m_;
  int n_;
  int nnz_;
  std::vector<T> val_;
  std::vector<int> row_ptr_;
  std::vector<int> col_ind_;
};

template<int operation>
void ApplyByElement(DenseMatrix<float>* result_matrix,
  const DenseMatrix<float>& first_matrix,
  const DenseMatrix<float>& second_matrix) {
  int height = first_matrix.no_rows();
  int width = first_matrix.no_columns();

  assert(height == second_matrix.no_rows());
  assert(width == second_matrix.no_columns());

  float* result_data = result_matrix->get_data();
  const float* first_data = first_matrix.get_data();
  const float* second_data = second_matrix.get_data();
  int size = height * width;

  if (operation == 0) {
    for (int i = 0; i < size; ++i)
      result_data[i] = first_data[i] * second_data[i];
  }
  if (operation == 1) {
    for (int i = 0; i < size; ++i) {
      if (first_data[i] == 0 || second_data[i] == 0)
        result_data[i] = 0;
      else
        result_data[i] = first_data[i] / second_data[i];
    }
  }
  if (operation != 0 && operation != 1) {
    LOG(ERROR) << "In function ApplyByElement() in Processo::ThreadFunction() "
      << "'operation' argument was set an unsupported value\n";
  }
}

}  // namespace

namespace artm {
namespace core {

Processor::Processor(ThreadSafeQueue<std::shared_ptr<ProcessorInput> >*  processor_queue,
                     ThreadSafeQueue<std::shared_ptr<ModelIncrement> >* merger_queue,
                     const Merger& merger,
                     const ThreadSafeHolder<InstanceSchema>& schema)
    : processor_queue_(processor_queue),
      merger_queue_(merger_queue),
      merger_(merger),
      schema_(schema),
      is_stopping(false),
      thread_() {
  // Keep this at the last action in constructor.
  // http://stackoverflow.com/questions/15751618/initialize-boost-thread-in-object-constructor
  boost::thread t(&Processor::ThreadFunction, this);
  thread_.swap(t);
}

Processor::~Processor() {
  is_stopping = true;
  if (thread_.joinable()) {
    thread_.join();
  }
}

Processor::StreamIterator::StreamIterator(const ProcessorInput& processor_input)
    : items_count_(processor_input.batch().item_size()),
      item_index_(-1),  // // handy trick for iterators
      stream_flags_(nullptr),
      processor_input_(processor_input) {
}

const Item* Processor::StreamIterator::Next() {
  for (;;) {
    item_index_++;  // handy trick pays you back!

    if (item_index_ >= items_count_) {
      // reached the end of the stream
      break;
    }

    if (!stream_flags_ || stream_flags_->value(item_index_)) {
      // found item that is included in the stream
      break;
    }
  }

  return Current();
}

const Item* Processor::StreamIterator::Current() const {
  if (item_index_ >= items_count_)
    return nullptr;

  return &(processor_input_.batch().item(item_index_));
}

bool Processor::StreamIterator::InStream(const std::string& stream_name) {
  if (item_index_ >= items_count_)
    return false;

  int index_of_stream = repeated_field_index_of(processor_input_.stream_name(), stream_name);
  if (index_of_stream == -1) {
    return true;
  }

  return processor_input_.stream_mask(index_of_stream).value(item_index_);
}

bool Processor::StreamIterator::InStream(int stream_index) {
  if (stream_index == -1)
    return true;

  assert(stream_index >= 0 && stream_index < processor_input_.stream_name_size());

  if (item_index_ >= items_count_)
    return false;

  return processor_input_.stream_mask(stream_index).value(item_index_);
}

static std::shared_ptr<ModelIncrement>
InitializeModelIncrement(const ProcessorInput& part, const ModelConfig& model_config,
                         const ::artm::core::TopicModel& topic_model) {
  std::shared_ptr<ModelIncrement> model_increment = std::make_shared<ModelIncrement>();
  model_increment->add_batch_uuid(part.batch_uuid());

  const Batch& batch = part.batch();
  int topic_size = model_config.topics_count();

  // process part and store result in merger queue
  model_increment->set_model_name(model_config.name());
  model_increment->set_topics_count(topic_size);
  model_increment->mutable_topic_name()->CopyFrom(topic_model.topic_name());
  for (int token_index = 0; token_index < part.batch().token_size(); ++token_index) {
    Token token = Token(batch.class_id(token_index), batch.token(token_index));
    model_increment->add_token(token.keyword);
    model_increment->add_class_id(token.class_id);
    FloatArray* counters = model_increment->add_token_increment();

    if (topic_model.has_token(token)) {
      model_increment->add_operation_type(ModelIncrement_OperationType_IncrementValue);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        counters->add_value(0.0f);
      }
    } else {
      model_increment->add_operation_type(ModelIncrement_OperationType_CreateIfNotExist);
    }
  }

  return model_increment;
}

static std::shared_ptr<DenseMatrix<float>>
InitializeTheta(const Batch& batch, const ModelConfig& model_config, const DataLoaderCacheEntry* cache) {
  int topic_size = model_config.topics_count();

  std::shared_ptr<DenseMatrix<float>> Theta;
  if (model_config.use_sparse_bow()) {
    Theta = std::make_shared<DenseMatrix<float>>(topic_size, batch.item_size(), false);
  } else {
    Theta = std::make_shared<DenseMatrix<float>>(topic_size, batch.item_size());
  }

  Theta->InitializeZeros();

  for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
    int index_of_item = -1;
    if ((cache != nullptr) && model_config.reuse_theta()) {
      index_of_item = repeated_field_index_of(cache->item_id(),
        batch.item(item_index).id());
    }

    if ((index_of_item != -1) && model_config.reuse_theta()) {
      const FloatArray& old_thetas = cache->theta(index_of_item);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        (*Theta)(topic_index, item_index) = old_thetas.value(topic_index);
      }
    } else {
      const float default_theta = 1.0f / topic_size;
      for (int iTopic = 0; iTopic < topic_size; ++iTopic) {
        float theta_value = default_theta;
        if (model_config.use_random_theta())
          theta_value = ThreadSafeRandom::singleton().GenerateFloat();
        (*Theta)(iTopic, item_index) = theta_value;
      }
    }
  }

  return Theta;
}

static std::shared_ptr<DenseMatrix<float>>
InitializePhi(const Batch& batch, const ModelConfig& model_config,
              const ::artm::core::TopicModel& topic_model) {
  bool phi_is_empty = true;
  int topic_size = topic_model.topic_size();
  auto phi_matrix = std::make_shared<DenseMatrix<float>>(batch.token_size(), topic_size);
  phi_matrix->InitializeZeros();
  for (int token_index = 0; token_index < batch.token_size(); ++token_index) {
    Token token = Token(batch.class_id(token_index), batch.token(token_index));

    if (topic_model.has_token(token)) {
      phi_is_empty = false;
      auto topic_iter = topic_model.GetTopicWeightIterator(token);
      for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
        float value = topic_iter[topic_index];
        if (value < 1e-16) {
          // Reset small values to 0.0 to avoid performance hit.
          // http://en.wikipedia.org/wiki/Denormal_number#Performance_issues
          // http://stackoverflow.com/questions/13964606/inconsistent-multiplication-performance-with-floats
          value = 0.0f;
        }
        (*phi_matrix)(token_index, topic_index) = value;
      }
    }
  }

  if (phi_is_empty)
    return nullptr;

  return phi_matrix;
}

static void RegularizeAndNormalizeTheta(int inner_iter, const Batch& batch, const ModelConfig& model_config,
                                        const InstanceSchema& schema, DenseMatrix<float>* Theta) {
  int topic_size = model_config.topics_count();

  // next section proceed Theta regularization
  int item_index = -1;
  std::vector<float> theta_next;
  for (const Item& item : batch.item()) {
    item_index++;
    // this loop put data from blas::matrix to std::vector. It's not efficient
    // and would be avoid after final choice of data structures and corrsponding
    // adaptation of regularization interface
    theta_next.clear();
    for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
      theta_next.push_back((*Theta)(topic_index, item_index));
    }

    auto reg_names = model_config.regularizer_name();
    auto reg_tau = model_config.regularizer_tau();
    for (auto reg_name_iterator = reg_names.begin();
      reg_name_iterator != reg_names.end();
      reg_name_iterator++) {
      auto regularizer = schema.regularizer(reg_name_iterator->c_str());
      if (regularizer != nullptr) {
        auto tau_index = reg_name_iterator - reg_names.begin();
        double tau = reg_tau.Get(tau_index);

        bool retval = regularizer->RegularizeTheta(
          item, &theta_next, model_config.topic_name(), inner_iter, tau);
        if (!retval) {
          LOG(ERROR) << "Problems with type or number of parameters in Theta" <<
            "regularizer <" << reg_name_iterator->c_str() <<
            ">. On this iteration this regularizer was turned off.\n";
        }
      } else {
        LOG(ERROR) << "Theta Regularizer with name <" << reg_name_iterator->c_str() <<
          "> does not exist.";
      }
    }

    // Normalize Theta for current item
    for (int i = 0; i < static_cast<int>(theta_next.size()); ++i) {
      if (theta_next[i] < 0) {
        theta_next[i] = 0;
      }
    }

    float sum = 0.0f;
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      sum += theta_next[topic_index];

    for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
      float val = (sum > 0) ? (theta_next[topic_index] / sum) : 0.0f;
      if (val < 1e-16f) val = 0.0f;
      (*Theta)(topic_index, item_index) = val;
    }
  }
}

static std::shared_ptr<CsrMatrix<float>>
InitializeSparseNdw(const Batch& batch, const ModelConfig& model_config) {
  std::vector<float> n_dw_val;
  std::vector<int> n_dw_row_ptr;
  std::vector<int> n_dw_col_ind;

  bool use_classes = false;
  std::map<ClassId, float> class_id_to_weight;
  if (model_config.class_id_size() != 0) {
    use_classes = true;
    for (int i = 0; i < model_config.class_id_size(); ++i) {
      class_id_to_weight.insert(std::make_pair(model_config.class_id(i), model_config.class_weight(i)));
    }
  }

  // For sparse case
  for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
    n_dw_row_ptr.push_back(n_dw_val.size());
    auto current_item = batch.item(item_index);
    for (auto& field : current_item.field()) {
      for (int token_index = 0; token_index < field.token_id_size(); ++token_index) {
        int token_id = field.token_id(token_index);

        float class_weight = 1.0f;
        if (use_classes) {
          ClassId class_id = batch.class_id(token_id);
          auto iter = class_id_to_weight.find(class_id);
          class_weight = (iter == class_id_to_weight.end()) ? 0.0f : iter->second;
        }

        float token_count = static_cast<float>(field.token_count(token_index));
        n_dw_val.push_back(class_weight * token_count);
        n_dw_col_ind.push_back(token_id);
      }
    }
  }

  n_dw_row_ptr.push_back(n_dw_val.size());
  return std::make_shared<CsrMatrix<float>>(batch.token_size(), &n_dw_val, &n_dw_row_ptr, &n_dw_col_ind);
}

static std::shared_ptr<DenseMatrix<float>>
InitializeDenseNdw(const Batch& batch) {
  auto n_dw = std::make_shared<DenseMatrix<float>>(batch.token_size(), batch.item_size());
  n_dw->InitializeZeros();

  for (int item_index = 0; item_index < n_dw->no_columns(); ++item_index) {
    auto current_item = batch.item(item_index);
    for (auto& field : current_item.field()) {
      for (int token_index = 0; token_index < field.token_id_size(); ++token_index) {
        int token_id = field.token_id(token_index);
        int token_count = field.token_count(token_index);
        (*n_dw)(token_id, item_index) += token_count;
      }
    }
  }

  return n_dw;
}

static std::shared_ptr<DenseMatrix<float>>
CalculateNwtSparse(const ModelConfig& model_config, const Batch& batch, const Mask* mask, const InstanceSchema& schema,
                   const CsrMatrix<float>& sparse_ndw, const DenseMatrix<float>& phi_matrix,
                   DenseMatrix<float>* theta_matrix, util::Blas* blas) {
  auto n_wt = std::make_shared<DenseMatrix<float>>(phi_matrix.no_rows(), phi_matrix.no_columns());
  n_wt->InitializeZeros();

  for (int inner_iter = 0; inner_iter < model_config.inner_iterations_count(); ++inner_iter) {
    DenseMatrix<float> n_td(theta_matrix->no_rows(), theta_matrix->no_columns(), false);
    n_td.InitializeZeros();

    int topics_count = phi_matrix.no_columns();
    int docs_count = theta_matrix->no_columns();
    for (int d = 0; d < docs_count; ++d) {
      for (int i = sparse_ndw.row_ptr()[d]; i < sparse_ndw.row_ptr()[d + 1]; ++i) {
        int w = sparse_ndw.col_ind()[i];
        float p_dw_val = blas->sdot(topics_count, &phi_matrix(w, 0), 1, &(*theta_matrix)(0, d), 1);  // NOLINT
        if (p_dw_val == 0) continue;
        blas->saxpy(topics_count, sparse_ndw.val()[i] / p_dw_val, &phi_matrix(w, 0), 1, &n_td(0, d), 1);
      }
    }

    ApplyByElement<0>(theta_matrix, *theta_matrix, n_td);
    RegularizeAndNormalizeTheta(inner_iter, batch, model_config, schema, theta_matrix);
  }

  int tokens_count = phi_matrix.no_rows();
  int topics_count = phi_matrix.no_columns();
  int docs_count = theta_matrix->no_columns();

  CsrMatrix<float> sparse_nwd(sparse_ndw);
  sparse_nwd.Transpose(blas);

  // n_wt should be count for items, that have corresponding true-value in stream mask
  // from batch. Or for all items, if such mask doesn't exist
  if (mask != nullptr) {
    for (int w = 0; w < tokens_count; ++w) {
      for (int i = sparse_nwd.row_ptr()[w]; i < sparse_nwd.row_ptr()[w + 1]; ++i) {
        int d = sparse_nwd.col_ind()[i];
        if (mask->value(d) == false) continue;
        float p_wd_val = blas->sdot(topics_count, &phi_matrix(w, 0), 1, &(*theta_matrix)(0, d), 1);  // NOLINT
        if (p_wd_val == 0) continue;
        blas->saxpy(topics_count, sparse_nwd.val()[i] / p_wd_val,
          &(*theta_matrix)(0, d), 1, &(*n_wt)(w, 0), 1);  // NOLINT
      }
    }
  } else {
    for (int w = 0; w < tokens_count; ++w) {
      for (int i = sparse_nwd.row_ptr()[w]; i < sparse_nwd.row_ptr()[w + 1]; ++i) {
        int d = sparse_nwd.col_ind()[i];
        float p_wd_val = blas->sdot(topics_count, &phi_matrix(w, 0), 1, &(*theta_matrix)(0, d), 1);  // NOLINT
        if (p_wd_val == 0) continue;
        blas->saxpy(topics_count, sparse_nwd.val()[i] / p_wd_val,
          &(*theta_matrix)(0, d), 1, &(*n_wt)(w, 0), 1);  // NOLINT
      }
    }
  }

  ApplyByElement<0>(n_wt.get(), *n_wt, phi_matrix);
  return n_wt;
}

static std::shared_ptr<DenseMatrix<float>>
CalculateNwtDense(const ModelConfig& model_config, const Batch& batch, const Mask* mask, const InstanceSchema& schema,
                  const DenseMatrix<float>& dense_ndw, const DenseMatrix<float>& phi_matrix,
                  DenseMatrix<float>* theta_matrix, util::Blas* blas) {
  auto n_wt = std::make_shared<DenseMatrix<float>>(phi_matrix.no_rows(), phi_matrix.no_columns());
  n_wt->InitializeZeros();

  DenseMatrix<float> Z(phi_matrix.no_rows(), theta_matrix->no_columns());
  for (int inner_iter = 0; inner_iter < model_config.inner_iterations_count(); ++inner_iter) {
    blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::NoTrans,
      phi_matrix.no_rows(), theta_matrix->no_columns(), phi_matrix.no_columns(), 1, phi_matrix.get_data(),
      phi_matrix.no_columns(), theta_matrix->get_data(), theta_matrix->no_columns(), 0, Z.get_data(),
      theta_matrix->no_columns());

    // Z = n_dw ./ Z
    ApplyByElement<1>(&Z, dense_ndw, Z);

    // Theta_new = Theta .* (Phi' * Z) ./ repmat(n_d, nTopics, 1);
    DenseMatrix<float> prod_trans_phi_Z(phi_matrix.no_columns(), Z.no_columns());

    blas->sgemm(util::Blas::RowMajor, util::Blas::Trans, util::Blas::NoTrans,
      phi_matrix.no_columns(), Z.no_columns(), phi_matrix.no_rows(), 1, phi_matrix.get_data(),
      phi_matrix.no_columns(), Z.get_data(), Z.no_columns(), 0,
      prod_trans_phi_Z.get_data(), Z.no_columns());

    ApplyByElement<0>(theta_matrix, *theta_matrix, prod_trans_phi_Z);
    RegularizeAndNormalizeTheta(inner_iter, batch, model_config, schema, theta_matrix);
  }

  blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::NoTrans,
    phi_matrix.no_rows(), theta_matrix->no_columns(), phi_matrix.no_columns(), 1, phi_matrix.get_data(),
    phi_matrix.no_columns(), theta_matrix->get_data(), theta_matrix->no_columns(), 0, Z.get_data(),
    theta_matrix->no_columns());

  ApplyByElement<1>(&Z, dense_ndw, Z);

  if (mask != nullptr) {
    // delete columns according to bool mask
    int true_value_count = 0;
    for (int i = 0; i < mask->value_size(); ++i) {
      if (mask->value(i) == true) true_value_count++;
    }

    DenseMatrix<float> masked_Z(Z.no_rows(), true_value_count);
    DenseMatrix<float> masked_Theta(theta_matrix->no_rows(), true_value_count);
    int real_index = 0;
    for (int i = 0; i < mask->value_size(); ++i) {
      if (mask->value(i) == true) {
        for (int j = 0; j < Z.no_rows(); ++j) {
          masked_Z(j, real_index) = Z(j, i);
        }
        for (int j = 0; j < theta_matrix->no_rows(); ++j) {
          masked_Theta(j, real_index) = (*theta_matrix)(j, i);
        }
        real_index++;
      }
    }

    DenseMatrix<float> prod_Z_Theta(masked_Z.no_rows(), masked_Theta.no_rows());
    blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::Trans,
      masked_Z.no_rows(), masked_Theta.no_rows(), masked_Z.no_columns(), 1,
      masked_Z.get_data(), masked_Z.no_columns(), masked_Theta.get_data(),
      masked_Theta.no_columns(), 0, prod_Z_Theta.get_data(), masked_Theta.no_rows());

    ApplyByElement<0>(n_wt.get(), prod_Z_Theta, phi_matrix);
  } else {
    DenseMatrix<float> prod_Z_Theta(Z.no_rows(), theta_matrix->no_rows());
    blas->sgemm(util::Blas::RowMajor, util::Blas::NoTrans, util::Blas::Trans,
      Z.no_rows(), theta_matrix->no_rows(), Z.no_columns(), 1, Z.get_data(),
      Z.no_columns(), theta_matrix->get_data(), theta_matrix->no_columns(), 0,
      prod_Z_Theta.get_data(), theta_matrix->no_rows());

    ApplyByElement<0>(n_wt.get(), prod_Z_Theta, phi_matrix);
  }

  return n_wt;
}

static const DataLoaderCacheEntry* FindCacheEntry(const ProcessorInput& part, const ModelConfig& model_config) {
  for (int i = 0; i < part.cached_theta_size(); ++i) {
    if ((part.cached_theta(i).batch_uuid() == part.batch_uuid()) &&
        (part.cached_theta(i).model_name() == model_config.name())) {
      return &part.cached_theta(i);
    }
  }
  return nullptr;
}

void Processor::FindThetaMatrix(const Batch& batch, const GetThetaMatrixArgs& args,
                                ThetaMatrix* result) {
  util::Blas* blas = util::Blas::mkl();
  if ((blas == nullptr) || !blas->is_loaded())
    blas = util::Blas::builtin();

  std::string model_name = args.model_name();
  std::shared_ptr<const TopicModel> topic_model = merger_.GetLatestTopicModel(model_name);
  if (topic_model == nullptr)
    BOOST_THROW_EXCEPTION(ArgumentOutOfRangeException("Unable to find topic model", model_name));

  std::shared_ptr<InstanceSchema> schema = schema_.get();
  const ModelConfig& model_config = schema->model_config(model_name);

  if (model_config.class_id_size() != model_config.class_weight_size())
    BOOST_THROW_EXCEPTION(InternalError(
    "model.class_id_size() != model.class_weight_size()"));

  int topic_size = topic_model->topic_size();
  if (topic_size != model_config.topics_count())
    BOOST_THROW_EXCEPTION(InternalError(
    "Topics count mismatch between model config and physical model representation"));


  bool use_sparse_bow = model_config.use_sparse_bow();
  std::shared_ptr<CsrMatrix<float>> sparse_ndw = use_sparse_bow ? InitializeSparseNdw(batch, model_config) : nullptr;
  std::shared_ptr<DenseMatrix<float>> dense_ndw = !use_sparse_bow ? InitializeDenseNdw(batch) : nullptr;

  std::shared_ptr<DenseMatrix<float>> theta_matrix = InitializeTheta(batch, model_config, nullptr);

  std::shared_ptr<DenseMatrix<float>> phi_matrix = InitializePhi(batch, model_config, *topic_model);
  if (phi_matrix == nullptr) {
    LOG(INFO) << "Phi is empty, calculations for the model " + model_name +
      "would not be processed on this iteration";
    return;  // return from lambda; goes to next step of std::for_each
  }

  std::shared_ptr<DenseMatrix<float>> n_wt;
  if (model_config.use_sparse_bow()) {
    n_wt = CalculateNwtSparse(model_config, batch, nullptr, *schema_.get(), *sparse_ndw, *phi_matrix,
      theta_matrix.get(), blas);
  } else {
    n_wt = CalculateNwtDense(model_config, batch, nullptr, *schema_.get(), *dense_ndw, *phi_matrix,
      theta_matrix.get(), blas);
  }

  DataLoaderCacheEntry cache_entry;
  cache_entry.set_model_name(model_name);
  cache_entry.mutable_topic_name()->CopyFrom(topic_model->topic_name());
  for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
    const Item& item = batch.item(item_index);
    cache_entry.add_item_id(item.id());
    FloatArray* item_weights = cache_entry.add_theta();
    for (int topic_index = 0; topic_index < topic_size; ++topic_index)
      item_weights->add_value((*theta_matrix)(topic_index, item_index));
  }

  BatchHelpers::PopulateThetaMatrixFromCacheEntry(cache_entry, args, result);
}

void Processor::ThreadFunction() {
  try {
    int total_processed_batches = 0;  // counter

    Helpers::SetThreadName(-1, "Processor thread");
    LOG(INFO) << "Processor thread started";
    int pop_retries = 0;
    const int pop_retries_max = 20;

    util::Blas* blas = util::Blas::mkl();
    if ((blas == nullptr) || !blas->is_loaded()) {
      LOG(INFO) << "Intel Math Kernel Library is not detected, "
          << "using built in implementation (can be slower than MKL)";
      blas = util::Blas::builtin();
    }

    for (;;) {
      if (is_stopping) {
        LOG(INFO) << "Processor thread stopped";
        LOG(INFO) << "Total number of processed batches: " << total_processed_batches;
        break;
      }

      std::shared_ptr<ProcessorInput> part;
      if (!processor_queue_->try_pop(&part)) {
        pop_retries++;
        LOG_IF(INFO, pop_retries == pop_retries_max) << "No data in processing queue, waiting...";

        // Sleep and check for interrupt.
        // To check for interrupt without sleep,
        // use boost::this_thread::interruption_point()
        // which also throws boost::thread_interrupted
        boost::this_thread::sleep(boost::posix_time::milliseconds(kIdleLoopFrequency));

        continue;
      }

      LOG_IF(INFO, pop_retries >= pop_retries_max) << "Processing queue has data, processing started";
      pop_retries = 0;

      CuckooWatch cuckoo("Batch processed in ");  // log time from now to destruction
      total_processed_batches++;

      const Batch& batch = part->batch();

      if (batch.class_id_size() != batch.token_size())
        BOOST_THROW_EXCEPTION(InternalError(
            "batch.class_id_size() != batch.token_size()"));

      std::shared_ptr<InstanceSchema> schema = schema_.get();
      std::vector<ModelName> model_names = schema->GetModelNames();

      std::shared_ptr<CsrMatrix<float>> sparse_ndw;
      std::shared_ptr<DenseMatrix<float>> dense_ndw;

      std::for_each(model_names.begin(), model_names.end(), [&](ModelName model_name) {
        const ModelConfig& model_config = schema->model_config(model_name);

        // do not process disabled models.
        if (!model_config.enabled()) return;  // return from lambda; goes to next step of std::for_each

        if (model_config.class_id_size() != model_config.class_weight_size())
          BOOST_THROW_EXCEPTION(InternalError(
              "model.class_id_size() != model.class_weight_size()"));

        std::shared_ptr<const TopicModel> topic_model = merger_.GetLatestTopicModel(model_name);
        assert(topic_model.get() != nullptr);

        int topic_size = topic_model->topic_size();
        if (topic_size != model_config.topics_count())
          BOOST_THROW_EXCEPTION(InternalError(
            "Topics count mismatch between model config and physical model representation"));

        if (model_config.use_sparse_bow())
          sparse_ndw = InitializeSparseNdw(batch, model_config);

        if (!model_config.use_sparse_bow() && (dense_ndw == nullptr)) {
          // dense_ndw does not depend on model_config => used the same dense_ndw for all models with !use_sparse_bow
          dense_ndw = InitializeDenseNdw(batch);
        }

        const DataLoaderCacheEntry* cache = FindCacheEntry(*part, model_config);
        std::shared_ptr<DenseMatrix<float>> theta_matrix = InitializeTheta(batch, model_config, cache);

        std::shared_ptr<ModelIncrement> model_increment = InitializeModelIncrement(*part, model_config, *topic_model);
        call_on_destruction c([&]() { merger_queue_->push(model_increment); });

        std::shared_ptr<DenseMatrix<float>> phi_matrix = InitializePhi(batch, model_config, *topic_model);
        if (phi_matrix == nullptr) {
          LOG(INFO) << "Phi is empty, calculations for the model " + model_name +
            "would not be processed on this iteration";
          return;  // return from lambda; goes to next step of std::for_each
        }

        // Find and save to the variable the index of model stream in the part->stream_name() list.
        int model_stream_index = repeated_field_index_of(part->stream_name(), model_config.stream_name());
        const Mask* stream_mask = (model_stream_index != -1) ? &part->stream_mask(model_stream_index) : nullptr;

        std::shared_ptr<DenseMatrix<float>> n_wt;
        if (model_config.use_sparse_bow()) {
          n_wt = CalculateNwtSparse(model_config, batch, stream_mask, *schema_.get(), *sparse_ndw, *phi_matrix,
                                    theta_matrix.get(), blas);
        } else {
          n_wt = CalculateNwtDense(model_config, batch, stream_mask, *schema_.get(), *dense_ndw, *phi_matrix,
                                   theta_matrix.get(), blas);
        }

        for (int token_index = 0; token_index < n_wt->no_rows(); ++token_index) {
          FloatArray* hat_n_wt_cur = model_increment->mutable_token_increment(token_index);

          if (hat_n_wt_cur->value_size() == 0)
            continue;

          if (hat_n_wt_cur->value_size() != topic_size)
            BOOST_THROW_EXCEPTION(InternalError("hat_n_wt_cur->value_size() != topic_size"));

          if (model_increment->operation_type(token_index) ==
              ModelIncrement_OperationType_IncrementValue) {
            for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
              float value = (*n_wt)(token_index, topic_index);
              hat_n_wt_cur->set_value(topic_index, value);
            }
          }
        }

        if (schema->config().cache_theta()) {
          // Update theta cache
          DataLoaderCacheEntry new_cache_entry;
          new_cache_entry.set_batch_uuid(part->batch_uuid());
          new_cache_entry.set_model_name(model_name);
          new_cache_entry.mutable_topic_name()->CopyFrom(model_increment->topic_name());
          for (int item_index = 0; item_index < batch.item_size(); ++item_index) {
            new_cache_entry.add_item_id(batch.item(item_index).id());
            FloatArray* cached_theta = new_cache_entry.add_theta();
            for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
              cached_theta->add_value((*theta_matrix)(topic_index, item_index));
            }
          }

          if (schema->config().has_disk_cache_path()) {
            std::string disk_cache_path = schema->config().disk_cache_path();
            boost::uuids::uuid uuid = boost::uuids::random_generator()();
            fs::path file(boost::lexical_cast<std::string>(uuid) + ".cache");
            try {
              BatchHelpers::SaveMessage(file.string(), disk_cache_path, new_cache_entry);
              new_cache_entry.set_filename((fs::path(disk_cache_path) / file).string());
              new_cache_entry.clear_theta();
              new_cache_entry.clear_item_id();
            } catch (...) {
              LOG(ERROR) << "Unable to save cache entry to " << schema->config().disk_cache_path();
            }
          }

          model_increment->add_cache()->CopyFrom(new_cache_entry);
        }

        std::map<ScoreName, std::shared_ptr<Score>> score_container;
        for (int score_index = 0; score_index < model_config.score_name_size(); ++score_index) {
          const ScoreName& score_name = model_config.score_name(score_index);
          auto score_calc = schema->score_calculator(score_name);
          if (score_calc == nullptr) {
            LOG(ERROR) << "Unable to find score calculator '" << score_name << "', referenced by "
              << "model " << model_config.name() << ".";
            continue;
          }

          if (!score_calc->is_cumulative())
            continue;  // skip all non-cumulative scores

          score_container.insert(std::make_pair(score_name, score_calc->CreateScore()));
        }

        std::vector<Token> token_dict;
        for (int token_index = 0; token_index < batch.token_size(); ++token_index) {
          token_dict.push_back(Token(batch.class_id(token_index), batch.token(token_index)));
        }

        StreamIterator iter(*part);
        while (iter.Next() != nullptr) {
          const Item* item = iter.Current();

          // Calculate all requested scores (such as perplexity)
          for (auto score_iter = score_container.begin();
               score_iter != score_container.end();
               ++score_iter) {
            const ScoreName& score_name = score_iter->first;
            std::shared_ptr<Score> score = score_iter->second;
            auto score_calc = schema->score_calculator(score_name);

            if (!iter.InStream(score_calc->stream_name())) continue;

            int item_index = iter.item_index();
            std::vector<float> theta_vec;
            for (int topic_index = 0; topic_index < topic_size; ++topic_index) {
              theta_vec.push_back((*theta_matrix)(topic_index, item_index));
            }
            score_calc->AppendScore(*item, token_dict, *topic_model, theta_vec, score.get());
          }
        }

        for (auto score_iter = score_container.begin();
             score_iter != score_container.end();
             ++score_iter) {
          model_increment->add_score_name(score_iter->first);
          model_increment->add_score(score_iter->second->SerializeAsString());
        }
      });

      // Wait until merger queue has space for a new element
      int merger_queue_max_size = schema_.get()->config().merger_queue_max_size();

      int push_retries = 0;
      const int push_retries_max = 50;

      for (;;) {
        if (merger_queue_->size() < merger_queue_max_size)
          break;

        push_retries++;
        LOG_IF(WARNING, push_retries == push_retries_max) << "Merger queue is full, waiting...";
        boost::this_thread::sleep(boost::posix_time::milliseconds(kIdleLoopFrequency));
      }

      LOG_IF(WARNING, push_retries >= push_retries_max) << "Merger queue is healthy again";

      // Here call_in_destruction will enqueue processor output into the merger queue.
    }
  }
  catch (boost::thread_interrupted&) {
    LOG(WARNING) << "thread_interrupted exception in Processor::ThreadFunction() function";
    return;
  }
  catch (std::runtime_error& ex) {
    LOG(ERROR) << ex.what();
    throw;
  } catch(...) {
    LOG(FATAL) << "Fatal exception in Processor::ThreadFunction() function";
    throw;
  }
}

}  // namespace core
}  // namespace artm