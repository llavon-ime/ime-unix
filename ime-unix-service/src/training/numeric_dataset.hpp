#pragma once

#include "training/commit_crypto.hpp"

#include <filesystem>
#include <string>
#include <unordered_set>
#include <vector>

struct sqlite3;

namespace ime::unix_service {

struct NumericDataset {
    std::vector<std::string> included_ids;
    // Written JSONL lines: one per record, three for a manually selected one.
    std::size_t samples = 0;
    std::size_t skipped = 0;
    // Records the conversion refused; they stay pending and would be skipped
    // again, so the manager can mark them in the review list.
    std::vector<std::string> skipped_ids;
    int pad_token_id = 0;
    int vocab_size = 0;
    int max_sequence_length = 0;
};

NumericDataset write_numeric_dataset(sqlite3* db, const std::filesystem::path& tables,
                                      const std::filesystem::path& model_config,
                                      const std::filesystem::path& output, int max_sequence_length,
                                      const std::unordered_set<std::string>* selected_ids = nullptr,
                                      const commit_crypto::Decryption* decryption = nullptr,
                                      bool manual_only = false);

}  // namespace ime::unix_service
