// Copyright 2018, Additive Regularization of Topic Models.

#include "artm/core/processor_transaction_helpers.h"

namespace artm {
namespace core {

namespace {
const double kTransactionsEps = 1e-100;
}

inline double ProcessorTransactionHelpers::ComputePtdx(const Item& item,
                                                       float init_value,
                                                       int start_index,
                                                       int end_index,
                                                       int topic_id,
                                                       const std::vector<int>& local_token_id_to_global_id,
                                                       const ::artm::core::PhiMatrix& p_wt) {
  double pre_p_dx_val = init_value;

  for (int token_id = start_index; token_id < end_index; ++token_id) {
    int local_id = item.token_id(token_id);
    pre_p_dx_val *= p_wt.get(local_token_id_to_global_id[local_id], topic_id);
  }
  return pre_p_dx_val;
}

void ProcessorTransactionHelpers::TransactionInferThetaAndUpdateNwtSparse(
                                     const ProcessBatchesArgs& args,
                                     const Batch& batch,
                                     float batch_weight,
                                     const ::artm::core::PhiMatrix& p_wt,
                                     const RegularizeThetaAgentCollection& theta_agents,
                                     LocalThetaMatrix<float>* theta_matrix,
                                     NwtWriteAdapter* nwt_writer,
                                     ThetaMatrix* new_cache_entry_ptr) {
  if (!args.opt_for_avx()) {
    LOG(WARNING) << "Current version of BigARTM doesn't support 'opt_for_avx' == false"
      << " with complex transactions, option 'opt_for_avx' will be ignored";
  }

  const int num_topics = p_wt.topic_size();
  const int docs_count = theta_matrix->num_items();

  std::vector<double> helper_vector(num_topics, 0.0f);

  LocalThetaMatrix<float> n_td(num_topics, docs_count);
  LocalThetaMatrix<float> r_td(num_topics, 1);

  std::vector<int> local_token_id_to_global_id(batch.token_size(), -1);
  for (int local_id = 0; local_id < batch.token_size(); ++local_id) {
    int global_id = p_wt.token_index({ batch.class_id(local_id), batch.token(local_id) });
    if (global_id == -1) {
      continue;
    }
    local_token_id_to_global_id[local_id] = global_id;
  }

  bool use_transaction_weight = false;
  std::unordered_map<TransactionTypeName, float> tt_name_to_weight;
  if (args.transaction_typename_size() > 0) {
    use_transaction_weight = true;
    for (int i = 0; i < args.transaction_typename_size(); ++i) {
      tt_name_to_weight.emplace(args.transaction_typename(i), args.transaction_weight(i));
    }
  }

  for (int d = 0; d < docs_count; ++d) {
    float *ntd_ptr = &n_td(0, d);
    float *theta_ptr = &(*theta_matrix)(0, d);  // NOLINT

    const auto &item = batch.item(d);

    for (int inner_iter = 0; inner_iter <= args.num_document_passes(); ++inner_iter) {
      for (int k = 0; k < num_topics; ++k) {
        ntd_ptr[k] = 0.0f;
      }

      for (int t_index = 0; t_index < item.transaction_start_index_size() - 1; ++t_index) {
        const int start_index = item.transaction_start_index(t_index);
        const int end_index = item.transaction_start_index(t_index + 1);
        double n_kdx = item.token_weight(start_index);

        const TransactionTypeName &tt_name = batch.transaction_typename(item.transaction_typename_id(t_index));
        float tt_weight = 1.0f;
        if (use_transaction_weight) {
          auto iter = tt_name_to_weight.find(tt_name);
          tt_weight = (iter == tt_name_to_weight.end()) ? 0.0f : iter->second;
        }

        double p_dx_val = 0.0;
        for (int k = 0; k < num_topics; ++k) {
          helper_vector[k] = ComputePtdx(item, theta_ptr[k], start_index,
                                         end_index, k, local_token_id_to_global_id, p_wt);
          p_dx_val += helper_vector[k];
        }

        if (isZero(p_dx_val, kTransactionsEps)) {
          continue;
        }

        for (int k = 0; k < num_topics; ++k) {
          ntd_ptr[k] += (tt_weight * n_kdx * helper_vector[k] / p_dx_val);
        }
      }

      for (int k = 0; k < num_topics; ++k) {
        theta_ptr[k] = ntd_ptr[k];
      }

      r_td.InitializeZeros();
      theta_agents.Apply(d, inner_iter, num_topics, theta_ptr, r_td.get_data());
    }
  }

  ProcessorHelpers::CreateThetaCacheEntry(new_cache_entry_ptr, theta_matrix, batch, p_wt, args);

  if (nwt_writer == nullptr) {
    return;
  }

  for (int d = 0; d < docs_count; ++d) {
    const auto &item = batch.item(d);

    for (int t_index = 0; t_index < item.transaction_start_index_size() - 1; ++t_index) {
      const int start_index = item.transaction_start_index(t_index);
      const int end_index = item.transaction_start_index(t_index + 1);
      const double n_kdx = item.token_weight(start_index);

      const TransactionTypeName &tt_name = batch.transaction_typename(item.transaction_typename_id(t_index));
      float tt_weight = 1.0f;
      if (use_transaction_weight) {
        auto iter = tt_name_to_weight.find(tt_name);
        tt_weight = (iter == tt_name_to_weight.end()) ? 0.0f : iter->second;
      }

      double p_dx_val = 0.0;
      for (int k = 0; k < num_topics; ++k) {
        helper_vector[k] = ComputePtdx(item, (*theta_matrix)(k, d), start_index,
                                       end_index, k, local_token_id_to_global_id, p_wt);
        p_dx_val += helper_vector[k];
      }

      std::vector<float> values(num_topics, 0.0f);
      for (int k = 0; k < num_topics; ++k) {
        values[k] = tt_weight * helper_vector[k] * n_kdx * batch_weight / p_dx_val;
      }

      for (int token_id = start_index; token_id < end_index; ++token_id) {
        auto global_id = local_token_id_to_global_id[item.token_id(token_id)];

        nwt_writer->Store(global_id, values);
      }
    }
  }
}



}  // namespace core
}  // namespace artm
