#include "test_suites.h"

#include <cstdlib>
#include <string>
#include <vector>

#include "input/prediction_state.hpp"

int run_prediction_state_tests() {
    using llavon::ime::PredictionState;
    using llavon::ime::protocol::Error;
    using llavon::ime::protocol::ErrorCode;
    using llavon::ime::protocol::Prediction;
    using llavon::ime::protocol::SessionId;

    bool ok = true;

    const auto make_session = [](std::uint8_t tag) {
        SessionId id{};
        id[0] = tag;
        return id;
    };
    const SessionId session = make_session(0x11);
    const SessionId other_session = make_session(0x22);

    PredictionState state;
    ok = ok && !state.session_open();
    ok = ok && state.next_request_id == 1 && state.generation == 0;
    ok = ok && !state.pending && !state.dirty;
    ok = ok && !state.correlates(Prediction{}) && state.correlates(Error{});

    // A request only starts when there is a completed segment and nothing is
    // already in flight.
    ok = ok && !state.begin({}, u"ㄋㄧˇ", 3) && !state.pending;
    ok = ok && state.begin({0}, u"ㄋㄧˇ", 3);
    ok = ok && state.pending && !state.dirty && state.segment_indices == std::vector<std::size_t>{0};
    ok = ok && state.key == u"ㄋㄧˇ" && state.revision == 3;

    // A second request while one is in flight only marks it dirty.
    ok = ok && !state.begin({0, 1}, u"ㄋㄧˇㄏㄠˇ", 4);
    ok = ok && state.pending && state.dirty;
    ok = ok && state.segment_indices == std::vector<std::size_t>{0};

    // Correlation relies on the in-flight request id, revision, and session.
    state.session_id = session;
    state.inflight_request_id = 7;
    state.inflight_revision = 3;
    Prediction prediction;
    prediction.session_id = session;
    prediction.request_id = 7;
    prediction.buffer_revision = 3;
    prediction.candidates = {{U'你'}};
    ok = ok && state.correlates(prediction);
    ok = ok && state.matches_composition(prediction, u"ㄋㄧˇ", 3);
    ok = ok && !state.matches_composition(prediction, u"ㄋㄧˇ", 4);
    ok = ok && !state.matches_composition(prediction, u"ㄋㄧ", 3);
    Prediction stale_count = prediction;
    stale_count.candidates = {{U'你'}, {U'好'}};
    ok = ok && state.correlates(stale_count) && !state.matches_composition(stale_count, u"ㄋㄧˇ", 3);
    Prediction wrong_request = prediction;
    wrong_request.request_id = 8;
    ok = ok && !state.correlates(wrong_request);
    Prediction wrong_revision = prediction;
    wrong_revision.buffer_revision = 4;
    ok = ok && !state.correlates(wrong_revision);
    Prediction wrong_session = prediction;
    wrong_session.session_id = other_session;
    ok = ok && !state.correlates(wrong_session);

    // Errors correlate by request id, with 0 meaning "not request specific".
    Error error;
    error.code = ErrorCode::ModelError;
    ok = ok && state.correlates(error);
    error.request_id = 7;
    ok = ok && state.correlates(error);
    error.request_id = 8;
    ok = ok && !state.correlates(error);

    // Finishing reports the dirty flag and clears the in-flight bookkeeping.
    ok = ok && state.finish();
    ok = ok && !state.pending && !state.dirty;
    ok = ok && !state.inflight_request_id.has_value() && state.inflight_revision == 0;
    ok = ok && state.segment_indices.empty();
    ok = ok && !state.correlates(prediction);
    ok = ok && state.correlates(error);
    ok = ok && !state.finish();

    // Dirty marking only applies while a request is in flight.
    state.mark_dirty();
    ok = ok && !state.dirty;
    ok = ok && state.begin({1}, u"ㄏㄠˇ", 9);
    state.mark_dirty();
    ok = ok && state.dirty;

    // Invalidation bumps the generation and drops everything in flight.
    const auto generation = state.generation;
    state.session_id = session;
    state.invalidate();
    ok = ok && state.generation == generation + 1;
    ok = ok && !state.pending && !state.dirty && !state.inflight_request_id.has_value();
    ok = ok && state.segment_indices.empty();
    ok = ok && state.session_open();

    state.next_request_id = 42;
    ok = ok && state.session_open() && state.next_request_id == 42;

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
