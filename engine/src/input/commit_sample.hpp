#pragma once

#include "input/input_processor.hpp"

namespace llavon::ime {

// Captures the text actually submitted, never a speculative preview. Keeping
// collection assembly separate prevents recording policy from changing keys.
std::optional<InputEffect::CommitSample> commit_training_sample(const InputSession& session,
                                                               std::u16string_view committed);

}  // namespace llavon::ime
